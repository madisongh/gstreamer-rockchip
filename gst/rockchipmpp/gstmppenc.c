/*
 * Copyright 2017 Rockchip Electronics Co., Ltd
 *     Author: Randy Li <randy.li@rock-chips.com>
 *
 * Copyright 2021 Rockchip Electronics Co., Ltd
 *     Author: Jeffy Chen <jeffy.chen@rock-chips.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include "gstmppallocator.h"
#include "gstmppenc.h"

#define GST_CAT_DEFAULT mpp_enc_debug
GST_DEBUG_CATEGORY (GST_CAT_DEFAULT);

#define parent_class gst_mpp_enc_parent_class
G_DEFINE_ABSTRACT_TYPE (GstMppEnc, gst_mpp_enc, GST_TYPE_VIDEO_ENCODER);

#define GST_MPP_ENC_TASK_STARTED(encoder) \
    (gst_pad_get_task_state ((encoder)->srcpad) == GST_TASK_STARTED)

#define GST_MPP_ENC_MUTEX(encoder) (&GST_MPP_ENC (encoder)->mutex)

#define GST_MPP_ENC_LOCK(encoder) \
  GST_VIDEO_ENCODER_STREAM_UNLOCK (encoder); \
  g_mutex_lock (GST_MPP_ENC_MUTEX (encoder)); \
  GST_VIDEO_ENCODER_STREAM_LOCK (encoder);

#define GST_MPP_ENC_UNLOCK(encoder) \
  g_mutex_unlock (GST_MPP_ENC_MUTEX (encoder));

#define GST_MPP_ENC_EVENT_MUTEX(encoder) (&GST_MPP_ENC (encoder)->event_mutex)
#define GST_MPP_ENC_EVENT_COND(encoder) (&GST_MPP_ENC (encoder)->event_cond)

#define GST_MPP_ENC_BROADCAST(encoder) \
  g_mutex_lock (GST_MPP_ENC_EVENT_MUTEX (encoder)); \
  g_cond_broadcast (GST_MPP_ENC_EVENT_COND (encoder)); \
  g_mutex_unlock (GST_MPP_ENC_EVENT_MUTEX (encoder));

#define GST_MPP_ENC_WAIT(encoder, condition) \
  g_mutex_lock (GST_MPP_ENC_EVENT_MUTEX (encoder)); \
  while (!(condition)) \
    g_cond_wait (GST_MPP_ENC_EVENT_COND (encoder), \
        GST_MPP_ENC_EVENT_MUTEX (encoder)); \
  g_mutex_unlock (GST_MPP_ENC_EVENT_MUTEX (encoder));

#define DEFAULT_PROP_HEADER_MODE MPP_ENC_HEADER_MODE_DEFAULT    /* First frame */
#define DEFAULT_PROP_SEI_MODE MPP_ENC_SEI_MODE_DISABLE
#define DEFAULT_PROP_RC_MODE MPP_ENC_RC_MODE_CBR
#define DEFAULT_PROP_ROTATION 0
#define DEFAULT_PROP_GOP -1     /* Same as FPS */
#define DEFAULT_PROP_MAX_REENC 1
#define DEFAULT_PROP_BPS 0      /* Auto */
#define DEFAULT_PROP_BPS_MIN 0  /* Auto */
#define DEFAULT_PROP_BPS_MAX 0  /* Auto */
#define DEFAULT_PROP_WIDTH 0    /* Original */
#define DEFAULT_PROP_HEIGHT 0   /* Original */
#define DEFAULT_PROP_ZERO_COPY_PKT TRUE

/* Input isn't ARM AFBC by default */
static GstVideoFormat DEFAULT_PROP_ARM_AFBC = FALSE;

#define MPP_MAX_PENDING 16      /* Max number of MPP pending frames */
static guint32 DEFAULT_PROP_MAX_PENDING = MPP_MAX_PENDING;

#define DEFAULT_FPS 30

enum
{
  PROP_0,
  PROP_MAX_PENDING,
  PROP_HEADER_MODE,
  PROP_RC_MODE,
  PROP_ROTATION,
  PROP_SEI_MODE,
  PROP_GOP,
  PROP_MAX_REENC,
  PROP_BPS,
  PROP_BPS_MIN,
  PROP_BPS_MAX,
  PROP_WIDTH,
  PROP_HEIGHT,
  PROP_ZERO_COPY_PKT,
  PROP_ARM_AFBC,
  PROP_LAST,
};

static const MppFrameFormat gst_mpp_enc_formats[] = {
  MPP_FMT_YUV420SP,
  MPP_FMT_YUV420P,
  MPP_FMT_YUV422_YUYV,
  MPP_FMT_YUV422_UYVY,
  MPP_FMT_YUV444SP,
  MPP_FMT_YUV444P,
  MPP_FMT_RGB565LE,
  MPP_FMT_BGR565LE,
  MPP_FMT_RGB888,
  MPP_FMT_BGR888,
  MPP_FMT_ARGB8888,
  MPP_FMT_ABGR8888,
  MPP_FMT_RGBA8888,
  MPP_FMT_BGRA8888,
};

static gboolean
gst_mpp_enc_format_supported (MppFrameFormat format)
{
  guint i;

  for (i = 0; i < ARRAY_SIZE (gst_mpp_enc_formats); i++) {
    if (format == gst_mpp_enc_formats[i])
      return TRUE;
  }

  return FALSE;
}

gboolean
gst_mpp_enc_supported (MppCodingType mpp_type)
{
  MppCtx mpp_ctx;
  MppApi *mpi;

  if (mpp_create (&mpp_ctx, &mpi))
    return FALSE;

  if (mpp_init (mpp_ctx, MPP_CTX_ENC, mpp_type)) {
    mpp_destroy (mpp_ctx);
    return FALSE;
  }

  mpp_destroy (mpp_ctx);
  return TRUE;
}

gboolean
gst_mpp_enc_video_info_align (GstVideoInfo * info)
{
  gint vstride = 0;

  /* Allow skipping vstride aligning for RKVENC */
  if (g_getenv ("GST_MPP_ENC_UNALIGNED_VSTRIDE"))
    vstride = GST_MPP_VIDEO_INFO_VSTRIDE (info);

  return gst_mpp_video_info_align (info, 0, vstride);
}

static void
gst_mpp_enc_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstVideoEncoder *encoder = GST_VIDEO_ENCODER (object);
  GstMppEnc *self = GST_MPP_ENC (encoder);

  switch (prop_id) {
    case PROP_MAX_PENDING:{
      self->max_pending = g_value_get_uint (value);
      GST_MPP_ENC_BROADCAST (encoder);
      return;
    }
    case PROP_HEADER_MODE:{
      MppEncHeaderMode header_mode = g_value_get_enum (value);
      if (self->header_mode == header_mode)
        return;

      self->header_mode = header_mode;
      break;
    }
    case PROP_SEI_MODE:{
      MppEncSeiMode sei_mode = g_value_get_enum (value);
      if (self->sei_mode == sei_mode)
        return;

      self->sei_mode = sei_mode;
      break;
    }
    case PROP_RC_MODE:{
      MppEncRcMode rc_mode = g_value_get_enum (value);
      if (self->rc_mode == rc_mode)
        return;

      self->rc_mode = rc_mode;
      break;
    }
    case PROP_GOP:{
      gint gop = g_value_get_int (value);
      if (self->gop == gop)
        return;

      self->gop = gop;
      break;
    }
    case PROP_MAX_REENC:{
      guint max_reenc = g_value_get_uint (value);
      if (self->max_reenc == max_reenc)
        return;

      self->max_reenc = max_reenc;
      break;
    }
    case PROP_BPS:{
      guint bps = g_value_get_uint (value);
      if (self->bps == bps)
        return;

      self->bps = bps;
      break;
    }
    case PROP_BPS_MIN:{
      guint bps_min = g_value_get_uint (value);
      if (self->bps_min == bps_min)
        return;

      self->bps_min = bps_min;
      break;
    }
    case PROP_BPS_MAX:{
      guint bps_max = g_value_get_uint (value);
      if (self->bps_max == bps_max)
        return;

      self->bps_max = bps_max;
      break;
    }
    case PROP_ROTATION:{
      if (self->input_state)
        GST_WARNING_OBJECT (encoder, "unable to change rotation");
      else
        self->rotation = g_value_get_enum (value);
      return;
    }
    case PROP_WIDTH:{
      if (self->input_state)
        GST_WARNING_OBJECT (encoder, "unable to change width");
      else
        self->width = g_value_get_uint (value);
      return;
    }
    case PROP_HEIGHT:{
      if (self->input_state)
        GST_WARNING_OBJECT (encoder, "unable to change height");
      else
        self->height = g_value_get_uint (value);
      return;
    }
    case PROP_ZERO_COPY_PKT:{
      self->zero_copy_pkt = g_value_get_boolean (value);
      return;
    }
    case PROP_ARM_AFBC:{
      if (self->input_state)
        GST_WARNING_OBJECT (encoder, "unable to change ARM AFBC");
      else
        self->arm_afbc = g_value_get_boolean (value);
      return;
    }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      return;
  }

  self->prop_dirty = TRUE;
}

static void
gst_mpp_enc_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstVideoEncoder *encoder = GST_VIDEO_ENCODER (object);
  GstMppEnc *self = GST_MPP_ENC (encoder);

  switch (prop_id) {
    case PROP_MAX_PENDING:
      g_value_set_uint (value, self->max_pending);
      break;
    case PROP_HEADER_MODE:
      g_value_set_enum (value, self->header_mode);
      break;
    case PROP_SEI_MODE:
      g_value_set_enum (value, self->sei_mode);
      break;
    case PROP_RC_MODE:
      g_value_set_enum (value, self->rc_mode);
      break;
    case PROP_ROTATION:
      g_value_set_enum (value, self->rotation);
      break;
    case PROP_GOP:
      g_value_set_int (value, self->gop);
      break;
    case PROP_MAX_REENC:
      g_value_set_uint (value, self->max_reenc);
      break;
    case PROP_BPS:
      g_value_set_uint (value, self->bps);
      break;
    case PROP_BPS_MIN:
      g_value_set_uint (value, self->bps_min);
      break;
    case PROP_BPS_MAX:
      g_value_set_uint (value, self->bps_max);
      break;
    case PROP_WIDTH:
      g_value_set_uint (value, self->width);
      break;
    case PROP_HEIGHT:
      g_value_set_uint (value, self->height);
      break;
    case PROP_ZERO_COPY_PKT:
      g_value_set_boolean (value, self->zero_copy_pkt);
      break;
    case PROP_ARM_AFBC:
      g_value_set_boolean (value, self->arm_afbc);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      return;
  }
}

gboolean
gst_mpp_enc_apply_properties (GstVideoEncoder * encoder)
{
  GstMppEnc *self = GST_MPP_ENC (encoder);
  GstVideoInfo *info = &self->info;
  gint fps = GST_VIDEO_INFO_FPS_N (info) / GST_VIDEO_INFO_FPS_D (info);

  if (!self->prop_dirty)
    return TRUE;

  self->prop_dirty = FALSE;

  if (self->mpi->control (self->mpp_ctx, MPP_ENC_SET_SEI_CFG, &self->sei_mode))
    GST_WARNING_OBJECT (self, "failed to set sei mode");

  if (self->mpi->control (self->mpp_ctx, MPP_ENC_SET_HEADER_MODE,
          &self->header_mode))
    GST_WARNING_OBJECT (self, "failed to set header mode");

  mpp_enc_cfg_set_s32 (self->mpp_cfg, "rc:gop",
      self->gop < 0 ? fps : self->gop);
  mpp_enc_cfg_set_u32 (self->mpp_cfg, "rc:max_reenc_times", self->max_reenc);
  mpp_enc_cfg_set_s32 (self->mpp_cfg, "rc:mode", self->rc_mode);

  if (!self->bps)
    self->bps =
        GST_VIDEO_INFO_WIDTH (info) * GST_VIDEO_INFO_HEIGHT (info) / 8 * fps;

  if (!self->bps || self->rc_mode == MPP_ENC_RC_MODE_FIXQP) {
    /* BPS settings are ignored */
  } else if (self->rc_mode == MPP_ENC_RC_MODE_CBR) {
    /* CBR mode has narrow bound */
    mpp_enc_cfg_set_s32 (self->mpp_cfg, "rc:bps_target", self->bps);
    mpp_enc_cfg_set_s32 (self->mpp_cfg, "rc:bps_max",
        self->bps_max ? : self->bps * 17 / 16);
    mpp_enc_cfg_set_s32 (self->mpp_cfg, "rc:bps_min",
        self->bps_min ? : self->bps * 15 / 16);
  } else {
    /* MPP_ENC_RC_MODE_VBR/MPP_ENC_RC_MODE_AVBR */
    /* VBR mode has wide bound */
    mpp_enc_cfg_set_s32 (self->mpp_cfg, "rc:bps_target", self->bps);
    mpp_enc_cfg_set_s32 (self->mpp_cfg, "rc:bps_max",
        self->bps_max ? : self->bps * 17 / 16);
    mpp_enc_cfg_set_s32 (self->mpp_cfg, "rc:bps_min",
        self->bps_min ? : self->bps * 1 / 16);
  }

  if (self->mpi->control (self->mpp_ctx, MPP_ENC_SET_CFG, self->mpp_cfg)) {
    GST_WARNING_OBJECT (self, "failed to set enc cfg");
    return FALSE;
  }

  return TRUE;
}

gboolean
gst_mpp_enc_set_src_caps (GstVideoEncoder * encoder, GstCaps * caps)
{
  GstMppEnc *self = GST_MPP_ENC (encoder);
  GstVideoInfo *info = &self->info;
  GstVideoCodecState *output_state;

  gst_caps_set_simple (caps,
      "width", G_TYPE_INT, GST_VIDEO_INFO_WIDTH (info),
      "height", G_TYPE_INT, GST_VIDEO_INFO_HEIGHT (info), NULL);

  GST_DEBUG_OBJECT (self, "output caps: %" GST_PTR_FORMAT, caps);

  output_state = gst_video_encoder_set_output_state (encoder,
      caps, self->input_state);

  GST_VIDEO_INFO_WIDTH (&output_state->info) = GST_VIDEO_INFO_WIDTH (info);
  GST_VIDEO_INFO_HEIGHT (&output_state->info) = GST_VIDEO_INFO_HEIGHT (info);
  gst_video_codec_state_unref (output_state);

  return gst_video_encoder_negotiate (encoder);
}

static void
gst_mpp_enc_stop_task (GstVideoEncoder * encoder, gboolean drain)
{
  GstMppEnc *self = GST_MPP_ENC (encoder);
  GstTask *task = encoder->srcpad->task;

  if (!GST_MPP_ENC_TASK_STARTED (encoder))
    return;

  GST_DEBUG_OBJECT (self, "stopping encoding thread");

  /* Discard pending frames */
  if (!drain)
    self->pending_frames = 0;

  GST_MPP_ENC_BROADCAST (encoder);

  GST_VIDEO_ENCODER_STREAM_UNLOCK (encoder);
  /* Wait for task thread to pause */
  if (task) {
    GST_OBJECT_LOCK (task);
    while (GST_TASK_STATE (task) == GST_TASK_STARTED)
      GST_TASK_WAIT (task);
    GST_OBJECT_UNLOCK (task);
  }

  gst_pad_stop_task (encoder->srcpad);
  GST_VIDEO_ENCODER_STREAM_LOCK (encoder);
}

static void
gst_mpp_enc_reset (GstVideoEncoder * encoder, gboolean drain, gboolean final)
{
  GstMppEnc *self = GST_MPP_ENC (encoder);

  GST_MPP_ENC_LOCK (encoder);

  GST_DEBUG_OBJECT (self, "resetting");

  self->flushing = TRUE;
  self->draining = drain;

  /* HACK: The MPP is not capable of handling resets properly. */
  self->draining = TRUE;

  gst_mpp_enc_stop_task (encoder, self->draining);

  self->flushing = final;
  self->draining = FALSE;

  self->mpi->reset (self->mpp_ctx);
  self->task_ret = GST_FLOW_OK;
  self->pending_frames = 0;

  if (self->frames) {
    g_list_free (self->frames);
    self->frames = NULL;
  }

  /* Force re-apply prop */
  self->prop_dirty = TRUE;

  GST_MPP_ENC_UNLOCK (encoder);
}

static gboolean
gst_mpp_enc_start (GstVideoEncoder * encoder)
{
  GstMppEnc *self = GST_MPP_ENC (encoder);
  MppPollType timeout;

  GST_DEBUG_OBJECT (self, "starting");

  gst_video_info_init (&self->info);

  self->allocator = gst_mpp_allocator_new ();
  if (!self->allocator)
    return FALSE;

  gst_mpp_allocator_set_cacheable (self->allocator, FALSE);

  if (mpp_create (&self->mpp_ctx, &self->mpi))
    goto err_unref_alloc;

  timeout = MPP_POLL_NON_BLOCK;
  if (self->mpi->control (self->mpp_ctx, MPP_SET_INPUT_TIMEOUT, &timeout))
    goto err_destroy_mpp;

  /* 1ms timeout for polling */
  timeout = 1;
  if (self->mpi->control (self->mpp_ctx, MPP_SET_OUTPUT_TIMEOUT, &timeout))
    goto err_destroy_mpp;

  if (mpp_init (self->mpp_ctx, MPP_CTX_ENC, self->mpp_type))
    goto err_destroy_mpp;

  if (mpp_frame_init (&self->mpp_frame))
    goto err_destroy_mpp;

  if (mpp_enc_cfg_init (&self->mpp_cfg))
    goto err_deinit_frame;

  if (self->mpi->control (self->mpp_ctx, MPP_ENC_GET_CFG, self->mpp_cfg))
    goto err_deinit_cfg;

  self->task_ret = GST_FLOW_OK;
  self->input_state = NULL;
  self->flushing = FALSE;
  self->pending_frames = 0;
  self->frames = NULL;
  self->required_keyframe_number = 0;

  g_mutex_init (&self->mutex);

  g_mutex_init (&self->event_mutex);
  g_cond_init (&self->event_cond);

  GST_DEBUG_OBJECT (self, "started");

  return TRUE;

err_deinit_cfg:
  mpp_enc_cfg_deinit (self->mpp_cfg);
err_deinit_frame:
  mpp_frame_deinit (&self->mpp_frame);
err_destroy_mpp:
  mpp_destroy (self->mpp_ctx);
err_unref_alloc:
  gst_object_unref (self->allocator);
  return FALSE;
}

static gboolean
gst_mpp_enc_stop (GstVideoEncoder * encoder)
{
  GstMppEnc *self = GST_MPP_ENC (encoder);

  GST_DEBUG_OBJECT (self, "stopping");

  GST_VIDEO_ENCODER_STREAM_LOCK (encoder);
  gst_mpp_enc_reset (encoder, FALSE, TRUE);
  GST_VIDEO_ENCODER_STREAM_UNLOCK (encoder);

  g_cond_clear (&self->event_cond);
  g_mutex_clear (&self->event_mutex);

  g_mutex_clear (&self->mutex);

  mpp_enc_cfg_deinit (self->mpp_cfg);
  mpp_frame_set_buffer (self->mpp_frame, NULL);
  mpp_frame_deinit (&self->mpp_frame);
  mpp_destroy (self->mpp_ctx);

  gst_object_unref (self->allocator);

  if (self->input_state)
    gst_video_codec_state_unref (self->input_state);

  GST_DEBUG_OBJECT (self, "stopped");

  return TRUE;
}

static gboolean
gst_mpp_enc_flush (GstVideoEncoder * encoder)
{
  GST_DEBUG_OBJECT (encoder, "flushing");
  gst_mpp_enc_reset (encoder, FALSE, FALSE);
  return TRUE;
}

static gboolean
gst_mpp_enc_finish (GstVideoEncoder * encoder)
{
  GST_DEBUG_OBJECT (encoder, "finishing");
  gst_mpp_enc_reset (encoder, TRUE, FALSE);
  return GST_FLOW_OK;
}

static gboolean
gst_mpp_enc_apply_strides (GstVideoEncoder * encoder, gint hstride,
    gint vstride)
{
  GstMppEnc *self = GST_MPP_ENC (encoder);
  GstVideoInfo *info = &self->info;

  mpp_frame_set_hor_stride (self->mpp_frame, hstride);
  mpp_frame_set_ver_stride (self->mpp_frame, vstride);
  mpp_enc_cfg_set_s32 (self->mpp_cfg, "prep:hor_stride", hstride);
  mpp_enc_cfg_set_s32 (self->mpp_cfg, "prep:ver_stride", vstride);

  if (hstride == GST_MPP_VIDEO_INFO_HSTRIDE (info) &&
      vstride == GST_MPP_VIDEO_INFO_VSTRIDE (info))
    return TRUE;

  GST_INFO_OBJECT (self, "strides updated to (%dx%d)", hstride, vstride);

  self->prop_dirty = TRUE;
  return gst_mpp_video_info_align (info, hstride, vstride);
}

static gboolean
gst_mpp_enc_set_format (GstVideoEncoder * encoder, GstVideoCodecState * state)
{
  GstMppEnc *self = GST_MPP_ENC (encoder);
  GstVideoInfo *info = &self->info;
  MppFrameFormat format;
  gint width, height, hstride, vstride;

  GST_DEBUG_OBJECT (self, "setting format: %" GST_PTR_FORMAT, state->caps);

  if (self->input_state) {
    if (gst_caps_is_strictly_equal (self->input_state->caps, state->caps))
      return TRUE;

    gst_mpp_enc_reset (encoder, TRUE, FALSE);

    gst_video_codec_state_unref (self->input_state);
    self->input_state = NULL;
  }

  self->input_state = gst_video_codec_state_ref (state);

  *info = state->info;

  if (!gst_mpp_enc_video_info_align (info))
    return FALSE;

  format = gst_mpp_gst_format_to_mpp_format (GST_VIDEO_INFO_FORMAT (info));
  width = GST_VIDEO_INFO_WIDTH (info);
  height = GST_VIDEO_INFO_HEIGHT (info);

  if (self->rotation % 180)
    SWAP (width, height);

  width = self->width ? : width;
  height = self->height ? : height;

  /*
   * Check for conversion
   * NOTE: Not checking the strides here, since they might not be the actual
   * ones (could be overrided by video-meta)
   */
  if (self->rotation || !gst_mpp_enc_format_supported (format) ||
      width != GST_VIDEO_INFO_WIDTH (info) ||
      height != GST_VIDEO_INFO_HEIGHT (info)) {
    if (!gst_mpp_use_rga ()) {
      GST_ERROR_OBJECT (self, "unable to convert without RGA");
      return FALSE;
    }

    /* Prefer NV12 when using RGA conversion */
    format = MPP_FMT_YUV420SP;

    gst_mpp_video_info_update_format (info,
        gst_mpp_mpp_format_to_gst_format (format), width, height);

    if (!gst_mpp_enc_video_info_align (info))
      return FALSE;

    GST_INFO_OBJECT (self, "converting to aligned %s",
        gst_mpp_video_format_to_string (GST_VIDEO_INFO_FORMAT (info)));
  }

  hstride = GST_MPP_VIDEO_INFO_HSTRIDE (info);
  vstride = GST_MPP_VIDEO_INFO_VSTRIDE (info);

  GST_INFO_OBJECT (self, "applying %s%s %dx%d (%dx%d)",
      gst_mpp_video_format_to_string (GST_VIDEO_INFO_FORMAT (info)),
      self->arm_afbc ? "(AFBC)" : "", width, height, hstride, vstride);

  if (self->arm_afbc) {
    if (self->mpp_type != MPP_VIDEO_CodingAVC &&
        self->mpp_type != MPP_VIDEO_CodingHEVC) {
      GST_WARNING_OBJECT (self, "Only H.264 and H.265 support ARM AFBC");
      self->arm_afbc = FALSE;
    } else {
      format |= MPP_FRAME_FBC_AFBC_V2;
    }
  }

  mpp_frame_set_fmt (self->mpp_frame, format);
  mpp_frame_set_width (self->mpp_frame, width);
  mpp_frame_set_height (self->mpp_frame, height);

  if (!GST_VIDEO_INFO_FPS_N (info) ||
      GST_VIDEO_INFO_FPS_N (info) / GST_VIDEO_INFO_FPS_D (info) > 256) {
    GST_WARNING_OBJECT (self, "framerate (%d/%d) is insane!",
        GST_VIDEO_INFO_FPS_N (info), GST_VIDEO_INFO_FPS_D (info));
    GST_VIDEO_INFO_FPS_N (info) = DEFAULT_FPS;
    GST_VIDEO_INFO_FPS_D (info) = 1;
  }

  mpp_enc_cfg_set_s32 (self->mpp_cfg, "prep:format", format);
  mpp_enc_cfg_set_s32 (self->mpp_cfg, "prep:width", width);
  mpp_enc_cfg_set_s32 (self->mpp_cfg, "prep:height", height);

  mpp_enc_cfg_set_s32 (self->mpp_cfg, "rc:fps_in_flex", 0);
  mpp_enc_cfg_set_s32 (self->mpp_cfg, "rc:fps_in_num",
      GST_VIDEO_INFO_FPS_N (info));
  mpp_enc_cfg_set_s32 (self->mpp_cfg, "rc:fps_in_denorm",
      GST_VIDEO_INFO_FPS_D (info));
  mpp_enc_cfg_set_s32 (self->mpp_cfg, "rc:fps_out_flex", 0);
  mpp_enc_cfg_set_s32 (self->mpp_cfg, "rc:fps_out_num",
      GST_VIDEO_INFO_FPS_N (info));
  mpp_enc_cfg_set_s32 (self->mpp_cfg, "rc:fps_out_denorm",
      GST_VIDEO_INFO_FPS_D (info));

  return gst_mpp_enc_apply_strides (encoder, hstride, vstride);
}

static gboolean
gst_mpp_enc_propose_allocation (GstVideoEncoder * encoder, GstQuery * query)
{
  GstMppEnc *self = GST_MPP_ENC (encoder);
  GstStructure *config, *params;
  GstVideoAlignment align;
  GstBufferPool *pool;
  GstVideoInfo info;
  GstCaps *caps;
  guint size;

  GST_DEBUG_OBJECT (self, "propose allocation");

  gst_query_parse_allocation (query, &caps, NULL);
  if (caps == NULL)
    return FALSE;

  if (!gst_video_info_from_caps (&info, caps))
    return FALSE;

  gst_mpp_enc_video_info_align (&info);
  size = GST_VIDEO_INFO_SIZE (&info);

  gst_video_alignment_reset (&align);
  align.padding_right = gst_mpp_get_pixel_stride (&info) -
      GST_VIDEO_INFO_WIDTH (&info);
  align.padding_bottom = GST_MPP_VIDEO_INFO_VSTRIDE (&info) -
      GST_VIDEO_INFO_HEIGHT (&info);

  /* Expose alignment to video-meta */
  params = gst_structure_new ("video-meta",
      "padding-top", G_TYPE_UINT, align.padding_top,
      "padding-bottom", G_TYPE_UINT, align.padding_bottom,
      "padding-left", G_TYPE_UINT, align.padding_left,
      "padding-right", G_TYPE_UINT, align.padding_right, NULL);
  gst_query_add_allocation_meta (query, GST_VIDEO_META_API_TYPE, params);
  gst_structure_free (params);

  pool = gst_video_buffer_pool_new ();

  config = gst_buffer_pool_get_config (pool);
  gst_buffer_pool_config_set_params (config, caps, size, 0, 0);
  gst_buffer_pool_config_set_allocator (config, self->allocator, NULL);

  /* Expose alignment to pool */
  gst_buffer_pool_config_add_option (config,
      GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT);
  gst_buffer_pool_config_set_video_alignment (config, &align);

  gst_buffer_pool_set_config (pool, config);

  gst_query_add_allocation_pool (query, pool, size, 0, 0);
  gst_query_add_allocation_param (query, self->allocator, NULL);

  gst_object_unref (pool);

  return GST_VIDEO_ENCODER_CLASS (parent_class)->propose_allocation (encoder,
      query);
}

static GstBuffer *
gst_mpp_enc_convert (GstVideoEncoder * encoder, GstVideoCodecFrame * frame)
{
  GstMppEnc *self = GST_MPP_ENC (encoder);
  GstVideoInfo src_info = self->input_state->info;
  GstVideoInfo dst_info = self->info;
  GstVideoFrame src_frame, dst_frame;
  GstBuffer *outbuf, *inbuf;
  GstMemory *in_mem, *out_mem = NULL;
  GstVideoMeta *meta;
  gsize size, maxsize, offset;
  gint src_hstride, src_vstride;
  guint i;

  inbuf = frame->input_buffer;

  meta = gst_buffer_get_video_meta (inbuf);
  if (meta) {
    for (i = 0; i < meta->n_planes; i++) {
      GST_VIDEO_INFO_PLANE_STRIDE (&src_info, i) = meta->stride[i];
      GST_VIDEO_INFO_PLANE_OFFSET (&src_info, i) = meta->offset[i];
    }
  }

  size = gst_buffer_get_sizes (inbuf, &offset, &maxsize);
  if (size < GST_VIDEO_INFO_SIZE (&src_info)) {
    GST_ERROR_OBJECT (self, "input buffer too small (%" G_GSIZE_FORMAT
        " < %" G_GSIZE_FORMAT ")", size, GST_VIDEO_INFO_SIZE (&src_info));
    return NULL;
  }

  outbuf = gst_buffer_new ();
  if (!outbuf)
    goto err;

  if (self->rotation)
    goto convert;

  if (gst_buffer_n_memory (inbuf) != 1)
    goto convert;

  in_mem = gst_buffer_peek_memory (inbuf, 0);

  out_mem = gst_mpp_allocator_import_gst_memory (self->allocator, in_mem);
  if (!out_mem)
    goto convert;

  src_hstride = GST_MPP_VIDEO_INFO_HSTRIDE (&src_info);
  src_vstride = GST_MPP_VIDEO_INFO_VSTRIDE (&src_info);

  /**
   * Update the strides of the dst video info temporarily to test if we
   * can use the src buffer directly.
   */
  if (!gst_mpp_video_info_align (&dst_info, src_hstride, src_vstride) ||
      !gst_mpp_enc_video_info_align (&dst_info) ||
      !gst_mpp_video_info_matched (&src_info, &dst_info)) {
    /* Reset the temporarily modified dst video info. */
    dst_info = self->info;
    goto convert;
  }

  gst_mpp_enc_apply_strides (encoder, src_hstride, src_vstride);
  if (!gst_mpp_enc_apply_properties (encoder))
    goto err;

  gst_buffer_append_memory (outbuf, out_mem);

  /* Keep a ref of the original memory */
  gst_buffer_append_memory (outbuf, gst_memory_ref (in_mem));

  GST_DEBUG_OBJECT (self, "using imported buffer");
  goto out;

convert:
  if (out_mem)
    gst_memory_unref (out_mem);

  out_mem = gst_allocator_alloc (self->allocator,
      GST_VIDEO_INFO_SIZE (&dst_info), NULL);
  if (!out_mem)
    goto err;

  gst_buffer_append_memory (outbuf, out_mem);

#ifdef HAVE_RGA
  if (gst_mpp_use_rga () &&
      gst_mpp_rga_convert (inbuf, &src_info, out_mem, &dst_info,
          self->rotation)) {
    GST_DEBUG_OBJECT (self, "using RGA converted buffer");
    goto out;
  }
#endif

  if (self->rotation ||
      GST_VIDEO_INFO_FORMAT (&src_info) != GST_VIDEO_INFO_FORMAT (&dst_info))
    goto err;

  if (gst_video_frame_map (&src_frame, &src_info, inbuf, GST_MAP_READ)) {
    if (gst_video_frame_map (&dst_frame, &dst_info, outbuf, GST_MAP_WRITE)) {
      GST_VIDEO_ENCODER_STREAM_UNLOCK (encoder);
      if (!gst_video_frame_copy (&dst_frame, &src_frame)) {
        GST_VIDEO_ENCODER_STREAM_LOCK (encoder);
        gst_video_frame_unmap (&dst_frame);
        gst_video_frame_unmap (&src_frame);
        goto err;
      }
      GST_VIDEO_ENCODER_STREAM_LOCK (encoder);
      gst_video_frame_unmap (&dst_frame);
    }
    gst_video_frame_unmap (&src_frame);
  }

  GST_DEBUG_OBJECT (self, "using software converted buffer");

out:
  gst_buffer_copy_into (outbuf, inbuf,
      GST_BUFFER_COPY_FLAGS | GST_BUFFER_COPY_TIMESTAMPS, 0, 0);

  gst_buffer_add_video_meta_full (outbuf, GST_VIDEO_FRAME_FLAG_NONE,
      GST_VIDEO_INFO_FORMAT (&dst_info),
      GST_VIDEO_INFO_WIDTH (&dst_info), GST_VIDEO_INFO_HEIGHT (&dst_info),
      GST_VIDEO_INFO_N_PLANES (&dst_info), dst_info.offset, dst_info.stride);

  return outbuf;
err:
  if (out_mem)
    gst_memory_unref (out_mem);

  if (outbuf)
    gst_buffer_unref (outbuf);

  GST_ERROR_OBJECT (self, "failed to convert frame");
  return NULL;
}

static gboolean
gst_mpp_enc_force_keyframe (GstVideoEncoder * encoder, gboolean keyframe)
{
  GstMppEnc *self = GST_MPP_ENC (encoder);

  /* HACK: Use gop(1) to force keyframe */

  if (!keyframe) {
    self->prop_dirty = TRUE;
    return gst_mpp_enc_apply_properties (encoder);
  }

  GST_INFO_OBJECT (self, "forcing keyframe");
  mpp_enc_cfg_set_s32 (self->mpp_cfg, "rc:gop", 1);

  if (self->mpi->control (self->mpp_ctx, MPP_ENC_SET_CFG, self->mpp_cfg)) {
    GST_WARNING_OBJECT (self, "failed to set enc cfg");
    return FALSE;
  }

  return TRUE;
}

static gboolean
gst_mpp_enc_send_frame_locked (GstVideoEncoder * encoder)
{
  GstMppEnc *self = GST_MPP_ENC (encoder);
  GstVideoCodecFrame *frame;
  GstMemory *mem;
  MppFrame mframe;
  MppBuffer mbuf;
  gboolean keyframe;
  guint32 frame_number;

  if (!self->frames)
    return FALSE;

  if (mpp_frame_init (&mframe))
    return FALSE;

  mpp_frame_set_fmt (mframe, mpp_frame_get_fmt (self->mpp_frame));
  mpp_frame_set_width (mframe, mpp_frame_get_width (self->mpp_frame));
  mpp_frame_set_height (mframe, mpp_frame_get_height (self->mpp_frame));
  mpp_frame_set_hor_stride (mframe, mpp_frame_get_hor_stride (self->mpp_frame));
  mpp_frame_set_ver_stride (mframe, mpp_frame_get_ver_stride (self->mpp_frame));

  frame_number = GPOINTER_TO_UINT (g_list_nth_data (self->frames, 0));
  frame = gst_video_encoder_get_frame (encoder, frame_number);

  keyframe = GST_VIDEO_CODEC_FRAME_IS_FORCE_KEYFRAME (frame);
  if (keyframe) {
    /* TODO: Find a way to set exact keyframe request */
    self->required_keyframe_number = frame_number;
    gst_mpp_enc_force_keyframe (encoder, TRUE);
  }

  /* HACK: Get the converted input buffer from frame->output_buffer */
  mem = gst_buffer_peek_memory (frame->output_buffer, 0);
  mbuf = gst_mpp_mpp_buffer_from_gst_memory (mem);
  mpp_frame_set_buffer (mframe, mbuf);

  gst_video_codec_frame_unref (frame);

  if (self->mpi->encode_put_frame (self->mpp_ctx, mframe)) {
    mpp_frame_deinit (&mframe);
    return FALSE;
  }

  GST_DEBUG_OBJECT (self, "encoding frame %d", frame_number);
  self->frames = g_list_delete_link (self->frames, self->frames);
  return TRUE;
}

static gboolean
gst_mpp_enc_poll_packet_locked (GstVideoEncoder * encoder)
{
  GstMppEnc *self = GST_MPP_ENC (encoder);
  GstVideoCodecFrame *frame;
  GstBuffer *buffer;
  GstMemory *mem;
  MppFrame mframe;
  MppPacket mpkt;
  MppMeta meta;
  MppBuffer mbuf;
  gint pkt_size;

  self->mpi->encode_get_packet (self->mpp_ctx, &mpkt);
  if (!mpkt)
    return FALSE;

  /* Deinit input frame */
  meta = mpp_packet_get_meta (mpkt);
  if (!mpp_meta_get_frame (meta, KEY_INPUT_FRAME, &mframe))
    mpp_frame_deinit (&mframe);

  /* Wake up the frame producer */
  self->pending_frames--;
  GST_MPP_ENC_BROADCAST (encoder);

  /* This encoded frame must be the oldest one */
  frame = gst_video_encoder_get_oldest_frame (encoder);

  if (self->flushing && !self->draining)
    goto drop;

  /* TODO: Remove it when using exact keyframe request */
  if (frame->system_frame_number == self->required_keyframe_number) {
    gst_mpp_enc_force_keyframe (encoder, FALSE);
    self->required_keyframe_number = 0;
  }

  pkt_size = mpp_packet_get_length (mpkt);
  mbuf = mpp_packet_get_buffer (mpkt);
  if (!mbuf)
    goto error;

  if (self->zero_copy_pkt) {
    buffer = gst_buffer_new ();
    if (!buffer)
      goto error;

    /* Allocated from the same DRM allocator in MPP */
    mpp_buffer_set_index (mbuf, gst_mpp_allocator_get_index (self->allocator));

    mem = gst_mpp_allocator_import_mppbuf (self->allocator, mbuf);
    if (!mem) {
      gst_buffer_unref (buffer);
      goto error;
    }

    gst_memory_resize (mem, 0, pkt_size);
    gst_buffer_append_memory (buffer, mem);
  } else {
    buffer = gst_video_encoder_allocate_output_buffer (encoder, pkt_size);
    if (!buffer)
      goto error;

    gst_buffer_fill (buffer, 0, mpp_buffer_get_ptr (mbuf), pkt_size);
  }

  gst_buffer_replace (&frame->output_buffer, buffer);
  gst_buffer_unref (buffer);

  GST_DEBUG_OBJECT (self, "finish frame ts=%" GST_TIME_FORMAT,
      GST_TIME_ARGS (frame->pts));

  gst_video_encoder_finish_frame (encoder, frame);

out:
  mpp_packet_deinit (&mpkt);
  return TRUE;
error:
  GST_WARNING_OBJECT (self, "can't process this frame");
drop:
  GST_DEBUG_OBJECT (self, "drop frame");
  gst_buffer_replace (&frame->output_buffer, NULL);
  gst_video_encoder_finish_frame (encoder, frame);
  goto out;
}

static void
gst_mpp_enc_loop (GstVideoEncoder * encoder)
{
  GstMppEnc *self = GST_MPP_ENC (encoder);

  GST_MPP_ENC_WAIT (encoder, self->pending_frames || self->flushing);

  GST_VIDEO_ENCODER_STREAM_LOCK (encoder);

  if (self->flushing && !self->pending_frames) {
    GST_INFO_OBJECT (self, "flushing");
    self->task_ret = GST_FLOW_FLUSHING;
    goto out;
  }

  /* Try sending ready frames to MPP (non-block) */
  while (gst_mpp_enc_send_frame_locked (encoder));

  /* Try polling encoded packets from MPP (1ms timeout) */
  while (gst_mpp_enc_poll_packet_locked (encoder));

out:
  if (self->task_ret != GST_FLOW_OK) {
    GST_DEBUG_OBJECT (self, "leaving output thread: %s",
        gst_flow_get_name (self->task_ret));

    gst_pad_pause_task (encoder->srcpad);
  }

  GST_VIDEO_ENCODER_STREAM_UNLOCK (encoder);
}

static GstFlowReturn
gst_mpp_enc_handle_frame (GstVideoEncoder * encoder, GstVideoCodecFrame * frame)
{
  GstMppEnc *self = GST_MPP_ENC (encoder);
  GstBuffer *buffer;
  GstFlowReturn ret = GST_FLOW_OK;

  GST_DEBUG_OBJECT (self, "handling frame %d", frame->system_frame_number);

  GST_MPP_ENC_LOCK (encoder);

  if (G_UNLIKELY (self->flushing))
    goto flushing;

  if (G_UNLIKELY (!GST_MPP_ENC_TASK_STARTED (encoder))) {
    GST_DEBUG_OBJECT (self, "starting encoding thread");

    gst_pad_start_task (encoder->srcpad,
        (GstTaskFunction) gst_mpp_enc_loop, encoder, NULL);
  }

  buffer = gst_mpp_enc_convert (encoder, frame);
  if (G_UNLIKELY (!buffer))
    goto not_negotiated;

  /* HACK: store the converted input buffer in frame->output_buffer */
  frame->output_buffer = buffer;

  /* Avoid holding too many frames */
  if (G_UNLIKELY (self->pending_frames >= self->max_pending)) {
    GST_VIDEO_ENCODER_STREAM_UNLOCK (encoder);
    GST_MPP_ENC_WAIT (encoder, self->pending_frames < self->max_pending
        || self->flushing);
    GST_VIDEO_ENCODER_STREAM_LOCK (encoder);
  }

  if (G_UNLIKELY (self->flushing))
    goto flushing;

  self->pending_frames++;
  self->frames =
      g_list_append (self->frames,
      GUINT_TO_POINTER (frame->system_frame_number));

  GST_MPP_ENC_BROADCAST (encoder);

  gst_video_codec_frame_unref (frame);

  GST_MPP_ENC_UNLOCK (encoder);

  return self->task_ret;

flushing:
  GST_WARNING_OBJECT (self, "flushing");
  ret = GST_FLOW_FLUSHING;
  goto drop;
not_negotiated:
  GST_ERROR_OBJECT (self, "not negotiated");
  ret = GST_FLOW_NOT_NEGOTIATED;
  goto drop;
drop:
  GST_WARNING_OBJECT (self, "can't handle this frame");
  gst_video_encoder_finish_frame (encoder, frame);

  GST_MPP_ENC_UNLOCK (encoder);

  return ret;
}

static GstStateChangeReturn
gst_mpp_enc_change_state (GstElement * element, GstStateChange transition)
{
  GstVideoEncoder *encoder = GST_VIDEO_ENCODER (element);

  if (transition == GST_STATE_CHANGE_PAUSED_TO_READY) {
    GST_VIDEO_ENCODER_STREAM_LOCK (encoder);
    gst_mpp_enc_reset (encoder, FALSE, TRUE);
    GST_VIDEO_ENCODER_STREAM_UNLOCK (encoder);
  }

  return GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
}

static void
gst_mpp_enc_init (GstMppEnc * self)
{
  self->mpp_type = MPP_VIDEO_CodingUnused;

  self->max_pending = DEFAULT_PROP_MAX_PENDING;

  self->header_mode = DEFAULT_PROP_HEADER_MODE;
  self->sei_mode = DEFAULT_PROP_SEI_MODE;
  self->rc_mode = DEFAULT_PROP_RC_MODE;
  self->rotation = DEFAULT_PROP_ROTATION;
  self->gop = DEFAULT_PROP_GOP;
  self->max_reenc = DEFAULT_PROP_MAX_REENC;
  self->bps = DEFAULT_PROP_BPS;
  self->bps_min = DEFAULT_PROP_BPS_MIN;
  self->bps_max = DEFAULT_PROP_BPS_MAX;
  self->zero_copy_pkt = DEFAULT_PROP_ZERO_COPY_PKT;
  self->arm_afbc = DEFAULT_PROP_ARM_AFBC;
  self->prop_dirty = TRUE;
}

#define GST_TYPE_MPP_ENC_HEADER_MODE (gst_mpp_enc_header_mode_get_type ())
static GType
gst_mpp_enc_header_mode_get_type (void)
{
  static GType header_mode = 0;

  if (!header_mode) {
    static const GEnumValue modes[] = {
      {MPP_ENC_HEADER_MODE_DEFAULT, "Only in the first frame", "first-frame"},
      {MPP_ENC_HEADER_MODE_EACH_IDR, "In every IDR frames", "each-idr"},
      {0, NULL, NULL}
    };
    header_mode = g_enum_register_static ("MppEncHeaderMode", modes);
  }
  return header_mode;
}

#define GST_TYPE_MPP_ENC_SEI_MODE (gst_mpp_enc_sei_mode_get_type ())
static GType
gst_mpp_enc_sei_mode_get_type (void)
{
  static GType sei_mode = 0;

  if (!sei_mode) {
    static const GEnumValue modes[] = {
      {MPP_ENC_SEI_MODE_DISABLE, "SEI disabled", "disable"},
      {MPP_ENC_SEI_MODE_ONE_SEQ, "One SEI per sequence", "one-seq"},
      {MPP_ENC_SEI_MODE_ONE_FRAME, "One SEI per frame(if changed)",
          "one-frame"},
      {0, NULL, NULL}
    };
    sei_mode = g_enum_register_static ("GstMppEncSeiMode", modes);
  }
  return sei_mode;
}

#define GST_TYPE_MPP_ENC_RC_MODE (gst_mpp_enc_rc_mode_get_type ())
static GType
gst_mpp_enc_rc_mode_get_type (void)
{
  static GType rc_mode = 0;

  if (!rc_mode) {
    static const GEnumValue modes[] = {
      {MPP_ENC_RC_MODE_VBR, "Variable bitrate", "vbr"},
      {MPP_ENC_RC_MODE_CBR, "Constant bitrate", "cbr"},
      {MPP_ENC_RC_MODE_FIXQP, "Fixed QP", "fixqp"},
      {0, NULL, NULL}
    };
    rc_mode = g_enum_register_static ("GstMppEncRcMode", modes);
  }
  return rc_mode;
}

#ifdef HAVE_RGA
#define GST_TYPE_MPP_ENC_ROTATION (gst_mpp_enc_rotation_get_type ())
static GType
gst_mpp_enc_rotation_get_type (void)
{
  static GType rotation = 0;

  if (!rotation) {
    static const GEnumValue rotations[] = {
      {0, "Rotate 0", "0"},
      {90, "Rotate 90", "90"},
      {180, "Rotate 180", "180"},
      {270, "Rotate 270", "270"},
      {0, NULL, NULL}
    };
    rotation = g_enum_register_static ("GstMppEncRotation", rotations);
  }
  return rotation;
}
#endif

static void
gst_mpp_enc_class_init (GstMppEncClass * klass)
{
  GstVideoEncoderClass *encoder_class = GST_VIDEO_ENCODER_CLASS (klass);
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  const gchar *env;

  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "mppenc", 0, "MPP encoder");

  encoder_class->start = GST_DEBUG_FUNCPTR (gst_mpp_enc_start);
  encoder_class->stop = GST_DEBUG_FUNCPTR (gst_mpp_enc_stop);
  encoder_class->flush = GST_DEBUG_FUNCPTR (gst_mpp_enc_flush);
  encoder_class->finish = GST_DEBUG_FUNCPTR (gst_mpp_enc_finish);
  encoder_class->set_format = GST_DEBUG_FUNCPTR (gst_mpp_enc_set_format);
  encoder_class->propose_allocation =
      GST_DEBUG_FUNCPTR (gst_mpp_enc_propose_allocation);
  encoder_class->handle_frame = GST_DEBUG_FUNCPTR (gst_mpp_enc_handle_frame);

  gobject_class->set_property = GST_DEBUG_FUNCPTR (gst_mpp_enc_set_property);
  gobject_class->get_property = GST_DEBUG_FUNCPTR (gst_mpp_enc_get_property);

  env = g_getenv ("GST_MPP_ENC_MAX_PENDING");
  if (env)
    DEFAULT_PROP_MAX_PENDING = MAX (MIN (atoi (env), MPP_MAX_PENDING), 1);

  g_object_class_install_property (gobject_class, PROP_MAX_PENDING,
      g_param_spec_uint ("max-pending", "Max pending frames",
          "Max pending frames",
          1, MPP_MAX_PENDING, DEFAULT_PROP_MAX_PENDING,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_HEADER_MODE,
      g_param_spec_enum ("header-mode", "Header mode",
          "Header mode",
          GST_TYPE_MPP_ENC_HEADER_MODE, DEFAULT_PROP_HEADER_MODE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_SEI_MODE,
      g_param_spec_enum ("sei-mode", "SEI mode",
          "SEI mode",
          GST_TYPE_MPP_ENC_SEI_MODE, DEFAULT_PROP_SEI_MODE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_RC_MODE,
      g_param_spec_enum ("rc-mode", "RC mode",
          "RC mode",
          GST_TYPE_MPP_ENC_RC_MODE, DEFAULT_PROP_RC_MODE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

#ifdef HAVE_RGA
  if (!gst_mpp_use_rga ())
    goto no_rga;

  g_object_class_install_property (gobject_class, PROP_ROTATION,
      g_param_spec_enum ("rotation", "Rotation",
          "Rotation",
          GST_TYPE_MPP_ENC_ROTATION, DEFAULT_PROP_ROTATION,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_WIDTH,
      g_param_spec_uint ("width", "Width",
          "Width (0 = original)",
          0, G_MAXINT, DEFAULT_PROP_WIDTH,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_HEIGHT,
      g_param_spec_uint ("height", "Height",
          "Height (0 = original)",
          0, G_MAXINT, DEFAULT_PROP_HEIGHT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

no_rga:
#endif

  g_object_class_install_property (gobject_class, PROP_GOP,
      g_param_spec_int ("gop", "Group of pictures",
          "Group of pictures starting with I frame (-1 = FPS, 1 = all I frames)",
          -1, G_MAXINT, DEFAULT_PROP_GOP,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_MAX_REENC,
      g_param_spec_uint ("max-reenc", "Max re-encode times",
          "Max re-encode times for one frame",
          0, 3, DEFAULT_PROP_MAX_REENC,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_BPS,
      g_param_spec_uint ("bps", "Target BPS",
          "Target BPS (0 = auto calculate)",
          0, G_MAXINT, DEFAULT_PROP_BPS,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_BPS_MIN,
      g_param_spec_uint ("bps-min", "Min BPS",
          "Min BPS (0 = auto calculate)",
          0, G_MAXINT, DEFAULT_PROP_BPS_MIN,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_BPS_MAX,
      g_param_spec_uint ("bps-max", "Max BPS",
          "Max BPS (0 = auto calculate)",
          0, G_MAXINT, DEFAULT_PROP_BPS_MAX,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_ZERO_COPY_PKT,
      g_param_spec_boolean ("zero-copy-pkt", "Zero-copy encoded packet",
          "Zero-copy encoded packet", DEFAULT_PROP_ZERO_COPY_PKT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  env = g_getenv ("GST_MPP_ENC_DEFAULT_ARM_AFBC");
  if (env && env[0] == '1')
    DEFAULT_PROP_ARM_AFBC = TRUE;

  g_object_class_install_property (gobject_class, PROP_ARM_AFBC,
      g_param_spec_boolean ("arm-afbc", "ARM AFBC",
          "Input is ARM AFBC compressed format", DEFAULT_PROP_ARM_AFBC,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  element_class->change_state = GST_DEBUG_FUNCPTR (gst_mpp_enc_change_state);
}

/*
 *  gstvaapipostproc.c - VA-API video postprocessing
 *
 *  Copyright (C) 2012-2014 Intel Corporation
 *    Author: Gwenole Beauchesne <gwenole.beauchesne@intel.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public License
 *  as published by the Free Software Foundation; either version 2.1
 *  of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free
 *  Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301 USA
 */

/**
 * SECTION:gstvaapipostproc
 * @short_description: A video postprocessing filter
 *
 * vaapipostproc consists in various postprocessing algorithms to be
 * applied to VA surfaces. So far, only basic bob deinterlacing is
 * implemented.
 */

#include "gstcompat.h"
#include <gst/video/video.h>

#include "gstvaapipostproc.h"
#include "gstvaapipluginutil.h"
#include "gstvaapivideobuffer.h"
#include "gstvaapivideobufferpool.h"
#include "gstvaapivideomemory.h"

#define GST_PLUGIN_NAME "vaapipostproc"
#define GST_PLUGIN_DESC "A video postprocessing filter"

GST_DEBUG_CATEGORY_STATIC (gst_debug_vaapipostproc);
#define GST_CAT_DEFAULT gst_debug_vaapipostproc

# define GST_VAAPIPOSTPROC_SURFACE_CAPS \
    GST_VIDEO_CAPS_MAKE_WITH_FEATURES(  \
        GST_CAPS_FEATURE_MEMORY_VAAPI_SURFACE, "{ ENCODED, I420, YV12, NV12 }")

/* Default templates */
/* *INDENT-OFF* */
static const char gst_vaapipostproc_sink_caps_str[] =
  GST_VAAPIPOSTPROC_SURFACE_CAPS ", "
  GST_CAPS_INTERLACED_MODES "; "
  GST_VIDEO_CAPS_MAKE (GST_VIDEO_FORMATS_ALL) ", "
   GST_CAPS_INTERLACED_MODES;
/* *INDENT-ON* */

/* *INDENT-OFF* */
static const char gst_vaapipostproc_src_caps_str[] =
  GST_VAAPIPOSTPROC_SURFACE_CAPS ", "
  GST_CAPS_INTERLACED_FALSE "; "
  GST_VIDEO_CAPS_MAKE_WITH_FEATURES (
      GST_CAPS_FEATURE_META_GST_VIDEO_GL_TEXTURE_UPLOAD_META, "{ RGBA, BGRA }") ", "
  GST_CAPS_INTERLACED_FALSE "; "
  GST_VIDEO_CAPS_MAKE (GST_VIDEO_FORMATS_ALL) ", "
  GST_CAPS_INTERLACED_FALSE;
/* *INDENT-ON* */

/* *INDENT-OFF* */
static GstStaticPadTemplate gst_vaapipostproc_sink_factory =
  GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (gst_vaapipostproc_sink_caps_str));
/* *INDENT-ON* */

/* *INDENT-OFF* */
static GstStaticPadTemplate gst_vaapipostproc_src_factory =
  GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (gst_vaapipostproc_src_caps_str));
/* *INDENT-ON* */

static void gst_vaapipostproc_colorbalance_init (gpointer iface, gpointer data);

G_DEFINE_TYPE_WITH_CODE (GstVaapiPostproc, gst_vaapipostproc,
    GST_TYPE_BASE_TRANSFORM, GST_VAAPI_PLUGIN_BASE_INIT_INTERFACES
    G_IMPLEMENT_INTERFACE (GST_TYPE_COLOR_BALANCE,
        gst_vaapipostproc_colorbalance_init));

static GstVideoFormat native_formats[] =
    { GST_VIDEO_FORMAT_NV12, GST_VIDEO_FORMAT_YV12, GST_VIDEO_FORMAT_I420 };

enum
{
  PROP_0,

  PROP_FORMAT,
  PROP_WIDTH,
  PROP_HEIGHT,
  PROP_FORCE_ASPECT_RATIO,
  PROP_DEINTERLACE_MODE,
  PROP_DEINTERLACE_METHOD,
  PROP_DENOISE,
  PROP_SHARPEN,
  PROP_HUE,
  PROP_SATURATION,
  PROP_BRIGHTNESS,
  PROP_CONTRAST,
  PROP_SCALE_METHOD,
  PROP_SKIN_TONE_ENHANCEMENT,
};

#define DEFAULT_FORMAT                  GST_VIDEO_FORMAT_ENCODED
#define DEFAULT_DEINTERLACE_MODE        GST_VAAPI_DEINTERLACE_MODE_AUTO
#define DEFAULT_DEINTERLACE_METHOD      GST_VAAPI_DEINTERLACE_METHOD_BOB

#define GST_VAAPI_TYPE_DEINTERLACE_MODE \
    gst_vaapi_deinterlace_mode_get_type()

static GType
gst_vaapi_deinterlace_mode_get_type (void)
{
  static GType deinterlace_mode_type = 0;

  static const GEnumValue mode_types[] = {
    {GST_VAAPI_DEINTERLACE_MODE_AUTO,
        "Auto detection", "auto"},
    {GST_VAAPI_DEINTERLACE_MODE_INTERLACED,
        "Force deinterlacing", "interlaced"},
    {GST_VAAPI_DEINTERLACE_MODE_DISABLED,
        "Never deinterlace", "disabled"},
    {0, NULL, NULL},
  };

  if (!deinterlace_mode_type) {
    deinterlace_mode_type =
        g_enum_register_static ("GstVaapiDeinterlaceMode", mode_types);
  }
  return deinterlace_mode_type;
}

static void
ds_reset (GstVaapiDeinterlaceState * ds)
{
  guint i;

  for (i = 0; i < G_N_ELEMENTS (ds->buffers); i++)
    gst_buffer_replace (&ds->buffers[i], NULL);
  ds->buffers_index = 0;
  ds->num_surfaces = 0;
  ds->deint = FALSE;
  ds->tff = FALSE;
}

static void
ds_add_buffer (GstVaapiDeinterlaceState * ds, GstBuffer * buf)
{
  gst_buffer_replace (&ds->buffers[ds->buffers_index], buf);
  ds->buffers_index = (ds->buffers_index + 1) % G_N_ELEMENTS (ds->buffers);
}

static inline GstBuffer *
ds_get_buffer (GstVaapiDeinterlaceState * ds, guint index)
{
  /* Note: the index increases towards older buffers.
     i.e. buffer at index 0 means the immediately preceding buffer
     in the history, buffer at index 1 means the one preceding the
     surface at index 0, etc. */
  const guint n = ds->buffers_index + G_N_ELEMENTS (ds->buffers) - index - 1;
  return ds->buffers[n % G_N_ELEMENTS (ds->buffers)];
}

static void
ds_set_surfaces (GstVaapiDeinterlaceState * ds)
{
  GstVaapiVideoMeta *meta;
  guint i;

  ds->num_surfaces = 0;
  for (i = 0; i < G_N_ELEMENTS (ds->buffers); i++) {
    GstBuffer *const buf = ds_get_buffer (ds, i);
    if (!buf)
      break;

    meta = gst_buffer_get_vaapi_video_meta (buf);
    ds->surfaces[ds->num_surfaces++] = gst_vaapi_video_meta_get_surface (meta);
  }
}

static GstVaapiFilterOpInfo *
find_filter_op (GPtrArray * filter_ops, GstVaapiFilterOp op)
{
  guint i;

  if (filter_ops) {
    for (i = 0; i < filter_ops->len; i++) {
      GstVaapiFilterOpInfo *const filter_op = g_ptr_array_index (filter_ops, i);
      if (filter_op->op == op)
        return filter_op;
    }
  }
  return NULL;
}

static inline gboolean
gst_vaapipostproc_ensure_display (GstVaapiPostproc * postproc)
{
  return
      gst_vaapi_plugin_base_ensure_display (GST_VAAPI_PLUGIN_BASE (postproc));
}

static gboolean
gst_vaapipostproc_ensure_uploader (GstVaapiPostproc * postproc)
{
  if (!gst_vaapipostproc_ensure_display (postproc))
    return FALSE;
  if (!gst_vaapi_plugin_base_ensure_uploader (GST_VAAPI_PLUGIN_BASE (postproc)))
    return FALSE;
  return TRUE;
}

static gboolean
gst_vaapipostproc_ensure_filter (GstVaapiPostproc * postproc)
{
  if (postproc->filter)
    return TRUE;

  if (!gst_vaapipostproc_ensure_display (postproc))
    return FALSE;

  gst_caps_replace (&postproc->allowed_srcpad_caps, NULL);
  gst_caps_replace (&postproc->allowed_sinkpad_caps, NULL);

  postproc->filter =
      gst_vaapi_filter_new (GST_VAAPI_PLUGIN_BASE_DISPLAY (postproc));
  if (!postproc->filter)
    return FALSE;
  return TRUE;
}

static gboolean
gst_vaapipostproc_ensure_filter_caps (GstVaapiPostproc * postproc)
{
  if (!gst_vaapipostproc_ensure_filter (postproc))
    return FALSE;

  postproc->filter_ops = gst_vaapi_filter_get_operations (postproc->filter);
  if (!postproc->filter_ops)
    return FALSE;

  postproc->filter_formats = gst_vaapi_filter_get_formats (postproc->filter);
  if (!postproc->filter_formats)
    return FALSE;
  return TRUE;
}

static gboolean
gst_vaapipostproc_create (GstVaapiPostproc * postproc)
{
  if (!gst_vaapi_plugin_base_open (GST_VAAPI_PLUGIN_BASE (postproc)))
    return FALSE;
  if (!gst_vaapipostproc_ensure_display (postproc))
    return FALSE;
  if (!gst_vaapipostproc_ensure_uploader (postproc))
    return FALSE;

  postproc->use_vpp = FALSE;
  postproc->has_vpp = gst_vaapipostproc_ensure_filter (postproc);
  return TRUE;
}

static void
gst_vaapipostproc_destroy_filter (GstVaapiPostproc * postproc)
{
  if (postproc->filter_formats) {
    g_array_unref (postproc->filter_formats);
    postproc->filter_formats = NULL;
  }

  if (postproc->filter_ops) {
    g_ptr_array_unref (postproc->filter_ops);
    postproc->filter_ops = NULL;
  }
  if (postproc->cb_channels) {
    g_list_free_full (postproc->cb_channels, g_object_unref);
    postproc->cb_channels = NULL;
  }
  gst_vaapi_filter_replace (&postproc->filter, NULL);
  gst_vaapi_video_pool_replace (&postproc->filter_pool, NULL);
}

static void
gst_vaapipostproc_destroy (GstVaapiPostproc * postproc)
{
  ds_reset (&postproc->deinterlace_state);
  gst_vaapipostproc_destroy_filter (postproc);

  gst_caps_replace (&postproc->allowed_sinkpad_caps, NULL);
  gst_caps_replace (&postproc->allowed_srcpad_caps, NULL);
  gst_vaapi_plugin_base_close (GST_VAAPI_PLUGIN_BASE (postproc));
}

static gboolean
gst_vaapipostproc_start (GstBaseTransform * trans)
{
  GstVaapiPostproc *const postproc = GST_VAAPIPOSTPROC (trans);

  ds_reset (&postproc->deinterlace_state);
  if (!gst_vaapi_plugin_base_open (GST_VAAPI_PLUGIN_BASE (postproc)))
    return FALSE;
  if (!gst_vaapipostproc_ensure_filter (postproc))
    return FALSE;
  return TRUE;
}

static gboolean
gst_vaapipostproc_stop (GstBaseTransform * trans)
{
  GstVaapiPostproc *const postproc = GST_VAAPIPOSTPROC (trans);

  ds_reset (&postproc->deinterlace_state);
  gst_vaapi_plugin_base_close (GST_VAAPI_PLUGIN_BASE (postproc));

  postproc->field_duration = GST_CLOCK_TIME_NONE;
  gst_video_info_init(&postproc->sinkpad_info);
  gst_video_info_init(&postproc->srcpad_info);
  gst_video_info_init(&postproc->filter_pool_info);

  return TRUE;
}

static gboolean
should_deinterlace_buffer (GstVaapiPostproc * postproc, GstBuffer * buf)
{
  if (!(postproc->flags & GST_VAAPI_POSTPROC_FLAG_DEINTERLACE) ||
      postproc->deinterlace_mode == GST_VAAPI_DEINTERLACE_MODE_DISABLED)
    return FALSE;

  if (postproc->deinterlace_mode == GST_VAAPI_DEINTERLACE_MODE_INTERLACED)
    return TRUE;

  g_assert (postproc->deinterlace_mode == GST_VAAPI_DEINTERLACE_MODE_AUTO);

  switch (GST_VIDEO_INFO_INTERLACE_MODE (&postproc->sinkpad_info)) {
    case GST_VIDEO_INTERLACE_MODE_INTERLEAVED:
      return TRUE;
    case GST_VIDEO_INTERLACE_MODE_PROGRESSIVE:
      return FALSE;
    case GST_VIDEO_INTERLACE_MODE_MIXED:
      if (GST_BUFFER_FLAG_IS_SET (buf, GST_VIDEO_BUFFER_FLAG_INTERLACED))
        return TRUE;
      break;
    default:
      GST_ERROR ("unhandled \"interlace-mode\", disabling deinterlacing");
      break;
  }
  return FALSE;
}

static GstBuffer *
create_output_buffer (GstVaapiPostproc * postproc)
{
  GstBuffer *outbuf;

  GstBufferPool *const pool =
      GST_VAAPI_PLUGIN_BASE (postproc)->srcpad_buffer_pool;
  GstFlowReturn ret;

  g_return_val_if_fail (pool != NULL, NULL);

  if (!gst_buffer_pool_set_active (pool, TRUE))
    goto error_activate_pool;

  outbuf = NULL;
  ret = gst_buffer_pool_acquire_buffer (pool, &outbuf, NULL);
  if (ret != GST_FLOW_OK || !outbuf)
    goto error_create_buffer;
  return outbuf;

  /* ERRORS */
error_activate_pool:
  {
    GST_ERROR ("failed to activate output video buffer pool");
    return NULL;
  }
error_create_buffer:
  {
    GST_ERROR ("failed to create output video buffer");
    return NULL;
  }
}

static gboolean
append_output_buffer_metadata (GstVaapiPostproc * postproc, GstBuffer * outbuf,
    GstBuffer * inbuf, guint flags)
{
  GstVaapiVideoMeta *inbuf_meta, *outbuf_meta;
  GstVaapiSurfaceProxy *proxy;

  gst_buffer_copy_into (outbuf, inbuf, flags | GST_BUFFER_COPY_FLAGS, 0, -1);

  /* GstVideoCropMeta */
  if (!postproc->use_vpp) {
    GstVideoCropMeta *const crop_meta = gst_buffer_get_video_crop_meta (inbuf);
    if (crop_meta) {
      GstVideoCropMeta *const out_crop_meta =
          gst_buffer_add_video_crop_meta (outbuf);
      if (out_crop_meta)
        *out_crop_meta = *crop_meta;
    }
  }

  /* GstVaapiVideoMeta */
  inbuf_meta = gst_buffer_get_vaapi_video_meta (inbuf);
  g_return_val_if_fail (inbuf_meta != NULL, FALSE);
  proxy = gst_vaapi_video_meta_get_surface_proxy (inbuf_meta);

  outbuf_meta = gst_buffer_get_vaapi_video_meta (outbuf);
  g_return_val_if_fail (outbuf_meta != NULL, FALSE);
  proxy = gst_vaapi_surface_proxy_copy (proxy);
  if (!proxy)
    return FALSE;

  gst_vaapi_video_meta_set_surface_proxy (outbuf_meta, proxy);
  gst_vaapi_surface_proxy_unref (proxy);
  return TRUE;
}

static gboolean
deint_method_is_advanced (GstVaapiDeinterlaceMethod deint_method)
{
  gboolean is_advanced;

  switch (deint_method) {
    case GST_VAAPI_DEINTERLACE_METHOD_MOTION_ADAPTIVE:
    case GST_VAAPI_DEINTERLACE_METHOD_MOTION_COMPENSATED:
      is_advanced = TRUE;
      break;
    default:
      is_advanced = FALSE;
      break;
  }
  return is_advanced;
}

static GstVaapiDeinterlaceMethod
get_next_deint_method (GstVaapiDeinterlaceMethod deint_method)
{
  switch (deint_method) {
    case GST_VAAPI_DEINTERLACE_METHOD_MOTION_COMPENSATED:
      deint_method = GST_VAAPI_DEINTERLACE_METHOD_MOTION_ADAPTIVE;
      break;
    default:
      /* Default to basic "bob" for all others */
      deint_method = GST_VAAPI_DEINTERLACE_METHOD_BOB;
      break;
  }
  return deint_method;
}

static gboolean
set_best_deint_method (GstVaapiPostproc * postproc, guint flags,
    GstVaapiDeinterlaceMethod * deint_method_ptr)
{
  GstVaapiDeinterlaceMethod deint_method = postproc->deinterlace_method;
  gboolean success;

  for (;;) {
    success = gst_vaapi_filter_set_deinterlacing (postproc->filter,
        deint_method, flags);
    if (success || deint_method == GST_VAAPI_DEINTERLACE_METHOD_BOB)
      break;
    deint_method = get_next_deint_method (deint_method);
  }
  *deint_method_ptr = deint_method;
  return success;
}

static GstFlowReturn
gst_vaapipostproc_process_vpp (GstBaseTransform * trans, GstBuffer * inbuf,
    GstBuffer * outbuf)
{
  GstVaapiPostproc *const postproc = GST_VAAPIPOSTPROC (trans);
  GstVaapiDeinterlaceState *const ds = &postproc->deinterlace_state;
  GstVaapiVideoMeta *inbuf_meta, *outbuf_meta;
  GstVaapiSurface *inbuf_surface, *outbuf_surface;
  GstVaapiSurfaceProxy *proxy;
  GstVaapiFilterStatus status;
  GstClockTime timestamp;
  GstFlowReturn ret;
  GstBuffer *fieldbuf;
  GstVaapiDeinterlaceMethod deint_method;
  guint flags, deint_flags;
  gboolean tff, deint, deint_refs, deint_changed;
  GstVaapiRectangle *crop_rect = NULL;
  GstVaapiRectangle tmp_rect;

  /* Validate filters */
  if ((postproc->flags & GST_VAAPI_POSTPROC_FLAG_FORMAT) &&
      !gst_vaapi_filter_set_format (postproc->filter, postproc->format))
    return GST_FLOW_NOT_SUPPORTED;

  if ((postproc->flags & GST_VAAPI_POSTPROC_FLAG_DENOISE) &&
      !gst_vaapi_filter_set_denoising_level (postproc->filter,
          postproc->denoise_level))
    return GST_FLOW_NOT_SUPPORTED;

  if ((postproc->flags & GST_VAAPI_POSTPROC_FLAG_SHARPEN) &&
      !gst_vaapi_filter_set_sharpening_level (postproc->filter,
          postproc->sharpen_level))
    return GST_FLOW_NOT_SUPPORTED;

  if ((postproc->flags & GST_VAAPI_POSTPROC_FLAG_HUE) &&
      !gst_vaapi_filter_set_hue (postproc->filter, postproc->hue))
    return GST_FLOW_NOT_SUPPORTED;

  if ((postproc->flags & GST_VAAPI_POSTPROC_FLAG_SATURATION) &&
      !gst_vaapi_filter_set_saturation (postproc->filter, postproc->saturation))
    return GST_FLOW_NOT_SUPPORTED;

  if ((postproc->flags & GST_VAAPI_POSTPROC_FLAG_BRIGHTNESS) &&
      !gst_vaapi_filter_set_brightness (postproc->filter, postproc->brightness))
    return GST_FLOW_NOT_SUPPORTED;

  if ((postproc->flags & GST_VAAPI_POSTPROC_FLAG_CONTRAST) &&
      !gst_vaapi_filter_set_contrast (postproc->filter, postproc->contrast))
    return GST_FLOW_NOT_SUPPORTED;

  if ((postproc->flags & GST_VAAPI_POSTPROC_FLAG_SCALE) &&
      !gst_vaapi_filter_set_scaling (postproc->filter, postproc->scale_method))
    return GST_FLOW_NOT_SUPPORTED;

  if ((postproc->flags & GST_VAAPI_POSTPROC_FLAG_SKINTONE) &&
      !gst_vaapi_filter_set_skintone (postproc->filter,
           postproc->skintone_enhance))
    return GST_FLOW_NOT_SUPPORTED;

  inbuf_meta = gst_buffer_get_vaapi_video_meta (inbuf);
  if (!inbuf_meta)
    goto error_invalid_buffer;
  inbuf_surface = gst_vaapi_video_meta_get_surface (inbuf_meta);

  GstVideoCropMeta *const crop_meta = gst_buffer_get_video_crop_meta (inbuf);
  if (crop_meta) {
    crop_rect = &tmp_rect;
    crop_rect->x = crop_meta->x;
    crop_rect->y = crop_meta->y;
    crop_rect->width = crop_meta->width;
    crop_rect->height = crop_meta->height;
  }
  if (!crop_rect)
    crop_rect = (GstVaapiRectangle *)
        gst_vaapi_video_meta_get_render_rect (inbuf_meta);

  timestamp = GST_BUFFER_TIMESTAMP (inbuf);
  tff = GST_BUFFER_FLAG_IS_SET (inbuf, GST_VIDEO_BUFFER_FLAG_TFF);
  deint = should_deinterlace_buffer (postproc, inbuf);

  /* Drop references if deinterlacing conditions changed */
  deint_changed = deint != ds->deint;
  if (deint_changed || (ds->num_surfaces > 0 && tff != ds->tff))
    ds_reset (ds);

  deint_method = postproc->deinterlace_method;
  deint_refs = deint_method_is_advanced (deint_method);
  if (deint_refs && 0) {
    GstBuffer *const prev_buf = ds_get_buffer (ds, 0);
    GstClockTime prev_pts, pts = GST_BUFFER_TIMESTAMP (inbuf);
    /* Reset deinterlacing state when there is a discontinuity */
    if (prev_buf && (prev_pts = GST_BUFFER_TIMESTAMP (prev_buf)) != pts) {
      const GstClockTimeDiff pts_diff = GST_CLOCK_DIFF (prev_pts, pts);
      if (pts_diff < 0 || (postproc->field_duration > 0 &&
              pts_diff >= postproc->field_duration * 3 - 1))
        ds_reset (ds);
    }
  }

  ds->deint = deint;
  ds->tff = tff;

  flags = gst_vaapi_video_meta_get_render_flags (inbuf_meta) &
      ~GST_VAAPI_PICTURE_STRUCTURE_MASK;

  /* First field */
  if (postproc->flags & GST_VAAPI_POSTPROC_FLAG_DEINTERLACE) {
    fieldbuf = create_output_buffer (postproc);
    if (!fieldbuf)
      goto error_create_buffer;

    outbuf_meta = gst_buffer_get_vaapi_video_meta (fieldbuf);
    if (!outbuf_meta)
      goto error_create_meta;

    proxy =
        gst_vaapi_surface_proxy_new_from_pool (GST_VAAPI_SURFACE_POOL
        (postproc->filter_pool));
    if (!proxy)
      goto error_create_proxy;
    gst_vaapi_video_meta_set_surface_proxy (outbuf_meta, proxy);
    gst_vaapi_surface_proxy_unref (proxy);

    if (deint) {
      deint_flags = (tff ? GST_VAAPI_DEINTERLACE_FLAG_TOPFIELD : 0);
      if (tff)
        deint_flags |= GST_VAAPI_DEINTERLACE_FLAG_TFF;
      if (!set_best_deint_method (postproc, deint_flags, &deint_method))
        goto error_op_deinterlace;

      if (deint_method != postproc->deinterlace_method) {
        GST_DEBUG ("unsupported deinterlace-method %u. Using %u instead",
            postproc->deinterlace_method, deint_method);
        postproc->deinterlace_method = deint_method;
        deint_refs = deint_method_is_advanced (deint_method);
      }

      if (deint_refs) {
        ds_set_surfaces (ds);
        if (!gst_vaapi_filter_set_deinterlacing_references (postproc->filter,
                ds->surfaces, ds->num_surfaces, NULL, 0))
          goto error_op_deinterlace;
      }
    } else if (deint_changed) {
      // Reset internal filter to non-deinterlacing mode
      deint_method = GST_VAAPI_DEINTERLACE_METHOD_NONE;
      if (!gst_vaapi_filter_set_deinterlacing (postproc->filter,
              deint_method, 0))
        goto error_op_deinterlace;
    }

    outbuf_surface = gst_vaapi_video_meta_get_surface (outbuf_meta);
    gst_vaapi_filter_set_cropping_rectangle (postproc->filter, crop_rect);
    status = gst_vaapi_filter_process (postproc->filter, inbuf_surface,
        outbuf_surface, flags);
    if (status != GST_VAAPI_FILTER_STATUS_SUCCESS)
      goto error_process_vpp;

    GST_BUFFER_TIMESTAMP (fieldbuf) = timestamp;
    GST_BUFFER_DURATION (fieldbuf) = postproc->field_duration;
    ret = gst_pad_push (trans->srcpad, fieldbuf);
    if (ret != GST_FLOW_OK)
      goto error_push_buffer;
  }
  fieldbuf = NULL;

  /* Second field */
  outbuf_meta = gst_buffer_get_vaapi_video_meta (outbuf);
  if (!outbuf_meta)
    goto error_create_meta;

  proxy =
      gst_vaapi_surface_proxy_new_from_pool (GST_VAAPI_SURFACE_POOL
      (postproc->filter_pool));
  if (!proxy)
    goto error_create_proxy;
  gst_vaapi_video_meta_set_surface_proxy (outbuf_meta, proxy);
  gst_vaapi_surface_proxy_unref (proxy);

  if (deint) {
    deint_flags = (tff ? 0 : GST_VAAPI_DEINTERLACE_FLAG_TOPFIELD);
    if (tff)
      deint_flags |= GST_VAAPI_DEINTERLACE_FLAG_TFF;
    if (!gst_vaapi_filter_set_deinterlacing (postproc->filter,
            deint_method, deint_flags))
      goto error_op_deinterlace;

    if (deint_refs
        && !gst_vaapi_filter_set_deinterlacing_references (postproc->filter,
            ds->surfaces, ds->num_surfaces, NULL, 0))
      goto error_op_deinterlace;
  } else if (deint_changed
      && !gst_vaapi_filter_set_deinterlacing (postproc->filter, deint_method,
          0))
    goto error_op_deinterlace;

  outbuf_surface = gst_vaapi_video_meta_get_surface (outbuf_meta);
  gst_vaapi_filter_set_cropping_rectangle (postproc->filter, crop_rect);
  status = gst_vaapi_filter_process (postproc->filter, inbuf_surface,
      outbuf_surface, flags);
  if (status != GST_VAAPI_FILTER_STATUS_SUCCESS)
    goto error_process_vpp;

  if (!(postproc->flags & GST_VAAPI_POSTPROC_FLAG_DEINTERLACE))
    gst_buffer_copy_into (outbuf, inbuf, GST_BUFFER_COPY_TIMESTAMPS, 0, -1);
  else {
    GST_BUFFER_TIMESTAMP (outbuf) = timestamp + postproc->field_duration;
    GST_BUFFER_DURATION (outbuf) = postproc->field_duration;
  }

  if (deint && deint_refs)
    ds_add_buffer (ds, inbuf);
  postproc->use_vpp = TRUE;
  return GST_FLOW_OK;

  /* ERRORS */
error_invalid_buffer:
  {
    GST_ERROR ("failed to validate source buffer");
    return GST_FLOW_ERROR;
  }
error_create_buffer:
  {
    GST_ERROR ("failed to create output buffer");
    return GST_FLOW_ERROR;
  }
error_create_meta:
  {
    GST_ERROR ("failed to create new output buffer meta");
    gst_buffer_replace (&fieldbuf, NULL);
    return GST_FLOW_ERROR;
  }
error_create_proxy:
  {
    GST_ERROR ("failed to create surface proxy from pool");
    gst_buffer_replace (&fieldbuf, NULL);
    return GST_FLOW_ERROR;
  }
error_op_deinterlace:
  {
    GST_ERROR ("failed to apply deinterlacing filter");
    gst_buffer_replace (&fieldbuf, NULL);
    return GST_FLOW_NOT_SUPPORTED;
  }
error_process_vpp:
  {
    GST_ERROR ("failed to apply VPP filters (error %d)", status);
    gst_buffer_replace (&fieldbuf, NULL);
    return GST_FLOW_ERROR;
  }
error_push_buffer:
  {
    if (ret != GST_FLOW_FLUSHING)
      GST_ERROR ("failed to push output buffer to video sink");
    return GST_FLOW_ERROR;
  }
}

static GstFlowReturn
gst_vaapipostproc_process (GstBaseTransform * trans, GstBuffer * inbuf,
    GstBuffer * outbuf)
{
  GstVaapiPostproc *const postproc = GST_VAAPIPOSTPROC (trans);
  GstVaapiVideoMeta *meta;
  GstClockTime timestamp;
  GstFlowReturn ret;
  GstBuffer *fieldbuf;
  guint fieldbuf_flags, outbuf_flags, flags;
  gboolean tff, deint;

  meta = gst_buffer_get_vaapi_video_meta (inbuf);
  if (!meta)
    goto error_invalid_buffer;

  timestamp = GST_BUFFER_TIMESTAMP (inbuf);
  tff = GST_BUFFER_FLAG_IS_SET (inbuf, GST_VIDEO_BUFFER_FLAG_TFF);
  deint = should_deinterlace_buffer (postproc, inbuf);

  flags = gst_vaapi_video_meta_get_render_flags (meta) &
      ~GST_VAAPI_PICTURE_STRUCTURE_MASK;

  /* First field */
  fieldbuf = create_output_buffer (postproc);
  if (!fieldbuf)
    goto error_create_buffer;
  append_output_buffer_metadata (postproc, fieldbuf, inbuf, 0);

  meta = gst_buffer_get_vaapi_video_meta (fieldbuf);
  fieldbuf_flags = flags;
  fieldbuf_flags |= deint ? (tff ?
      GST_VAAPI_PICTURE_STRUCTURE_TOP_FIELD :
      GST_VAAPI_PICTURE_STRUCTURE_BOTTOM_FIELD) :
      GST_VAAPI_PICTURE_STRUCTURE_FRAME;
  gst_vaapi_video_meta_set_render_flags (meta, fieldbuf_flags);

  GST_BUFFER_TIMESTAMP (fieldbuf) = timestamp;
  GST_BUFFER_DURATION (fieldbuf) = postproc->field_duration;
  ret = gst_pad_push (trans->srcpad, fieldbuf);
  if (ret != GST_FLOW_OK)
    goto error_push_buffer;

  /* Second field */
  append_output_buffer_metadata (postproc, outbuf, inbuf, 0);

  meta = gst_buffer_get_vaapi_video_meta (outbuf);
  outbuf_flags = flags;
  outbuf_flags |= deint ? (tff ?
      GST_VAAPI_PICTURE_STRUCTURE_BOTTOM_FIELD :
      GST_VAAPI_PICTURE_STRUCTURE_TOP_FIELD) :
      GST_VAAPI_PICTURE_STRUCTURE_FRAME;
  gst_vaapi_video_meta_set_render_flags (meta, outbuf_flags);

  GST_BUFFER_TIMESTAMP (outbuf) = timestamp + postproc->field_duration;
  GST_BUFFER_DURATION (outbuf) = postproc->field_duration;
  return GST_FLOW_OK;

  /* ERRORS */
error_invalid_buffer:
  {
    GST_ERROR ("failed to validate source buffer");
    return GST_FLOW_ERROR;
  }
error_create_buffer:
  {
    GST_ERROR ("failed to create output buffer");
    return GST_FLOW_EOS;
  }
error_push_buffer:
  {
    if (ret != GST_FLOW_FLUSHING)
      GST_ERROR ("failed to push output buffer to video sink");
    return GST_FLOW_EOS;
  }
}

static GstFlowReturn
gst_vaapipostproc_passthrough (GstBaseTransform * trans, GstBuffer * inbuf,
    GstBuffer * outbuf)
{
  GstVaapiVideoMeta *meta;

  /* No video processing needed, simply copy buffer metadata */
  meta = gst_buffer_get_vaapi_video_meta (inbuf);
  if (!meta)
    goto error_invalid_buffer;

  append_output_buffer_metadata (GST_VAAPIPOSTPROC (trans), outbuf, inbuf,
      GST_BUFFER_COPY_TIMESTAMPS);
  return GST_FLOW_OK;

  /* ERRORS */
error_invalid_buffer:
  {
    GST_ERROR ("failed to validate source buffer");
    return GST_FLOW_ERROR;
  }
}

static gboolean
is_deinterlace_enabled (GstVaapiPostproc * postproc, GstVideoInfo * vip)
{
  gboolean deinterlace;

  switch (postproc->deinterlace_mode) {
    case GST_VAAPI_DEINTERLACE_MODE_AUTO:
      deinterlace = GST_VIDEO_INFO_IS_INTERLACED (vip);
      break;
    case GST_VAAPI_DEINTERLACE_MODE_INTERLACED:
      deinterlace = TRUE;
      break;
    default:
      deinterlace = FALSE;
      break;
  }
  return deinterlace;
}

static gboolean
video_info_changed (GstVideoInfo * old_vip, GstVideoInfo * new_vip)
{
  if (GST_VIDEO_INFO_FORMAT (old_vip) != GST_VIDEO_INFO_FORMAT (new_vip))
    return TRUE;
  if (GST_VIDEO_INFO_INTERLACE_MODE (old_vip) !=
      GST_VIDEO_INFO_INTERLACE_MODE (new_vip))
    return TRUE;
  if (GST_VIDEO_INFO_WIDTH (old_vip) != GST_VIDEO_INFO_WIDTH (new_vip))
    return TRUE;
  if (GST_VIDEO_INFO_HEIGHT (old_vip) != GST_VIDEO_INFO_HEIGHT (new_vip))
    return TRUE;
  return FALSE;
}

static gboolean
gst_vaapipostproc_update_sink_caps (GstVaapiPostproc * postproc, GstCaps * caps,
    gboolean * caps_changed_ptr)
{
  GstVideoInfo vi;
  gboolean deinterlace;

  GST_INFO_OBJECT (postproc, "new sink caps = %" GST_PTR_FORMAT, caps);

  if (!gst_video_info_from_caps (&vi, caps))
    return FALSE;

  if (video_info_changed (&vi, &postproc->sinkpad_info))
    postproc->sinkpad_info = vi, *caps_changed_ptr = TRUE;

  deinterlace = is_deinterlace_enabled (postproc, &vi);
  if (deinterlace)
    postproc->flags |= GST_VAAPI_POSTPROC_FLAG_DEINTERLACE;
  postproc->field_duration = GST_VIDEO_INFO_FPS_N (&vi) > 0 ?
      gst_util_uint64_scale (GST_SECOND, GST_VIDEO_INFO_FPS_D (&vi),
      (1 + deinterlace) * GST_VIDEO_INFO_FPS_N (&vi)) : 0;

  postproc->get_va_surfaces = gst_caps_has_vaapi_surface (caps);
  return TRUE;
}

static gboolean
gst_vaapipostproc_update_src_caps (GstVaapiPostproc * postproc, GstCaps * caps,
    gboolean * caps_changed_ptr)
{
  GstVideoInfo vi;

  GST_INFO_OBJECT (postproc, "new src caps = %" GST_PTR_FORMAT, caps);

  if (!gst_video_info_from_caps (&vi, caps))
    return FALSE;

  if (video_info_changed (&vi, &postproc->srcpad_info))
    postproc->srcpad_info = vi, *caps_changed_ptr = TRUE;

  if (postproc->format != GST_VIDEO_INFO_FORMAT (&postproc->sinkpad_info) &&
      postproc->format != DEFAULT_FORMAT)
    postproc->flags |= GST_VAAPI_POSTPROC_FLAG_FORMAT;

  if ((postproc->width || postproc->height) &&
      postproc->width != GST_VIDEO_INFO_WIDTH (&postproc->sinkpad_info) &&
      postproc->height != GST_VIDEO_INFO_HEIGHT (&postproc->sinkpad_info))
    postproc->flags |= GST_VAAPI_POSTPROC_FLAG_SIZE;
  return TRUE;
}

static gboolean
ensure_allowed_sinkpad_caps (GstVaapiPostproc * postproc)
{
  GstCaps *out_caps, *raw_caps;

  if (postproc->allowed_sinkpad_caps)
    return TRUE;

  /* Create VA caps */
  out_caps = gst_caps_from_string (GST_VAAPIPOSTPROC_SURFACE_CAPS ", "
      GST_CAPS_INTERLACED_MODES);
  if (!out_caps) {
    GST_ERROR ("failed to create VA sink caps");
    return FALSE;
  }

  /* Append raw video caps */
  if (gst_vaapipostproc_ensure_uploader (postproc)) {
    raw_caps = GST_VAAPI_PLUGIN_BASE_UPLOADER_CAPS (postproc);
    if (raw_caps) {
      out_caps = gst_caps_make_writable (out_caps);
      gst_caps_append (out_caps, gst_caps_copy (raw_caps));
    } else
      GST_WARNING ("failed to create YUV sink caps");
  }
  postproc->allowed_sinkpad_caps = out_caps;

  /* XXX: append VA/VPP filters */
  return TRUE;
}

/* Fixup output caps so that to reflect the supported set of pixel formats */
static GstCaps *
expand_allowed_srcpad_caps (GstVaapiPostproc * postproc, GstCaps * caps)
{
  GValue value = G_VALUE_INIT, v_format = G_VALUE_INIT;
  guint i, num_structures;

  if (postproc->filter == NULL)
    goto cleanup;
  if (!gst_vaapipostproc_ensure_filter_caps (postproc))
    goto cleanup;

  /* Reset "format" field for each structure */
  if (!gst_vaapi_value_set_format_list (&value, postproc->filter_formats))
    goto cleanup;
  if (gst_vaapi_value_set_format (&v_format, GST_VIDEO_FORMAT_ENCODED)) {
    gst_value_list_prepend_value (&value, &v_format);
    g_value_unset (&v_format);
  }

  num_structures = gst_caps_get_size (caps);
  for (i = 0; i < num_structures; i++) {
    GstCapsFeatures *const features = gst_caps_get_features (caps, i);
    if (gst_caps_features_contains (features,
            GST_CAPS_FEATURE_META_GST_VIDEO_GL_TEXTURE_UPLOAD_META))
      continue;

    GstStructure *const structure = gst_caps_get_structure (caps, i);
    if (!structure)
      continue;
    gst_structure_set_value (structure, "format", &value);
  }
  g_value_unset (&value);

cleanup:
  return caps;
}

static gboolean
ensure_allowed_srcpad_caps (GstVaapiPostproc * postproc)
{
  GstCaps *out_caps;

  if (postproc->allowed_srcpad_caps)
    return TRUE;

  /* Create initial caps from pad template */
  out_caps = gst_caps_from_string (gst_vaapipostproc_src_caps_str);
  if (!out_caps) {
    GST_ERROR ("failed to create VA src caps");
    return FALSE;
  }

  postproc->allowed_srcpad_caps =
      expand_allowed_srcpad_caps (postproc, out_caps);
  return postproc->allowed_srcpad_caps != NULL;
}

static void
find_best_size (GstVaapiPostproc * postproc, GstVideoInfo * vip,
    guint * width_ptr, guint * height_ptr)
{
  guint width, height;

  width = GST_VIDEO_INFO_WIDTH (vip);
  height = GST_VIDEO_INFO_HEIGHT (vip);
  if (postproc->width && postproc->height) {
    width = postproc->width;
    height = postproc->height;
  } else if (postproc->keep_aspect) {
    const gdouble ratio = (gdouble) width / height;
    if (postproc->width) {
      width = postproc->width;
      height = postproc->width / ratio;
    } else if (postproc->height) {
      height = postproc->height;
      width = postproc->height * ratio;
    }
  } else if (postproc->width)
    width = postproc->width;
  else if (postproc->height)
    height = postproc->height;

  *width_ptr = width;
  *height_ptr = height;
}

static GstCaps *
gst_vaapipostproc_transform_caps_impl (GstBaseTransform * trans,
    GstPadDirection direction, GstCaps * caps)
{
  GstVaapiPostproc *const postproc = GST_VAAPIPOSTPROC (trans);
  GstVideoInfo vi;
  GstVideoFormat out_format;
  GstCaps *out_caps;
  GstVaapiCapsFeature feature;
  const gchar *feature_str;
  guint width, height;

  /* Generate the sink pad caps, that could be fixated afterwards */
  if (direction == GST_PAD_SRC) {
    if (!ensure_allowed_sinkpad_caps (postproc))
      return NULL;
    return gst_caps_ref (postproc->allowed_sinkpad_caps);
  }

  /* Generate complete set of src pad caps if non-fixated sink pad
     caps are provided */
  if (!gst_caps_is_fixed (caps)) {
    if (!ensure_allowed_srcpad_caps (postproc))
      return NULL;
    return gst_caps_ref (postproc->allowed_srcpad_caps);
  }

  /* Generate the expected src pad caps, from the current fixated
     sink pad caps */
  if (!gst_video_info_from_caps (&vi, caps))
    return NULL;

  // Set double framerate in interlaced mode
  if (is_deinterlace_enabled (postproc, &vi)) {
    gint fps_n = GST_VIDEO_INFO_FPS_N (&vi);
    gint fps_d = GST_VIDEO_INFO_FPS_D (&vi);
    if (!gst_util_fraction_multiply (fps_n, fps_d, 2, 1, &fps_n, &fps_d))
      return NULL;
    GST_VIDEO_INFO_FPS_N (&vi) = fps_n;
    GST_VIDEO_INFO_FPS_D (&vi) = fps_d;
  }

  // Signal the other pad that we only generate progressive frames
  GST_VIDEO_INFO_INTERLACE_MODE (&vi) = GST_VIDEO_INTERLACE_MODE_PROGRESSIVE;

  // Update size from user-specified parameters
  find_best_size (postproc, &vi, &width, &height);

  // Update format from user-specified parameters
  /* XXX: this is a workaround until auto-plugging is fixed when
   * format=ENCODED + memory:VASurface caps feature are provided.
   * use the downstream negotiated video format as the output format
   * if the user didn't explicitly ask for colorspace conversion.
   * Use a filter caps which contain all raw video formats, (excluding
   * GST_VIDEO_FORMAT_ENCODED) */
  if (postproc->format != DEFAULT_FORMAT)
    out_format = postproc->format;
  else {
    GstCaps *peer_caps;
    GstVideoInfo peer_vi;
    peer_caps =
        gst_pad_peer_query_caps (GST_BASE_TRANSFORM_SRC_PAD (trans),
        postproc->allowed_srcpad_caps);
    if (gst_caps_is_empty (peer_caps))
      return peer_caps;
    if (!gst_caps_is_fixed (peer_caps))
      peer_caps = gst_caps_fixate (peer_caps);
    gst_video_info_from_caps (&peer_vi, peer_caps);
    out_format = GST_VIDEO_INFO_FORMAT (&peer_vi);
    postproc->format = out_format;
    if (peer_caps)
      gst_caps_unref (peer_caps);
  }

  feature =
      gst_vaapi_find_preferred_caps_feature (GST_BASE_TRANSFORM_SRC_PAD (trans),
      out_format, &out_format);
  gst_video_info_change_format (&vi, out_format, width, height);

  out_caps = gst_video_info_to_caps (&vi);
  if (!out_caps)
    return NULL;

  if (feature) {
    feature_str = gst_vaapi_caps_feature_to_string (feature);
    if (feature_str)
      gst_caps_set_features (out_caps, 0,
          gst_caps_features_new (feature_str, NULL));
  }
  return out_caps;
}

static GstCaps *
gst_vaapipostproc_transform_caps (GstBaseTransform * trans,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter)
{
  GstCaps *out_caps;

  caps = gst_vaapipostproc_transform_caps_impl (trans, direction, caps);
  if (caps && filter) {
    out_caps = gst_caps_intersect_full (caps, filter, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (caps);
    return out_caps;
  }
  return caps;
}

static gboolean
gst_vaapipostproc_transform_size (GstBaseTransform * trans,
    GstPadDirection direction, GstCaps * caps, gsize size,
    GstCaps * othercaps, gsize * othersize)
{
  GstVaapiPostproc *const postproc = GST_VAAPIPOSTPROC (trans);

  if (direction == GST_PAD_SINK || postproc->get_va_surfaces)
    *othersize = 0;
  else
    *othersize = size;
  return TRUE;
}

static GstFlowReturn
gst_vaapipostproc_transform (GstBaseTransform * trans, GstBuffer * inbuf,
    GstBuffer * outbuf)
{
  GstVaapiPostproc *const postproc = GST_VAAPIPOSTPROC (trans);
  GstBuffer *buf;
  GstFlowReturn ret;

  ret =
      gst_vaapi_plugin_base_get_input_buffer (GST_VAAPI_PLUGIN_BASE (postproc),
      inbuf, &buf);
  if (ret != GST_FLOW_OK)
    return GST_FLOW_ERROR;

  ret = GST_FLOW_NOT_SUPPORTED;
  if (postproc->flags) {
    /* Use VA/VPP extensions to process this frame */
    if (postproc->has_vpp &&
        (postproc->flags != GST_VAAPI_POSTPROC_FLAG_DEINTERLACE ||
            deint_method_is_advanced (postproc->deinterlace_method))) {
      ret = gst_vaapipostproc_process_vpp (trans, buf, outbuf);
      if (ret != GST_FLOW_NOT_SUPPORTED)
        goto done;
      GST_WARNING ("unsupported VPP filters. Disabling");
    }

    /* Only append picture structure meta data (top/bottom field) */
    if (postproc->flags & GST_VAAPI_POSTPROC_FLAG_DEINTERLACE) {
      ret = gst_vaapipostproc_process (trans, buf, outbuf);
      if (ret != GST_FLOW_NOT_SUPPORTED)
        goto done;
    }
  }

  /* Fallback: passthrough to the downstream element as is */
  ret = gst_vaapipostproc_passthrough (trans, buf, outbuf);

done:
  gst_buffer_unref (buf);
  return ret;
}

static GstFlowReturn
gst_vaapipostproc_prepare_output_buffer (GstBaseTransform * trans,
    GstBuffer * inbuf,
    GstBuffer ** outbuf_ptr)
{
  GstVaapiPostproc *const postproc = GST_VAAPIPOSTPROC (trans);

  *outbuf_ptr = create_output_buffer (postproc);
  return *outbuf_ptr ? GST_FLOW_OK : GST_FLOW_ERROR;
}

static gboolean
ensure_srcpad_buffer_pool (GstVaapiPostproc * postproc, GstCaps * caps)
{
  GstVideoInfo vi;
  GstVaapiVideoPool *pool;

  gst_video_info_init (&vi);
  gst_video_info_from_caps (&vi, caps);
  gst_video_info_change_format (&vi, postproc->format,
      GST_VIDEO_INFO_WIDTH (&vi), GST_VIDEO_INFO_HEIGHT (&vi));

  if (postproc->filter_pool
      && !video_info_changed (&vi, &postproc->filter_pool_info))
    return TRUE;
  postproc->filter_pool_info = vi;

  pool =
      gst_vaapi_surface_pool_new_full (GST_VAAPI_PLUGIN_BASE_DISPLAY (postproc),
      &postproc->filter_pool_info, 0);
  if (!pool)
    return FALSE;

  gst_vaapi_video_pool_replace (&postproc->filter_pool, pool);
  gst_vaapi_video_pool_unref (pool);
  return TRUE;
}

static gboolean
is_native_video_format (GstVideoFormat format)
{
  guint i = 0;
  for (i = 0; i < G_N_ELEMENTS (native_formats); i++)
    if (native_formats[i] == format)
      return TRUE;
  return FALSE;
}

static gboolean
gst_vaapipostproc_set_caps (GstBaseTransform * trans, GstCaps * caps,
    GstCaps * out_caps)
{
  GstVaapiPostproc *const postproc = GST_VAAPIPOSTPROC (trans);
  gboolean caps_changed = FALSE;
  GstVideoInfo vinfo;

  if (!gst_vaapipostproc_update_sink_caps (postproc, caps, &caps_changed))
    return FALSE;
  /* HACK: This is a workaround to deal with the va-intel-driver for non-native
   * formats while doing advanced deinterlacing. The format of reference surfaces must
   * be same as the format used by the driver internally for motion adaptive
   * deinterlacing and motion compensated deinterlacing */
  gst_video_info_from_caps (&vinfo, caps);
  if (deint_method_is_advanced (postproc->deinterlace_method)
      && !is_native_video_format (GST_VIDEO_INFO_FORMAT (&vinfo))) {
    GST_WARNING_OBJECT (postproc,
        "Advanced deinterlacing requires the native video formats used by the driver internally");
    return FALSE;
  }
  if (!gst_vaapipostproc_update_src_caps (postproc, out_caps, &caps_changed))
    return FALSE;

  if (caps_changed) {
    gst_vaapipostproc_destroy (postproc);
    if (!gst_vaapipostproc_create (postproc))
      return FALSE;
    if (!gst_vaapi_plugin_base_set_caps (GST_VAAPI_PLUGIN_BASE (trans),
            caps, out_caps))
      return FALSE;
  }

  if (!ensure_srcpad_buffer_pool (postproc, out_caps))
    return FALSE;
  return TRUE;
}

static gboolean
gst_vaapipostproc_query (GstBaseTransform * trans, GstPadDirection direction,
    GstQuery * query)
{
  GstVaapiPostproc *const postproc = GST_VAAPIPOSTPROC (trans);

  if (gst_vaapi_reply_to_query (query,
          GST_VAAPI_PLUGIN_BASE_DISPLAY (postproc))) {
    GST_DEBUG_OBJECT (postproc, "sharing display %p",
        GST_VAAPI_PLUGIN_BASE_DISPLAY (postproc));
    return TRUE;
  }

  return
      GST_BASE_TRANSFORM_CLASS (gst_vaapipostproc_parent_class)->query (trans,
      direction, query);
}

static gboolean
gst_vaapipostproc_propose_allocation (GstBaseTransform * trans,
    GstQuery * decide_query, GstQuery * query)
{
  GstVaapiPostproc *const postproc = GST_VAAPIPOSTPROC (trans);
  GstVaapiPluginBase *const plugin = GST_VAAPI_PLUGIN_BASE (trans);

  /* Let vaapidecode allocate the video buffers */
  if (postproc->get_va_surfaces)
    return FALSE;
  if (!gst_vaapi_plugin_base_propose_allocation (plugin, query))
    return FALSE;
  return TRUE;
}

static gboolean
gst_vaapipostproc_decide_allocation (GstBaseTransform * trans, GstQuery * query)
{
  return gst_vaapi_plugin_base_decide_allocation (GST_VAAPI_PLUGIN_BASE (trans),
      query, 0);
}

static void
gst_vaapipostproc_finalize (GObject * object)
{
  GstVaapiPostproc *const postproc = GST_VAAPIPOSTPROC (object);

  gst_vaapipostproc_destroy (postproc);

  gst_vaapi_plugin_base_finalize (GST_VAAPI_PLUGIN_BASE (postproc));
  G_OBJECT_CLASS (gst_vaapipostproc_parent_class)->finalize (object);
}

static void
gst_vaapipostproc_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstVaapiPostproc *const postproc = GST_VAAPIPOSTPROC (object);

  switch (prop_id) {
    case PROP_FORMAT:
      postproc->format = g_value_get_enum (value);
      break;
    case PROP_WIDTH:
      postproc->width = g_value_get_uint (value);
      break;
    case PROP_HEIGHT:
      postproc->height = g_value_get_uint (value);
      break;
    case PROP_FORCE_ASPECT_RATIO:
      postproc->keep_aspect = g_value_get_boolean (value);
      break;
    case PROP_DEINTERLACE_MODE:
      postproc->deinterlace_mode = g_value_get_enum (value);
      break;
    case PROP_DEINTERLACE_METHOD:
      postproc->deinterlace_method = g_value_get_enum (value);
      break;
    case PROP_DENOISE:
      postproc->denoise_level = g_value_get_float (value);
      postproc->flags |= GST_VAAPI_POSTPROC_FLAG_DENOISE;
      break;
    case PROP_SHARPEN:
      postproc->sharpen_level = g_value_get_float (value);
      postproc->flags |= GST_VAAPI_POSTPROC_FLAG_SHARPEN;
      break;
    case PROP_HUE:
      postproc->hue = g_value_get_float (value);
      postproc->flags |= GST_VAAPI_POSTPROC_FLAG_HUE;
      break;
    case PROP_SATURATION:
      postproc->saturation = g_value_get_float (value);
      postproc->flags |= GST_VAAPI_POSTPROC_FLAG_SATURATION;
      break;
    case PROP_BRIGHTNESS:
      postproc->brightness = g_value_get_float (value);
      postproc->flags |= GST_VAAPI_POSTPROC_FLAG_BRIGHTNESS;
      break;
    case PROP_CONTRAST:
      postproc->contrast = g_value_get_float (value);
      postproc->flags |= GST_VAAPI_POSTPROC_FLAG_CONTRAST;
      break;
    case PROP_SCALE_METHOD:
      postproc->scale_method = g_value_get_enum (value);
      postproc->flags |= GST_VAAPI_POSTPROC_FLAG_SCALE;
      break;
    case PROP_SKIN_TONE_ENHANCEMENT:
      postproc->skintone_enhance = g_value_get_boolean (value);
      postproc->flags |= GST_VAAPI_POSTPROC_FLAG_SKINTONE;
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_vaapipostproc_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstVaapiPostproc *const postproc = GST_VAAPIPOSTPROC (object);

  switch (prop_id) {
    case PROP_FORMAT:
      g_value_set_enum (value, postproc->format);
      break;
    case PROP_WIDTH:
      g_value_set_uint (value, postproc->width);
      break;
    case PROP_HEIGHT:
      g_value_set_uint (value, postproc->height);
      break;
    case PROP_FORCE_ASPECT_RATIO:
      g_value_set_boolean (value, postproc->keep_aspect);
      break;
    case PROP_DEINTERLACE_MODE:
      g_value_set_enum (value, postproc->deinterlace_mode);
      break;
    case PROP_DEINTERLACE_METHOD:
      g_value_set_enum (value, postproc->deinterlace_method);
      break;
    case PROP_DENOISE:
      g_value_set_float (value, postproc->denoise_level);
      break;
    case PROP_SHARPEN:
      g_value_set_float (value, postproc->sharpen_level);
      break;
    case PROP_HUE:
      g_value_set_float (value, postproc->hue);
      break;
    case PROP_SATURATION:
      g_value_set_float (value, postproc->saturation);
      break;
    case PROP_BRIGHTNESS:
      g_value_set_float (value, postproc->brightness);
      break;
    case PROP_CONTRAST:
      g_value_set_float (value, postproc->contrast);
      break;
    case PROP_SCALE_METHOD:
      g_value_set_enum (value, postproc->scale_method);
      break;
    case PROP_SKIN_TONE_ENHANCEMENT:
      g_value_set_boolean (value, postproc->skintone_enhance);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_vaapipostproc_class_init (GstVaapiPostprocClass * klass)
{
  GObjectClass *const object_class = G_OBJECT_CLASS (klass);
  GstElementClass *const element_class = GST_ELEMENT_CLASS (klass);
  GstBaseTransformClass *const trans_class = GST_BASE_TRANSFORM_CLASS (klass);
  GstPadTemplate *pad_template;
  GPtrArray *filter_ops;
  GstVaapiFilterOpInfo *filter_op;

  GST_DEBUG_CATEGORY_INIT (gst_debug_vaapipostproc,
      GST_PLUGIN_NAME, 0, GST_PLUGIN_DESC);

  gst_vaapi_plugin_base_class_init (GST_VAAPI_PLUGIN_BASE_CLASS (klass));

  object_class->finalize = gst_vaapipostproc_finalize;
  object_class->set_property = gst_vaapipostproc_set_property;
  object_class->get_property = gst_vaapipostproc_get_property;
  trans_class->start = gst_vaapipostproc_start;
  trans_class->stop = gst_vaapipostproc_stop;
  trans_class->transform_caps = gst_vaapipostproc_transform_caps;
  trans_class->transform_size = gst_vaapipostproc_transform_size;
  trans_class->transform = gst_vaapipostproc_transform;
  trans_class->set_caps = gst_vaapipostproc_set_caps;
  trans_class->query = gst_vaapipostproc_query;
  trans_class->propose_allocation = gst_vaapipostproc_propose_allocation;
  trans_class->decide_allocation = gst_vaapipostproc_decide_allocation;

  trans_class->prepare_output_buffer = gst_vaapipostproc_prepare_output_buffer;

  gst_element_class_set_static_metadata (element_class,
      "VA-API video postprocessing",
      "Filter/Converter/Video;Filter/Converter/Video/Scaler;"
      "Filter/Effect/Video;Filter/Effect/Video/Deinterlace",
      GST_PLUGIN_DESC, "Gwenole Beauchesne <gwenole.beauchesne@intel.com>");

  /* sink pad */
  pad_template = gst_static_pad_template_get (&gst_vaapipostproc_sink_factory);
  gst_element_class_add_pad_template (element_class, pad_template);

  /* src pad */
  pad_template = gst_static_pad_template_get (&gst_vaapipostproc_src_factory);
  gst_element_class_add_pad_template (element_class, pad_template);

  /**
   * GstVaapiPostproc:deinterlace-mode:
   *
   * This selects whether the deinterlacing should always be applied
   * or if they should only be applied on content that has the
   * "interlaced" flag on the caps.
   */
  g_object_class_install_property
      (object_class,
      PROP_DEINTERLACE_MODE,
      g_param_spec_enum ("deinterlace-mode",
          "Deinterlace mode",
          "Deinterlace mode to use",
          GST_VAAPI_TYPE_DEINTERLACE_MODE,
          DEFAULT_DEINTERLACE_MODE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstVaapiPostproc:deinterlace-method:
   *
   * This selects the deinterlacing method to apply.
   */
  g_object_class_install_property
      (object_class,
      PROP_DEINTERLACE_METHOD,
      g_param_spec_enum ("deinterlace-method",
          "Deinterlace method",
          "Deinterlace method to use",
          GST_VAAPI_TYPE_DEINTERLACE_METHOD,
          DEFAULT_DEINTERLACE_METHOD,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  filter_ops = gst_vaapi_filter_get_operations (NULL);
  if (!filter_ops)
    return;

  /**
   * GstVaapiPostproc:format:
   *
   * The forced output pixel format, expressed as a #GstVideoFormat.
   */
  filter_op = find_filter_op (filter_ops, GST_VAAPI_FILTER_OP_FORMAT);
  if (filter_op)
    g_object_class_install_property (object_class,
        PROP_FORMAT, filter_op->pspec);

  /**
   * GstVaapiPostproc:width:
   *
   * The forced output width in pixels. If set to zero, the width is
   * calculated from the height if aspect ration is preserved, or
   * inherited from the sink caps width
   */
  g_object_class_install_property
      (object_class,
      PROP_WIDTH,
      g_param_spec_uint ("width",
          "Width",
          "Forced output width",
          0, G_MAXINT, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstVaapiPostproc:height:
   *
   * The forced output height in pixels. If set to zero, the height is
   * calculated from the width if aspect ration is preserved, or
   * inherited from the sink caps height
   */
  g_object_class_install_property
      (object_class,
      PROP_HEIGHT,
      g_param_spec_uint ("height",
          "Height",
          "Forced output height",
          0, G_MAXINT, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstVaapiPostproc:force-aspect-ratio:
   *
   * When enabled, scaling respects video aspect ratio; when disabled,
   * the video is distorted to fit the width and height properties.
   */
  g_object_class_install_property
      (object_class,
      PROP_FORCE_ASPECT_RATIO,
      g_param_spec_boolean ("force-aspect-ratio",
          "Force aspect ratio",
          "When enabled, scaling will respect original aspect ratio",
          TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstVaapiPostproc:denoise:
   *
   * The level of noise reduction to apply.
   */
  filter_op = find_filter_op (filter_ops, GST_VAAPI_FILTER_OP_DENOISE);
  if (filter_op)
    g_object_class_install_property (object_class,
        PROP_DENOISE, filter_op->pspec);

  /**
   * GstVaapiPostproc:sharpen:
   *
   * The level of sharpening to apply for positive values, or the
   * level of blurring for negative values.
   */
  filter_op = find_filter_op (filter_ops, GST_VAAPI_FILTER_OP_SHARPEN);
  if (filter_op)
    g_object_class_install_property (object_class,
        PROP_SHARPEN, filter_op->pspec);

  /**
   * GstVaapiPostproc:hue:
   *
   * The color hue, expressed as a float value. Range is -180.0 to
   * 180.0. Default value is 0.0 and represents no modification.
   */
  filter_op = find_filter_op (filter_ops, GST_VAAPI_FILTER_OP_HUE);
  if (filter_op)
    g_object_class_install_property (object_class, PROP_HUE, filter_op->pspec);

  /**
   * GstVaapiPostproc:saturation:
   *
   * The color saturation, expressed as a float value. Range is 0.0 to
   * 2.0. Default value is 1.0 and represents no modification.
   */
  filter_op = find_filter_op (filter_ops, GST_VAAPI_FILTER_OP_SATURATION);
  if (filter_op)
    g_object_class_install_property (object_class,
        PROP_SATURATION, filter_op->pspec);

  /**
   * GstVaapiPostproc:brightness:
   *
   * The color brightness, expressed as a float value. Range is -1.0
   * to 1.0. Default value is 0.0 and represents no modification.
   */
  filter_op = find_filter_op (filter_ops, GST_VAAPI_FILTER_OP_BRIGHTNESS);
  if (filter_op)
    g_object_class_install_property (object_class,
        PROP_BRIGHTNESS, filter_op->pspec);

  /**
   * GstVaapiPostproc:contrast:
   *
   * The color contrast, expressed as a float value. Range is 0.0 to
   * 2.0. Default value is 1.0 and represents no modification.
   */
  filter_op = find_filter_op (filter_ops, GST_VAAPI_FILTER_OP_CONTRAST);
  if (filter_op)
    g_object_class_install_property (object_class,
        PROP_CONTRAST, filter_op->pspec);

  /**
   * GstVaapiPostproc:scale-method:
   *
   * The scaling method to use, expressed as an enum value. See
   * #GstVaapiScaleMethod.
   */
  filter_op = find_filter_op (filter_ops, GST_VAAPI_FILTER_OP_SCALING);
  if (filter_op)
    g_object_class_install_property (object_class,
        PROP_SCALE_METHOD, filter_op->pspec);

  /**
   * GstVaapiPostproc:skin-tone-enhancement:
   *
   * Apply the skin tone enhancement algorithm.
   */
  filter_op = find_filter_op (filter_ops, GST_VAAPI_FILTER_OP_SKINTONE);
  if (filter_op)
    g_object_class_install_property (object_class,
        PROP_SKIN_TONE_ENHANCEMENT, filter_op->pspec);

  g_ptr_array_unref (filter_ops);
}

static float *
find_value_ptr (GstVaapiPostproc * postproc, GstVaapiFilterOp op)
{
  switch (op) {
    case GST_VAAPI_FILTER_OP_HUE:
      return &postproc->hue;
    case GST_VAAPI_FILTER_OP_SATURATION:
      return &postproc->saturation;
    case GST_VAAPI_FILTER_OP_BRIGHTNESS:
      return &postproc->brightness;
    case GST_VAAPI_FILTER_OP_CONTRAST:
      return &postproc->contrast;
    default:
      return NULL;
  }
}

static void
cb_set_default_value (GstVaapiPostproc * postproc, GPtrArray * filter_ops,
    GstVaapiFilterOp op)
{
  GstVaapiFilterOpInfo *filter_op;
  GParamSpecFloat *pspec;
  float *var;

  filter_op = find_filter_op (filter_ops, op);
  if (!filter_op)
    return;
  var = find_value_ptr (postproc, op);
  if (!var)
    return;
  pspec = G_PARAM_SPEC_FLOAT (filter_op->pspec);
  *var = pspec->default_value;
}

static void
gst_vaapipostproc_init (GstVaapiPostproc * postproc)
{
  GPtrArray *filter_ops;
  guint i;

  gst_vaapi_plugin_base_init (GST_VAAPI_PLUGIN_BASE (postproc),
      GST_CAT_DEFAULT);

  postproc->format = DEFAULT_FORMAT;
  postproc->deinterlace_mode = DEFAULT_DEINTERLACE_MODE;
  postproc->deinterlace_method = DEFAULT_DEINTERLACE_METHOD;
  postproc->field_duration = GST_CLOCK_TIME_NONE;
  postproc->keep_aspect = TRUE;
  postproc->get_va_surfaces = TRUE;

  filter_ops = gst_vaapi_filter_get_operations (NULL);
  if (filter_ops) {
    for (i = GST_VAAPI_FILTER_OP_HUE; i <= GST_VAAPI_FILTER_OP_CONTRAST; i++)
      cb_set_default_value (postproc, filter_ops, i);
    g_ptr_array_unref (filter_ops);
  }

  gst_video_info_init (&postproc->sinkpad_info);
  gst_video_info_init (&postproc->srcpad_info);
  gst_video_info_init (&postproc->filter_pool_info);
}

/* ------------------------------------------------------------------------ */
/* --- GstColorBalance interface                                        --- */
/* ------------------------------------------------------------------------ */

#define CB_CHANNEL_FACTOR 1000.0

typedef struct
{
  GstVaapiFilterOp op;
  const gchar *name;
} ColorBalanceChannel;

ColorBalanceChannel cb_channels[] = {
  {GST_VAAPI_FILTER_OP_HUE, "VA_FILTER_HUE"},
  {GST_VAAPI_FILTER_OP_SATURATION, "VA_FILTER_SATURATION"},
  {GST_VAAPI_FILTER_OP_BRIGHTNESS, "VA_FILTER_BRIGHTNESS"},
  {GST_VAAPI_FILTER_OP_CONTRAST, "VA_FILTER_CONTRAST"},
};

static void
cb_channels_init (GstVaapiPostproc * postproc)
{
  GPtrArray *filter_ops;
  GstVaapiFilterOpInfo *filter_op;
  GParamSpecFloat *pspec;
  GstColorBalanceChannel *channel;
  guint i;

  if (postproc->cb_channels)
    return;

  if (!gst_vaapipostproc_ensure_filter (postproc))
    return;

  filter_ops = postproc->filter_ops ? g_ptr_array_ref (postproc->filter_ops)
      : gst_vaapi_filter_get_operations (postproc->filter);
  if (!filter_ops)
    return;

  for (i = 0; i < G_N_ELEMENTS (cb_channels); i++) {
    filter_op = find_filter_op (filter_ops, cb_channels[i].op);
    if (!filter_op)
      continue;

    pspec = G_PARAM_SPEC_FLOAT (filter_op->pspec);
    channel = g_object_new (GST_TYPE_COLOR_BALANCE_CHANNEL, NULL);
    channel->label = g_strdup (cb_channels[i].name);
    channel->min_value = pspec->minimum * CB_CHANNEL_FACTOR;
    channel->max_value = pspec->maximum * CB_CHANNEL_FACTOR;

    postproc->cb_channels = g_list_prepend (postproc->cb_channels, channel);
  }

  g_ptr_array_unref (filter_ops);
}

static const GList *
gst_vaapipostproc_colorbalance_list_channels (GstColorBalance * balance)
{
  GstVaapiPostproc *const postproc = GST_VAAPIPOSTPROC (balance);

  cb_channels_init (postproc);
  return postproc->cb_channels;
}

static gfloat *
cb_get_value_ptr (GstVaapiPostproc * postproc, GstColorBalanceChannel * channel,
    GstVaapiPostprocFlags * flags)
{
  guint i;
  gfloat *ret = NULL;

  for (i = 0; i < G_N_ELEMENTS (cb_channels); i++) {
    if (g_ascii_strcasecmp (cb_channels[i].name, channel->label) == 0)
      break;
  }
  if (i >= G_N_ELEMENTS (cb_channels))
    return NULL;

  ret = find_value_ptr (postproc, cb_channels[i].op);
  if (flags)
    *flags = 1 << cb_channels[i].op;
  return ret;
}

static void
gst_vaapipostproc_colorbalance_set_value (GstColorBalance * balance,
    GstColorBalanceChannel * channel, gint value)
{
  GstVaapiPostproc *const postproc = GST_VAAPIPOSTPROC (balance);
  GstVaapiPostprocFlags flags;
  gfloat new_val, *var;

  value = CLAMP (value, channel->min_value, channel->max_value);
  new_val = (gfloat) value / CB_CHANNEL_FACTOR;

  var = cb_get_value_ptr (postproc, channel, &flags);
  if (var) {
    *var = new_val;
    postproc->flags |= flags;
    gst_color_balance_value_changed (balance, channel, value);
    return;
  }

  GST_WARNING_OBJECT (postproc, "unknown channel %s", channel->label);
}

static gint
gst_vaapipostproc_colorbalance_get_value (GstColorBalance * balance,
    GstColorBalanceChannel * channel)
{
  GstVaapiPostproc *const postproc = GST_VAAPIPOSTPROC (balance);
  gfloat *var;
  gint new_val;

  var = cb_get_value_ptr (postproc, channel, NULL);
  if (var) {
    new_val = (gint) ((*var) * CB_CHANNEL_FACTOR);
    new_val = CLAMP (new_val, channel->min_value, channel->max_value);
    return new_val;
  }

  GST_WARNING_OBJECT (postproc, "unknown channel %s", channel->label);
  return G_MININT;
}

static GstColorBalanceType
gst_vaapipostproc_colorbalance_get_balance_type (GstColorBalance * balance)
{
  return GST_COLOR_BALANCE_HARDWARE;
}

static void
gst_vaapipostproc_colorbalance_init (gpointer iface, gpointer data)
{
  GstColorBalanceInterface *cbface = iface;
  cbface->list_channels = gst_vaapipostproc_colorbalance_list_channels;
  cbface->set_value = gst_vaapipostproc_colorbalance_set_value;
  cbface->get_value = gst_vaapipostproc_colorbalance_get_value;
  cbface->get_balance_type = gst_vaapipostproc_colorbalance_get_balance_type;
}

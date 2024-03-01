/*
 * Copyright (C) 2011, Hewlett-Packard Development Company, L.P.
 *   Author: Sebastian Dröge <sebastian.droege@collabora.co.uk>, Collabora Ltd.
 * Copyright (C) 2013, Collabora Ltd.
 *   Author: Sebastian Dröge <sebastian.droege@collabora.co.uk>
 * Copyright (C) 2015-2017, Renesas Electronics Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstomxbufferpool.h"
#include "gstomxvideodec.h"
#include "gstomxvideoenc.h"
#include "gst/allocators/gstdmabuf.h"
#ifdef HAVE_MMNGRBUF
#include "mmngr_buf_user_public.h"
#endif
#ifdef HAVE_VIDEODEC_EXT
#include "OMXR_Extension_vdcmn.h"
#endif
#include <unistd.h>             /* getpagesize() */

GST_DEBUG_CATEGORY_STATIC (gst_omx_buffer_pool_debug_category);
#define GST_CAT_DEFAULT gst_omx_buffer_pool_debug_category

typedef struct _GstOMXMemory GstOMXMemory;
typedef struct _GstOMXMemoryAllocator GstOMXMemoryAllocator;
typedef struct _GstOMXMemoryAllocatorClass GstOMXMemoryAllocatorClass;

struct _GstOMXMemory
{
  GstMemory mem;

  GstOMXBuffer *buf;
};

struct _GstOMXMemoryAllocator
{
  GstAllocator parent;
};

struct _GstOMXMemoryAllocatorClass
{
  GstAllocatorClass parent_class;
};

#define GST_OMX_MEMORY_TYPE "openmax"

static GstMemory *
gst_omx_memory_allocator_alloc_dummy (GstAllocator * allocator, gsize size,
    GstAllocationParams * params)
{
  g_assert_not_reached ();
  return NULL;
}

static void
gst_omx_memory_allocator_free (GstAllocator * allocator, GstMemory * mem)
{
  GstOMXMemory *omem = (GstOMXMemory *) mem;

  /* TODO: We need to remember which memories are still used
   * so we can wait until everything is released before allocating
   * new memory
   */

  g_slice_free (GstOMXMemory, omem);
}

static gpointer
gst_omx_memory_map (GstMemory * mem, gsize maxsize, GstMapFlags flags)
{
  GstOMXMemory *omem = (GstOMXMemory *) mem;

  return omem->buf->omx_buf->pBuffer + omem->mem.offset;
}

static void
gst_omx_memory_unmap (GstMemory * mem)
{
}

static GstMemory *
gst_omx_memory_share (GstMemory * mem, gssize offset, gssize size)
{
  g_assert_not_reached ();
  return NULL;
}

GType gst_omx_memory_allocator_get_type (void);
G_DEFINE_TYPE (GstOMXMemoryAllocator, gst_omx_memory_allocator,
    GST_TYPE_ALLOCATOR);

#define GST_TYPE_OMX_MEMORY_ALLOCATOR   (gst_omx_memory_allocator_get_type())
#define GST_IS_OMX_MEMORY_ALLOCATOR(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_OMX_MEMORY_ALLOCATOR))

static void
gst_omx_memory_allocator_class_init (GstOMXMemoryAllocatorClass * klass)
{
  GstAllocatorClass *allocator_class;

  allocator_class = (GstAllocatorClass *) klass;

  allocator_class->alloc = gst_omx_memory_allocator_alloc_dummy;
  allocator_class->free = gst_omx_memory_allocator_free;
}

static void
gst_omx_memory_allocator_init (GstOMXMemoryAllocator * allocator)
{
  GstAllocator *alloc = GST_ALLOCATOR_CAST (allocator);

  alloc->mem_type = GST_OMX_MEMORY_TYPE;
  alloc->mem_map = gst_omx_memory_map;
  alloc->mem_unmap = gst_omx_memory_unmap;
  alloc->mem_share = gst_omx_memory_share;

  /* default copy & is_span */

  GST_OBJECT_FLAG_SET (allocator, GST_ALLOCATOR_FLAG_CUSTOM_ALLOC);
}

static GstMemory *
gst_omx_memory_allocator_alloc (GstAllocator * allocator, GstMemoryFlags flags,
    GstOMXBuffer * buf)
{
  GstOMXMemory *mem;
  gint align;

  /* FIXME: We don't allow sharing because we need to know
   * when the memory becomes unused and can only then put
   * it back to the pool. Which is done in the pool's release
   * function
   */
  flags |= GST_MEMORY_FLAG_NO_SHARE;

  /* GStreamer uses a bitmask for the alignment while
   * OMX uses the alignment itself. So we have to convert
   * here */
  align = buf->port->port_def.nBufferAlignment;
  if (align > 0)
    align -= 1;
  if (((align + 1) & align) != 0) {
    GST_WARNING ("Invalid alignment that is not a power of two: %u",
        (guint) buf->port->port_def.nBufferAlignment);
    align = 0;
  }

  mem = g_slice_new (GstOMXMemory);
  /* the shared memory is always readonly */
  gst_memory_init (GST_MEMORY_CAST (mem), flags, allocator, NULL,
      buf->omx_buf->nAllocLen, align, 0, buf->omx_buf->nAllocLen);

  mem->buf = buf;

  return GST_MEMORY_CAST (mem);
}

/* Buffer pool for the buffers of an OpenMAX port.
 *
 * This pool is only used if we either passed buffers from another
 * pool to the OMX port or provide the OMX buffers directly to other
 * elements.
 *
 *
 * A buffer is in the pool if it is currently owned by the port,
 * i.e. after OMX_{Fill,Empty}ThisBuffer(). A buffer is outside
 * the pool after it was taken from the port after it was handled
 * by the port, i.e. {Empty,Fill}BufferDone.
 *
 * Buffers can be allocated by us (OMX_AllocateBuffer()) or allocated
 * by someone else and (temporarily) passed to this pool
 * (OMX_UseBuffer(), OMX_UseEGLImage()). In the latter case the pool of
 * the buffer will be overriden, and restored in free_buffer(). Other
 * buffers are just freed there.
 *
 * The pool always has a fixed number of minimum and maximum buffers
 * and these are allocated while starting the pool and released afterwards.
 * They correspond 1:1 to the OMX buffers of the port, which are allocated
 * before the pool is started.
 *
 * Acquiring a buffer from this pool happens after the OMX buffer has
 * been acquired from the port. gst_buffer_pool_acquire_buffer() is
 * supposed to return the buffer that corresponds to the OMX buffer.
 *
 * For buffers provided to upstream, the buffer will be passed to
 * the component manually when it arrives and then unreffed. If the
 * buffer is released before reaching the component it will be just put
 * back into the pool as if EmptyBufferDone has happened. If it was
 * passed to the component, it will be back into the pool when it was
 * released and EmptyBufferDone has happened.
 *
 * For buffers provided to downstream, the buffer will be returned
 * back to the component (OMX_FillThisBuffer()) when it is released.
 */

static GQuark gst_omx_buffer_data_quark = 0;

#define DEBUG_INIT \
  GST_DEBUG_CATEGORY_INIT (gst_omx_buffer_pool_debug_category, "omxbufferpool", 0, \
      "debug category for gst-omx buffer pool base class");

G_DEFINE_TYPE_WITH_CODE (GstOMXBufferPool, gst_omx_buffer_pool,
    GST_TYPE_BUFFER_POOL, DEBUG_INIT);

static void gst_omx_buffer_pool_free_buffer (GstBufferPool * bpool,
    GstBuffer * buffer);

static gboolean
gst_omx_buffer_pool_start (GstBufferPool * bpool)
{
  GstOMXBufferPool *pool = GST_OMX_BUFFER_POOL (bpool);

  /* Only allow to start the pool if we still are attached
   * to a component and port */
  GST_OBJECT_LOCK (pool);
  if (!pool->component || !pool->port) {
    GST_OBJECT_UNLOCK (pool);
    return FALSE;
  }
  GST_OBJECT_UNLOCK (pool);

  return
      GST_BUFFER_POOL_CLASS (gst_omx_buffer_pool_parent_class)->start (bpool);
}

static gboolean
gst_omx_buffer_pool_stop (GstBufferPool * bpool)
{
  GstOMXBufferPool *pool = GST_OMX_BUFFER_POOL (bpool);
  gint i = 0;

  /* When not using the default GstBufferPool::GstAtomicQueue then
   * GstBufferPool::free_buffer is not called while stopping the pool
   * (because the queue is empty) */
  for (i = 0; i < pool->buffers->len; i++)
    GST_BUFFER_POOL_CLASS (gst_omx_buffer_pool_parent_class)->release_buffer
        (bpool, g_ptr_array_index (pool->buffers, i));

  /* Remove any buffers that are there */
  g_ptr_array_set_size (pool->buffers, 0);

  if (pool->caps)
    gst_caps_unref (pool->caps);
  pool->caps = NULL;

  pool->add_videometa = FALSE;

  return GST_BUFFER_POOL_CLASS (gst_omx_buffer_pool_parent_class)->stop (bpool);
}

static const gchar **
gst_omx_buffer_pool_get_options (GstBufferPool * bpool)
{
  static const gchar *raw_video_options[] =
      { GST_BUFFER_POOL_OPTION_VIDEO_META, NULL };
  static const gchar *options[] = { NULL };
  GstOMXBufferPool *pool = GST_OMX_BUFFER_POOL (bpool);

  GST_OBJECT_LOCK (pool);
  if (pool->port && pool->port->port_def.eDomain == OMX_PortDomainVideo
      && pool->port->port_def.format.video.eCompressionFormat ==
      OMX_VIDEO_CodingUnused) {
    GST_OBJECT_UNLOCK (pool);
    return raw_video_options;
  }
  GST_OBJECT_UNLOCK (pool);

  return options;
}

static gboolean
gst_omx_buffer_pool_set_config (GstBufferPool * bpool, GstStructure * config)
{
  GstOMXBufferPool *pool = GST_OMX_BUFFER_POOL (bpool);
  GstCaps *caps;

  GST_OBJECT_LOCK (pool);

  if (!gst_buffer_pool_config_get_params (config, &caps, NULL, NULL, NULL))
    goto wrong_config;

  if (caps == NULL)
    goto no_caps;

  if (pool->port && pool->port->port_def.eDomain == OMX_PortDomainVideo
      && pool->port->port_def.format.video.eCompressionFormat ==
      OMX_VIDEO_CodingUnused) {
    GstVideoInfo info;

    /* now parse the caps from the config */
    if (!gst_video_info_from_caps (&info, caps))
      goto wrong_video_caps;

    /* enable metadata based on config of the pool */
    pool->add_videometa =
        gst_buffer_pool_config_has_option (config,
        GST_BUFFER_POOL_OPTION_VIDEO_META);

    pool->video_info = info;
  }

  if (pool->caps)
    gst_caps_unref (pool->caps);
  pool->caps = gst_caps_ref (caps);

  GST_OBJECT_UNLOCK (pool);

  return GST_BUFFER_POOL_CLASS (gst_omx_buffer_pool_parent_class)->set_config
      (bpool, config);

  /* ERRORS */
wrong_config:
  {
    GST_OBJECT_UNLOCK (pool);
    GST_WARNING_OBJECT (pool, "invalid config");
    return FALSE;
  }
no_caps:
  {
    GST_OBJECT_UNLOCK (pool);
    GST_WARNING_OBJECT (pool, "no caps in config");
    return FALSE;
  }
wrong_video_caps:
  {
    GST_OBJECT_UNLOCK (pool);
    GST_WARNING_OBJECT (pool,
        "failed getting geometry from caps %" GST_PTR_FORMAT, caps);
    return FALSE;
  }
}

#if defined (HAVE_MMNGRBUF) && defined (HAVE_VIDEODEC_EXT)
static gboolean
gst_omx_buffer_pool_export_dmabuf (GstOMXBufferPool * pool,
    guint phys_addr, gint size, gint * id_export, gint * dmabuf_fd)
{
  gint res;

  res =
      mmngr_export_start_in_user_ext (id_export,
      (gsize) size, phys_addr, dmabuf_fd, NULL);
  if (res != R_MM_OK) {
    GST_ERROR_OBJECT (pool,
        "mmngr_export_start_in_user failed (phys_addr:0x%08x)", phys_addr);
    return FALSE;
  }
  GST_DEBUG_OBJECT (pool,
      "Export dmabuf:%d id_export:%d (phys_addr:0x%08x)", *dmabuf_fd,
      *id_export, phys_addr);

  return TRUE;
}

/* This function will create a GstBuffer contain dmabuf_fd of decoded
 * video got from Media Component
 */
static GstBuffer *
gst_omx_buffer_pool_create_buffer_contain_dmabuf (GstOMXBufferPool * self,
    GstOMXBuffer * omx_buf, gint * stride, gint * slice, gsize * offset)
{
  gint dmabuf_fd[GST_VIDEO_MAX_PLANES];
  gint plane_size[GST_VIDEO_MAX_PLANES];
  gint plane_size_ext[GST_VIDEO_MAX_PLANES];
  gint dmabuf_id[GST_VIDEO_MAX_PLANES];
  gint page_offset[GST_VIDEO_MAX_PLANES];
  GstBuffer *new_buf;
  gint i;
  gint page_size;

  new_buf = gst_buffer_new ();
  page_size = getpagesize ();

  GST_DEBUG_OBJECT (self, "Create dmabuf mem pBuffer=%p",
      omx_buf->omx_buf->pBuffer);

  for (i = 0; i < GST_VIDEO_INFO_N_PLANES (&self->video_info); i++) {
    guint phys_addr;
    GstMemory *mem;

    OMXR_MC_VIDEO_DECODERESULTTYPE *decode_res =
        (OMXR_MC_VIDEO_DECODERESULTTYPE *) omx_buf->omx_buf->pOutputPortPrivate;

    phys_addr = (guintptr) decode_res->pvPhysImageAddressY + offset[i];
    /* Calculate offset between physical address and page boundary */
    page_offset[i] = phys_addr & (page_size - 1);

    plane_size[i] = stride[i] * slice[i];
    GST_DEBUG_OBJECT (self, "Plane size %d: %d", i, plane_size[i]);

    /* When downstream plugins do mapping from dmabuf fd it requires
     * mapping from boundary page and size align for page size so
     * memory for plane must increase to handle for this case */
    plane_size_ext[i] = GST_ROUND_UP_N (plane_size[i] + page_offset[i],
        page_size);
    GST_DEBUG_OBJECT (self, "Plane size extend %d: %d", i, plane_size_ext[i]);

    if (!gst_omx_buffer_pool_export_dmabuf (self, phys_addr,
            plane_size_ext[i], &dmabuf_id[i], &dmabuf_fd[i])) {
      GST_ERROR_OBJECT (self, "dmabuf exporting failed");
      return NULL;
    }

    g_array_append_val (self->id_array, dmabuf_id[i]);
    /* Set offset's information */
    mem = gst_dmabuf_allocator_alloc (self->allocator, dmabuf_fd[i],
        plane_size_ext[i]);
    mem->offset = page_offset[i];
    /* Only allow to access plane size */
    mem->size = plane_size[i];
    gst_buffer_append_memory (new_buf, mem);

  }

  g_ptr_array_add (self->buffers, new_buf);
  gst_buffer_add_video_meta_full (new_buf, GST_VIDEO_FRAME_FLAG_NONE,
      GST_VIDEO_INFO_FORMAT (&self->video_info),
      GST_VIDEO_INFO_WIDTH (&self->video_info),
      GST_VIDEO_INFO_HEIGHT (&self->video_info),
      GST_VIDEO_INFO_N_PLANES (&self->video_info), offset, stride);

  return new_buf;
}
#endif

static GstFlowReturn
gst_omx_buffer_pool_alloc_buffer (GstBufferPool * bpool,
    GstBuffer ** buffer, GstBufferPoolAcquireParams * params)
{
  GstOMXBufferPool *pool = GST_OMX_BUFFER_POOL (bpool);
  GstBuffer *buf;
  GstOMXBuffer *omx_buf;

  g_return_val_if_fail (pool->allocating, GST_FLOW_ERROR);

  omx_buf = g_ptr_array_index (pool->port->buffers, pool->current_buffer_index);
  g_return_val_if_fail (omx_buf != NULL, GST_FLOW_ERROR);

  if (pool->other_pool) {
    guint i, n;

    buf = g_ptr_array_index (pool->buffers, pool->current_buffer_index);
    g_assert (pool->other_pool == buf->pool);
    gst_object_replace ((GstObject **) & buf->pool, NULL);

    n = gst_buffer_n_memory (buf);
    for (i = 0; i < n; i++) {
      GstMemory *mem = gst_buffer_peek_memory (buf, i);

      /* FIXME: We don't allow sharing because we need to know
       * when the memory becomes unused and can only then put
       * it back to the pool. Which is done in the pool's release
       * function
       */
      GST_MINI_OBJECT_FLAG_SET (mem, GST_MEMORY_FLAG_NO_SHARE);
    }

    if (pool->add_videometa) {
      GstVideoMeta *meta;

      meta = gst_buffer_get_video_meta (buf);
      if (!meta) {
        gst_buffer_add_video_meta (buf, GST_VIDEO_FRAME_FLAG_NONE,
            GST_VIDEO_INFO_FORMAT (&pool->video_info),
            GST_VIDEO_INFO_WIDTH (&pool->video_info),
            GST_VIDEO_INFO_HEIGHT (&pool->video_info));
      }
    }

    pool->need_copy = FALSE;
  } else {
    GstMemory *mem;
    const guint nstride = pool->port->port_def.format.video.nStride;
    const guint nslice = pool->port->port_def.format.video.nSliceHeight;
    gsize offset[GST_VIDEO_MAX_PLANES] = { 0, };
    gint stride[GST_VIDEO_MAX_PLANES] = { 0, };
    gint slice[GST_VIDEO_MAX_PLANES] = { 0, };

    stride[0] = nstride;
    slice[0] = nslice;

    switch (GST_VIDEO_INFO_FORMAT (&pool->video_info)) {
      case GST_VIDEO_FORMAT_ABGR:
      case GST_VIDEO_FORMAT_ARGB:
      case GST_VIDEO_FORMAT_RGB16:
      case GST_VIDEO_FORMAT_BGR16:
      case GST_VIDEO_FORMAT_YUY2:
      case GST_VIDEO_FORMAT_UYVY:
      case GST_VIDEO_FORMAT_YVYU:
      case GST_VIDEO_FORMAT_GRAY8:
        break;
      case GST_VIDEO_FORMAT_I420:
        stride[1] = nstride / 2;
        slice[1] = nslice / 2;
        offset[1] = offset[0] + stride[0] * nslice;
        stride[2] = nstride / 2;
        slice[2] = slice[1];
        offset[2] = offset[1] + (stride[1] * nslice / 2);
        break;
      case GST_VIDEO_FORMAT_NV12:
      case GST_VIDEO_FORMAT_NV16:
        stride[1] = nstride;
        slice[1] = nslice / 2;
        offset[1] = offset[0] + stride[0] * nslice;
        break;
      default:
        g_assert_not_reached ();
        break;
    }

    if (GST_IS_OMX_VIDEO_DEC (pool->element) &&
        GST_OMX_VIDEO_DEC (pool->element)->use_dmabuf == TRUE &&
        (omx_buf->omx_buf->pOutputPortPrivate)) {
#if defined (HAVE_MMNGRBUF) && defined (HAVE_VIDEODEC_EXT)
      if (pool->allocator && GST_IS_OMX_MEMORY_ALLOCATOR (pool->allocator)) {
        gst_object_unref (pool->allocator);
        pool->allocator = gst_dmabuf_allocator_new ();
      }
      GST_DEBUG_OBJECT (pool, "DMABUF - Using %s allocator",
          pool->allocator->mem_type);

      buf = gst_omx_buffer_pool_create_buffer_contain_dmabuf (pool,
          omx_buf, (gint *) (&stride), (gint *) (&slice), (gsize *) (&offset));
      if (!buf) {
        GST_ERROR_OBJECT (pool, "Can not create buffer contain dmabuf");
        return GST_FLOW_ERROR;
      }
#else
      GST_ELEMENT_ERROR (pool->element, STREAM, FAILED, (NULL),
          ("dmabuf mode is invalid now due to not have MMNGR_BUF or MC does not support getting physical address"));
      return GST_FLOW_ERROR;
#endif
    } else {
      if (pool->allocator && !GST_IS_OMX_MEMORY_ALLOCATOR (pool->allocator)) {
        gst_object_unref (pool->allocator);
        pool->allocator =
            g_object_new (gst_omx_memory_allocator_get_type (), NULL);
      }
      GST_DEBUG_OBJECT (pool, "Using %s allocator", pool->allocator->mem_type);

      if (GST_IS_OMX_VIDEO_ENC (pool->element) &&
          pool->port->port_def.eDir == OMX_DirInput)
        /* Propose actual area of encoder to upstream */
        mem = gst_memory_new_wrapped (0, omx_buf->omx_buf->pBuffer,
            omx_buf->omx_buf->nAllocLen, 0, 0, NULL, NULL);
      else
        mem = gst_omx_memory_allocator_alloc (pool->allocator, 0, omx_buf);

      buf = gst_buffer_new ();
      gst_buffer_append_memory (buf, mem);
      g_ptr_array_add (pool->buffers, buf);
      if (pool->add_videometa) {
        pool->need_copy = FALSE;
      } else {
        GstVideoInfo info;
        gboolean need_copy = FALSE;
        gint i;

        gst_video_info_init (&info);
        gst_video_info_set_format (&info,
            GST_VIDEO_INFO_FORMAT (&pool->video_info),
            GST_VIDEO_INFO_WIDTH (&pool->video_info),
            GST_VIDEO_INFO_HEIGHT (&pool->video_info));

        for (i = 0; i < GST_VIDEO_INFO_N_PLANES (&pool->video_info); i++) {
          if (info.stride[i] != stride[i] || info.offset[i] != offset[i]) {
            need_copy = TRUE;
            break;
          }
        }

        pool->need_copy = need_copy;
      }

      if (pool->need_copy || pool->add_videometa) {
        /* We always add the videometa. It's the job of the user
         * to copy the buffer if pool->need_copy is TRUE
         */
        gst_buffer_add_video_meta_full (buf, GST_VIDEO_FRAME_FLAG_NONE,
            GST_VIDEO_INFO_FORMAT (&pool->video_info),
            GST_VIDEO_INFO_WIDTH (&pool->video_info),
            GST_VIDEO_INFO_HEIGHT (&pool->video_info),
            GST_VIDEO_INFO_N_PLANES (&pool->video_info), offset, stride);
      }
    }
  }

  gst_mini_object_set_qdata (GST_MINI_OBJECT_CAST (buf),
      gst_omx_buffer_data_quark, omx_buf, NULL);

  *buffer = buf;

  pool->current_buffer_index++;

  return GST_FLOW_OK;
}

static void
gst_omx_buffer_pool_free_buffer (GstBufferPool * bpool, GstBuffer * buffer)
{
  GstOMXBufferPool *pool = GST_OMX_BUFFER_POOL (bpool);

  /* If the buffers belong to another pool, restore them now */
  GST_OBJECT_LOCK (pool);
  if (pool->other_pool) {
    gst_object_replace ((GstObject **) & buffer->pool,
        (GstObject *) pool->other_pool);
  }
  GST_OBJECT_UNLOCK (pool);

  gst_mini_object_set_qdata (GST_MINI_OBJECT_CAST (buffer),
      gst_omx_buffer_data_quark, NULL, NULL);

  GST_BUFFER_POOL_CLASS (gst_omx_buffer_pool_parent_class)->free_buffer (bpool,
      buffer);
}

static GstFlowReturn
gst_omx_buffer_pool_acquire_buffer (GstBufferPool * bpool,
    GstBuffer ** buffer, GstBufferPoolAcquireParams * params)
{
  GstFlowReturn ret;
  GstOMXBufferPool *pool = GST_OMX_BUFFER_POOL (bpool);

  if (pool->port->port_def.eDir == OMX_DirOutput) {
    GstBuffer *buf;

    g_return_val_if_fail (pool->current_buffer_index != -1, GST_FLOW_ERROR);

    buf = g_ptr_array_index (pool->buffers, pool->current_buffer_index);
    g_return_val_if_fail (buf != NULL, GST_FLOW_ERROR);
    *buffer = buf;
    ret = GST_FLOW_OK;

    /* If it's our own memory we have to set the sizes */
    if ((!pool->other_pool) &&
        ((GST_OMX_VIDEO_DEC (pool->element)->use_dmabuf) == FALSE)) {
      GstMemory *mem = gst_buffer_peek_memory (*buffer, 0);

      g_assert (mem
          && g_strcmp0 (mem->allocator->mem_type, GST_OMX_MEMORY_TYPE) == 0);
      mem->size = ((GstOMXMemory *) mem)->buf->omx_buf->nFilledLen;
      mem->offset = ((GstOMXMemory *) mem)->buf->omx_buf->nOffset;
    }
  } else {
    if (GST_IS_OMX_VIDEO_ENC (pool->element)) {
      GstBuffer *buf;
      GstOMXBuffer *omx_buf;
      gint count = 0;

      /* Search on number of OMXBuffer of port to find available GstBuffer
       * (emptied OMXBuffer) to propose to upstream. If after 3 times searching,
       * can not find target GstBuffer, return flow error
       */
      do {
        buf = g_ptr_array_index (pool->buffers, pool->enc_buffer_index);
        g_return_val_if_fail (buf != NULL, GST_FLOW_ERROR);

        omx_buf =
            gst_mini_object_get_qdata (GST_MINI_OBJECT_CAST (buf),
            gst_omx_buffer_data_quark);
        pool->enc_buffer_index++;
        if (pool->enc_buffer_index == pool->port->port_def.nBufferCountActual)
          pool->enc_buffer_index = 0;

        count += 1;
      } while (omx_buf->used == TRUE &&
          count < pool->port->port_def.nBufferCountActual * 3);

      if (count == pool->port->port_def.nBufferCountActual * 3) {
        ret = GST_FLOW_ERROR;
        GST_ERROR_OBJECT (pool,
            "Can not acquire buffer after 3 times searching");
      } else {
        *buffer = buf;
        ret = GST_FLOW_OK;
      }
    } else {
      /* Acquire any buffer that is available to be filled by upstream */
      ret =
          GST_BUFFER_POOL_CLASS
          (gst_omx_buffer_pool_parent_class)->acquire_buffer (bpool, buffer,
          params);
    }
  }

  return ret;
}

static void
gst_omx_buffer_pool_release_buffer (GstBufferPool * bpool, GstBuffer * buffer)
{
  GstOMXBufferPool *pool = GST_OMX_BUFFER_POOL (bpool);
  OMX_ERRORTYPE err;
  GstOMXBuffer *omx_buf;

  g_assert (pool->component && pool->port);

  if (!pool->allocating && !pool->deactivated) {
    omx_buf =
        gst_mini_object_get_qdata (GST_MINI_OBJECT_CAST (buffer),
        gst_omx_buffer_data_quark);
    if (pool->port->port_def.eDir == OMX_DirOutput && !omx_buf->used) {
      /* Release back to the port, can be filled again */
      err = gst_omx_port_release_buffer (pool->port, omx_buf);
      if (err != OMX_ErrorNone) {
        GST_ELEMENT_ERROR (pool->element, LIBRARY, SETTINGS, (NULL),
            ("Failed to relase output buffer to component: %s (0x%08x)",
                gst_omx_error_to_string (err), err));
      }
    } else if (!omx_buf->used) {
      /* TODO: Implement.
       *
       * If not used (i.e. was not passed to the component) this should do
       * the same as EmptyBufferDone.
       * If it is used (i.e. was passed to the component) this should do
       * nothing until EmptyBufferDone.
       *
       * EmptyBufferDone should release the buffer to the pool so it can
       * be allocated again
       *
       * Needs something to call back here in EmptyBufferDone, like keeping
       * a ref on the buffer in GstOMXBuffer until EmptyBufferDone... which
       * would ensure that the buffer is always unused when this is called.
       */
      if (GST_OMX_VIDEO_ENC (pool->element)->no_copy == FALSE) {
        g_assert_not_reached ();
        GST_BUFFER_POOL_CLASS (gst_omx_buffer_pool_parent_class)->release_buffer
            (bpool, buffer);
      }
    }
  }
}

static void
gst_omx_buffer_pool_finalize (GObject * object)
{
  GstOMXBufferPool *pool = GST_OMX_BUFFER_POOL (object);

#ifdef HAVE_MMNGRBUF
  if (GST_OMX_VIDEO_DEC (pool->element)->use_dmabuf) {
    gint i;
    gint dmabuf_id;

    for (i = 0; i < pool->id_array->len; i++) {
      dmabuf_id = g_array_index (pool->id_array, gint, i);
      if (dmabuf_id >= 0) {
        GST_DEBUG_OBJECT (pool, "mmngr_export_end_in_user (%d)", dmabuf_id);
        mmngr_export_end_in_user_ext (dmabuf_id);
      } else {
        GST_WARNING_OBJECT (pool, "Invalid dmabuf_id");
      }
    }
  }
  g_array_free (pool->id_array, TRUE);
#endif

  if (pool->element)
    gst_object_unref (pool->element);
  pool->element = NULL;

  if (pool->buffers)
    g_ptr_array_unref (pool->buffers);
  pool->buffers = NULL;

  if (pool->other_pool)
    gst_object_unref (pool->other_pool);
  pool->other_pool = NULL;

  if (pool->allocator)
    gst_object_unref (pool->allocator);
  pool->allocator = NULL;

  if (pool->caps)
    gst_caps_unref (pool->caps);
  pool->caps = NULL;

  G_OBJECT_CLASS (gst_omx_buffer_pool_parent_class)->finalize (object);
}

static void
gst_omx_buffer_pool_class_init (GstOMXBufferPoolClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstBufferPoolClass *gstbufferpool_class = (GstBufferPoolClass *) klass;

  gst_omx_buffer_data_quark = g_quark_from_static_string ("GstOMXBufferData");

  gobject_class->finalize = gst_omx_buffer_pool_finalize;
  gstbufferpool_class->start = gst_omx_buffer_pool_start;
  gstbufferpool_class->stop = gst_omx_buffer_pool_stop;
  gstbufferpool_class->get_options = gst_omx_buffer_pool_get_options;
  gstbufferpool_class->set_config = gst_omx_buffer_pool_set_config;
  gstbufferpool_class->alloc_buffer = gst_omx_buffer_pool_alloc_buffer;
  gstbufferpool_class->free_buffer = gst_omx_buffer_pool_free_buffer;
  gstbufferpool_class->acquire_buffer = gst_omx_buffer_pool_acquire_buffer;
  gstbufferpool_class->release_buffer = gst_omx_buffer_pool_release_buffer;
}

static void
gst_omx_buffer_pool_init (GstOMXBufferPool * pool)
{
  pool->buffers = g_ptr_array_new ();
  pool->allocator = g_object_new (gst_omx_memory_allocator_get_type (), NULL);
#ifdef HAVE_MMNGRBUF
  pool->id_array = g_array_new (FALSE, FALSE, sizeof (gint));
#endif
  pool->enc_buffer_index = 0;
}

GstBufferPool *
gst_omx_buffer_pool_new (GstElement * element, GstOMXComponent * component,
    GstOMXPort * port)
{
  GstOMXBufferPool *pool;

  pool = g_object_new (gst_omx_buffer_pool_get_type (), NULL);
  pool->element = gst_object_ref (element);
  pool->component = component;
  pool->port = port;

  return GST_BUFFER_POOL (pool);
}

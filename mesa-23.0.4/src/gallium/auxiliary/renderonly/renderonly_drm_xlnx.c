/*
 * Copyright (C) 2024 Ichiro Kawazome <ichiro_k@ca2.so-net.ne.jp>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *    Ichiro Kawazome <ichiro_k@ca2.so-net.ne.jp>
 */

#include "renderonly/renderonly.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <xf86drm.h>

#include "frontend/drm_driver.h"
#include "pipe/p_screen.h"
#include "util/format/u_format.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"

/**
 * Parameter identifier of various information
 */
enum drm_xlnx_param {
   DRM_XLNX_PARAM_DRIVER_IDENTIFIER       = 0,
   DRM_XLNX_PARAM_SCANOUT_ALIGNMENT_SIZE  = 1,
   DRM_XLNX_PARAM_DUMB_ALIGNMENT_SIZE     = 2,
   DRM_XLNX_PARAM_DUMB_CACHE_AVALABLE     = 3,
   DRM_XLNX_PARAM_DUMB_CACHE_DEFAULT_MODE = 4,
   DRM_XLNX_PARAM_DUMB_ALIGNMENT_MODE     = 5,
};

/**
 * Get various information of the Xilinx DRM KMS Driver
 */
struct drm_xlnx_get_param {
   __u32 param; /* in , value in enum drm_xlnx_param */
   __u32 pad;   /* pad, must be zero */
   __u64 value; /* out, parameter value */
};

/**
 * Set various information of the Xilinx DRM KMS Driver
 */
struct drm_xlnx_set_param {
   __u32 param; /* in , value in enum drm_xlnx_param */
   __u32 pad;   /* pad, must be zero */
   __u64 value; /* in , parameter value */
};

/**
 * Xilinx DRM KMS Driver specific ioctls.
 */
#define DRM_XLNX_GET_PARAM   0x00
#define DRM_XLNX_SET_PARAM   0x01

#define DRM_IOCTL_XLNX_GET_PARAM DRM_IOWR(DRM_COMMAND_BASE+DRM_XLNX_GET_PARAM, struct drm_xlnx_get_param)
#define DRM_IOCTL_XLNX_SET_PARAM DRM_IOWR(DRM_COMMAND_BASE+DRM_XLNX_SET_PARAM, struct drm_xlnx_set_param)

/**
 * Xilinx DRM KMS Driver Identifier
 */
#define DRM_XLNX_DRIVER_IDENTIFIER      (0x53620C75)

/**
 * Xilinx DRM KMS Driver Create Dumb Flags
 */
#define DRM_XLNX_GEM_DUMB_SCANOUT_BIT       (0)
#define DRM_XLNX_GEM_DUMB_SCANOUT_MASK      (1 << DRM_XLNX_GEM_DUMB_SCANOUT_BIT)
#define DRM_XLNX_GEM_DUMB_SCANOUT           (1 << DRM_XLNX_GEM_DUMB_SCANOUT_BIT)
#define DRM_XLNX_GEM_DUMB_NON_SCANOUT       (0 << DRM_XLNX_GEM_DUMB_SCANOUT_BIT)

#define DRM_XLNX_GEM_DUMB_CACHE_BIT         (1)
#define DRM_XLNX_GEM_DUMB_CACHE_MASK        (3 << DRM_XLNX_GEM_DUMB_CACHE_BIT)
#define DRM_XLNX_GEM_DUMB_CACHE_DEFAULT     (0 << DRM_XLNX_GEM_DUMB_CACHE_BIT)
#define DRM_XLNX_GEM_DUMB_CACHE_OFF         (2 << DRM_XLNX_GEM_DUMB_CACHE_BIT)
#define DRM_XLNX_GEM_DUMB_CACHE_ON          (3 << DRM_XLNX_GEM_DUMB_CACHE_BIT)

#define DRM_XLNX_GEM_DUMB_ALIGN_MODE_BIT    (3)
#define DRM_XLNX_GEM_DUMB_ALIGN_MODE_MASK   (1 << DRM_XLNX_GEM_DUMB_ALIGN_MODE_BIT)
#define DRM_XLNX_GEM_DUMB_ALIGN_NOCHECK     (1 << DRM_XLNX_GEM_DUMB_ALIGN_MODE_BIT)
#define DRM_XLNX_GEM_DUMB_ALIGN_DEFAULT     (0 << DRM_XLNX_GEM_DUMB_ALIGN_MODE_BIT)

static struct renderonly_scanout *
drm_xlnx_create_dumb_buffer_for_resource(struct pipe_resource *rsc,
                    struct renderonly *ro,
                    struct winsys_handle *out_handle,
                    bool   mmap_cache)
{
   struct renderonly_scanout *scanout;
   int err;
   struct drm_mode_create_dumb create_dumb = {
      .width  = rsc->width0,
      .height = rsc->height0,
      .bpp    = util_format_get_blocksizebits(rsc->format),
   };

   create_dumb.pitch = create_dumb.width * DIV_ROUND_UP(create_dumb.bpp, 8);
   create_dumb.flags = DRM_XLNX_GEM_DUMB_ALIGN_NOCHECK;

   struct drm_mode_destroy_dumb destroy_dumb = {0};

   if (mmap_cache)
      create_dumb.flags |= DRM_XLNX_GEM_DUMB_CACHE_ON;

   /* create dumb buffer at scanout GPU */
   err = drmIoctl(ro->kms_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_dumb);
   if (err < 0) {
      fprintf(stderr, "DRM_IOCTL_MODE_CREATE_DUMB failed: %s\n",
            strerror(errno));
      return NULL;
   }

   simple_mtx_lock(&ro->bo_map_lock);
   scanout = util_sparse_array_get(&ro->bo_map, create_dumb.handle);
   simple_mtx_unlock(&ro->bo_map_lock);

   if (!scanout)
      goto free_dumb;

   scanout->handle = create_dumb.handle;
   scanout->stride = create_dumb.pitch;

   assert(p_atomic_read(&scanout->refcnt) == 0);
   p_atomic_set(&scanout->refcnt, 1);

   if (!out_handle)
      return scanout;

   /* fill in winsys handle */
   memset(out_handle, 0, sizeof(*out_handle));
   out_handle->type = WINSYS_HANDLE_TYPE_FD;
   out_handle->stride = create_dumb.pitch;

   err = drmPrimeHandleToFD(ro->kms_fd, create_dumb.handle, O_CLOEXEC,
         (int *)&out_handle->handle);
   if (err < 0) {
      fprintf(stderr, "failed to export dumb buffer: %s\n", strerror(errno));
      goto free_dumb;
   }

   return scanout;

free_dumb:
   destroy_dumb.handle = scanout->handle;
   drmIoctl(ro->kms_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_dumb);

   return NULL;
}

static struct renderonly_scanout *
drm_xlnx_create_dumb_buffer_for_resource_mmap_cache(
   struct pipe_resource *rsc,
   struct renderonly *ro,
   struct winsys_handle *out_handle)
{
   return drm_xlnx_create_dumb_buffer_for_resource(rsc, ro, out_handle, true);
}

static struct renderonly_scanout *
drm_xlnx_create_dumb_buffer_for_resource_mmap_nocache(
   struct pipe_resource *rsc,
   struct renderonly *ro,
   struct winsys_handle *out_handle)
{
   return drm_xlnx_create_dumb_buffer_for_resource(rsc, ro, out_handle, false);
}

/**
 * Probe drm xlnx for renderonly initialize.
 */
bool renderonly_probe_drm_xlnx(struct renderonly *ro)
{
   int           ret;
   bool          mmap_cache_avalable        = false;
   int           alignment_default_size     = 0;
   bool          alignment_nocheck_avalable = false;
   const char    drm_name[ ] = {'x','l','n','x','\0'};
   char          name_buf[sizeof(drm_name)];
   struct drm_version        version;
   struct drm_xlnx_get_param get_param_arg;
   struct drm_xlnx_set_param set_param_arg;

   if (!ro)
      return false;

   if (ro->kms_fd < 0)
      return false;

   memset(&version , 0, sizeof(version ));
   memset(&name_buf, 0, sizeof(name_buf));
   version.name     = name_buf;
   version.name_len = sizeof(name_buf);
   ret = drmIoctl(ro->kms_fd, DRM_IOCTL_VERSION, &version);
   if (ret) {
      fprintf(stderr, "DRM_IOCTL_VERSION failed: %s\n", strerror(errno));
      return false;
   }
   if (strncmp(version.name, drm_name, sizeof(drm_name)) != 0) {
      return false;
   }

   memset(&get_param_arg, 0, sizeof(get_param_arg));
   get_param_arg.param = DRM_XLNX_PARAM_DRIVER_IDENTIFIER;
   ret = drmIoctl(ro->kms_fd, DRM_IOCTL_XLNX_GET_PARAM, &get_param_arg);
   if (ret) {
      return false;
   }
   if (get_param_arg.value != DRM_XLNX_DRIVER_IDENTIFIER) {
      return false;
   }

   memset(&get_param_arg, 0, sizeof(get_param_arg));
   get_param_arg.param = DRM_XLNX_PARAM_DUMB_ALIGNMENT_SIZE;
   ret = drmIoctl(ro->kms_fd, DRM_IOCTL_XLNX_GET_PARAM, &get_param_arg);
   if (ret) {
      return false;
   }
   alignment_default_size = (int)get_param_arg.value;

   memset(&get_param_arg, 0, sizeof(get_param_arg));
   get_param_arg.param = DRM_XLNX_PARAM_DUMB_ALIGNMENT_MODE;
   ret = drmIoctl(ro->kms_fd, DRM_IOCTL_XLNX_GET_PARAM, &get_param_arg);
   if (ret) {
      alignment_nocheck_avalable = false;
   } else {
      alignment_nocheck_avalable = (get_param_arg.value != 0) ? true : false;
   }
   if (alignment_nocheck_avalable == false) {
     if ((alignment_default_size ==  1) ||
         (alignment_default_size ==  2) ||
         (alignment_default_size ==  4) ||
         (alignment_default_size ==  8) ||
         (alignment_default_size == 16))
       alignment_nocheck_avalable = true;
   }
   if (alignment_nocheck_avalable == false) {
     return false;
   }

   memset(&get_param_arg, 0, sizeof(get_param_arg));
   get_param_arg.param = DRM_XLNX_PARAM_DUMB_CACHE_AVALABLE;
   ret = drmIoctl(ro->kms_fd, DRM_IOCTL_XLNX_GET_PARAM, &get_param_arg);
   if (ret) {
      mmap_cache_avalable = false;
   } else {
      mmap_cache_avalable = (get_param_arg.value != 0) ? true : false;
   }
   if (mmap_cache_avalable == true)
      ro->create_for_resource = drm_xlnx_create_dumb_buffer_for_resource_mmap_cache;
   else 
      ro->create_for_resource = drm_xlnx_create_dumb_buffer_for_resource_mmap_nocache;

   return true;

}

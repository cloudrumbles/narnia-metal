/*
 * Copyright 2026 Shahriyyah
 * SPDX-License-Identifier: MIT
 *
 * narnia-metal remote WSI: a virtual swapchain for the net backend.
 *
 * Surfaces are still created by Mesa's common WSI (they are just structs
 * holding the X11 connection and window). Everything after that is here and
 * never touches the local display: swapchain images are ordinary device-local
 * images, a present sends NM_PRESENT so the host copies the image out on its
 * own GPU, and an image can be acquired again once the host says it is done
 * with it (NM_PRESENT_DONE). Frames never come back over the network.
 *
 * These entry points use the vn_net_ prefix and replace the WSI entries of
 * the physical device and device dispatch tables when
 * renderer->info.remote_wsi is set.
 */

#include "vn_common.h"

#include <time.h>

#include "vk_util.h"
#include "vulkan/vk_icd.h"

#ifdef VK_USE_PLATFORM_XCB_KHR
#include <xcb/xcb.h>
#endif
#ifdef VK_USE_PLATFORM_XLIB_KHR
#include <X11/Xlib-xcb.h>
#endif

#include "vn_device.h"
#include "vn_image.h"
#include "vn_instance.h"
#include "vn_physical_device.h"
#include "vn_queue.h"
#include "vn_renderer_net.h"
#include "vn_ring.h"

#define VN_NET_MIN_IMAGES 2
#define VN_NET_MAX_IMAGES 8

struct vn_net_swapchain_image {
   VkImage image;
   VkDeviceMemory memory;
   vn_object_id image_id;

   bool acquired; /* owned by the app */
   bool busy;     /* presented, host not done yet */
   uint64_t present_seq;
};

struct vn_net_swapchain {
   struct vk_object_base base;

   struct vn_device *dev;
   uint32_t id;

   VkExtent2D extent;
   VkFormat format;
   VkSharingMode sharing;

   mtx_t mutex;
   cnd_t cond;
   uint64_t present_seq;

   uint32_t image_count;
   struct vn_net_swapchain_image images[];
};

VK_DEFINE_NONDISP_HANDLE_CASTS(vn_net_swapchain,
                               base,
                               VkSwapchainKHR,
                               VK_OBJECT_TYPE_SWAPCHAIN_KHR)

static const VkFormat vn_net_formats[] = {
   VK_FORMAT_B8G8R8A8_SRGB,
   VK_FORMAT_B8G8R8A8_UNORM,
   VK_FORMAT_R8G8B8A8_SRGB,
   VK_FORMAT_R8G8B8A8_UNORM,
};

static const VkPresentModeKHR vn_net_present_modes[] = {
   VK_PRESENT_MODE_FIFO_KHR,
   VK_PRESENT_MODE_MAILBOX_KHR,
   VK_PRESENT_MODE_IMMEDIATE_KHR,
};

/* ------------------------------------------------------------------ */
/* surfaces                                                           */

/* the size of the window behind the surface, if we can tell */
static bool
vn_net_surface_extent(VkSurfaceKHR _surface, VkExtent2D *extent)
{
   VkIcdSurfaceBase *surface = (VkIcdSurfaceBase *)(uintptr_t)_surface;
   if (!surface)
      return false;

#if defined(VK_USE_PLATFORM_XCB_KHR)
   xcb_connection_t *conn = NULL;
   xcb_window_t window = 0;
   switch (surface->platform) {
   case VK_ICD_WSI_PLATFORM_XCB: {
      VkIcdSurfaceXcb *xcb = (VkIcdSurfaceXcb *)surface;
      conn = xcb->connection;
      window = xcb->window;
      break;
   }
#if defined(VK_USE_PLATFORM_XLIB_KHR)
   case VK_ICD_WSI_PLATFORM_XLIB: {
      VkIcdSurfaceXlib *xlib = (VkIcdSurfaceXlib *)surface;
      conn = XGetXCBConnection(xlib->dpy);
      window = xlib->window;
      break;
   }
#endif
   default:
      return false;
   }

   xcb_get_geometry_reply_t *reply =
      xcb_get_geometry_reply(conn, xcb_get_geometry(conn, window), NULL);
   if (!reply)
      return false;
   *extent = (VkExtent2D){ reply->width, reply->height };
   free(reply);
   return extent->width && extent->height;
#else
   return false;
#endif
}

static void
vn_net_get_caps(VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR *caps)
{
   VkExtent2D extent;
   if (!vn_net_surface_extent(surface, &extent))
      extent = (VkExtent2D){ UINT32_MAX, UINT32_MAX };

   *caps = (VkSurfaceCapabilitiesKHR){
      .minImageCount = VN_NET_MIN_IMAGES,
      .maxImageCount = VN_NET_MAX_IMAGES,
      .currentExtent = extent,
      .minImageExtent = { 1, 1 },
      .maxImageExtent = { 16384, 16384 },
      .maxImageArrayLayers = 1,
      .supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      .currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      .supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR |
                                 VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
      .supportedUsageFlags =
         VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
         VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT,
   };
}

VKAPI_ATTR VkResult VKAPI_CALL
vn_net_GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physicalDevice,
                                          uint32_t queueFamilyIndex,
                                          VkSurfaceKHR surface,
                                          VkBool32 *pSupported)
{
   *pSupported = VK_TRUE;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
vn_net_GetPhysicalDeviceSurfaceCapabilitiesKHR(
   VkPhysicalDevice physicalDevice,
   VkSurfaceKHR surface,
   VkSurfaceCapabilitiesKHR *pSurfaceCapabilities)
{
   vn_net_get_caps(surface, pSurfaceCapabilities);
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
vn_net_GetPhysicalDeviceSurfaceCapabilities2KHR(
   VkPhysicalDevice physicalDevice,
   const VkPhysicalDeviceSurfaceInfo2KHR *pSurfaceInfo,
   VkSurfaceCapabilities2KHR *pSurfaceCapabilities)
{
   vn_net_get_caps(pSurfaceInfo->surface,
                   &pSurfaceCapabilities->surfaceCapabilities);

   const VkSurfacePresentModeKHR *mode =
      vk_find_struct_const(pSurfaceInfo->pNext, SURFACE_PRESENT_MODE_KHR);

   vk_foreach_struct (ext, pSurfaceCapabilities->pNext) {
      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_SURFACE_PROTECTED_CAPABILITIES_KHR: {
         VkSurfaceProtectedCapabilitiesKHR *prot = (void *)ext;
         prot->supportsProtected = VK_FALSE;
         break;
      }
      case VK_STRUCTURE_TYPE_SHARED_PRESENT_SURFACE_CAPABILITIES_KHR: {
         VkSharedPresentSurfaceCapabilitiesKHR *shared = (void *)ext;
         shared->sharedPresentSupportedUsageFlags = 0;
         break;
      }
      case VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_COMPATIBILITY_KHR: {
         VkSurfacePresentModeCompatibilityKHR *compat = (void *)ext;
         if (compat->pPresentModes) {
            if (compat->presentModeCount && mode) {
               compat->pPresentModes[0] = mode->presentMode;
               compat->presentModeCount = 1;
            } else {
               compat->presentModeCount = 0;
            }
         } else {
            compat->presentModeCount = mode ? 1 : 0;
         }
         break;
      }
      case VK_STRUCTURE_TYPE_SURFACE_PRESENT_SCALING_CAPABILITIES_KHR: {
         VkSurfacePresentScalingCapabilitiesKHR *scaling = (void *)ext;
         scaling->supportedPresentScaling = 0;
         scaling->supportedPresentGravityX = 0;
         scaling->supportedPresentGravityY = 0;
         scaling->minScaledImageExtent =
            pSurfaceCapabilities->surfaceCapabilities.minImageExtent;
         scaling->maxScaledImageExtent =
            pSurfaceCapabilities->surfaceCapabilities.maxImageExtent;
         break;
      }
      default:
         break;
      }
   }

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
vn_net_GetPhysicalDeviceSurfaceFormatsKHR(
   VkPhysicalDevice physicalDevice,
   VkSurfaceKHR surface,
   uint32_t *pSurfaceFormatCount,
   VkSurfaceFormatKHR *pSurfaceFormats)
{
   VK_OUTARRAY_MAKE_TYPED(VkSurfaceFormatKHR, out, pSurfaceFormats,
                          pSurfaceFormatCount);
   for (uint32_t i = 0; i < ARRAY_SIZE(vn_net_formats); i++) {
      vk_outarray_append_typed(VkSurfaceFormatKHR, &out, f)
      {
         f->format = vn_net_formats[i];
         f->colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      }
   }
   return vk_outarray_status(&out);
}

VKAPI_ATTR VkResult VKAPI_CALL
vn_net_GetPhysicalDeviceSurfaceFormats2KHR(
   VkPhysicalDevice physicalDevice,
   const VkPhysicalDeviceSurfaceInfo2KHR *pSurfaceInfo,
   uint32_t *pSurfaceFormatCount,
   VkSurfaceFormat2KHR *pSurfaceFormats)
{
   VK_OUTARRAY_MAKE_TYPED(VkSurfaceFormat2KHR, out, pSurfaceFormats,
                          pSurfaceFormatCount);
   for (uint32_t i = 0; i < ARRAY_SIZE(vn_net_formats); i++) {
      vk_outarray_append_typed(VkSurfaceFormat2KHR, &out, f)
      {
         f->surfaceFormat.format = vn_net_formats[i];
         f->surfaceFormat.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      }
   }
   return vk_outarray_status(&out);
}

VKAPI_ATTR VkResult VKAPI_CALL
vn_net_GetPhysicalDeviceSurfacePresentModesKHR(
   VkPhysicalDevice physicalDevice,
   VkSurfaceKHR surface,
   uint32_t *pPresentModeCount,
   VkPresentModeKHR *pPresentModes)
{
   VK_OUTARRAY_MAKE_TYPED(VkPresentModeKHR, out, pPresentModes,
                          pPresentModeCount);
   for (uint32_t i = 0; i < ARRAY_SIZE(vn_net_present_modes); i++) {
      vk_outarray_append_typed(VkPresentModeKHR, &out, m)
         *m = vn_net_present_modes[i];
   }
   return vk_outarray_status(&out);
}

VKAPI_ATTR VkResult VKAPI_CALL
vn_net_GetPhysicalDevicePresentRectanglesKHR(VkPhysicalDevice physicalDevice,
                                             VkSurfaceKHR surface,
                                             uint32_t *pRectCount,
                                             VkRect2D *pRects)
{
   VK_OUTARRAY_MAKE_TYPED(VkRect2D, out, pRects, pRectCount);
   VkExtent2D extent;
   if (vn_net_surface_extent(surface, &extent)) {
      vk_outarray_append_typed(VkRect2D, &out, r)
         *r = (VkRect2D){ .extent = extent };
   }
   return vk_outarray_status(&out);
}

VKAPI_ATTR VkResult VKAPI_CALL
vn_net_GetDeviceGroupPresentCapabilitiesKHR(
   VkDevice device,
   VkDeviceGroupPresentCapabilitiesKHR *pCapabilities)
{
   memset(pCapabilities->presentMask, 0, sizeof(pCapabilities->presentMask));
   pCapabilities->presentMask[0] = 0x1;
   pCapabilities->modes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
vn_net_GetDeviceGroupSurfacePresentModesKHR(
   VkDevice device,
   VkSurfaceKHR surface,
   VkDeviceGroupPresentModeFlagsKHR *pModes)
{
   *pModes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
   return VK_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* swapchains                                                         */

static void
vn_net_swapchain_present_done(void *data, uint32_t image_index)
{
   struct vn_net_swapchain *chain = data;
   mtx_lock(&chain->mutex);
   if (image_index < chain->image_count)
      chain->images[image_index].busy = false;
   cnd_broadcast(&chain->cond);
   mtx_unlock(&chain->mutex);
}

static uint32_t
vn_net_pick_memory_type(const struct vn_physical_device *physical_dev,
                        uint32_t type_bits)
{
   const VkPhysicalDeviceMemoryProperties *props =
      &physical_dev->memory_properties;
   for (uint32_t i = 0; i < props->memoryTypeCount; i++) {
      if ((type_bits & (1u << i)) &&
          (props->memoryTypes[i].propertyFlags &
           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
         return i;
   }
   return ffs(type_bits) - 1;
}

static void
vn_net_swapchain_destroy_images(struct vn_device *dev,
                                struct vn_net_swapchain *chain,
                                const VkAllocationCallbacks *alloc)
{
   const struct vk_device_dispatch_table *vk = &dev->base.vk.dispatch_table;
   VkDevice device = vn_device_to_handle(dev);
   for (uint32_t i = 0; i < chain->image_count; i++) {
      if (chain->images[i].image)
         vk->DestroyImage(device, chain->images[i].image, NULL);
      if (chain->images[i].memory)
         vk->FreeMemory(device, chain->images[i].memory, NULL);
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
vn_net_CreateSwapchainKHR(VkDevice device,
                          const VkSwapchainCreateInfoKHR *pCreateInfo,
                          const VkAllocationCallbacks *pAllocator,
                          VkSwapchainKHR *pSwapchain)
{
   struct vn_device *dev = vn_device_from_handle(device);
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.vk.alloc;
   const struct vk_device_dispatch_table *vk = &dev->base.vk.dispatch_table;

   const uint32_t count = CLAMP(pCreateInfo->minImageCount, VN_NET_MIN_IMAGES,
                                VN_NET_MAX_IMAGES);

   struct vn_net_swapchain *chain = vk_zalloc(
      alloc, sizeof(*chain) + sizeof(chain->images[0]) * count,
      VN_DEFAULT_ALIGN, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!chain)
      return vn_error(dev->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   vk_object_base_init(&dev->base.vk, &chain->base,
                       VK_OBJECT_TYPE_SWAPCHAIN_KHR);
   chain->dev = dev;
   chain->extent = pCreateInfo->imageExtent;
   chain->format = pCreateInfo->imageFormat;
   chain->sharing = pCreateInfo->imageSharingMode;
   chain->image_count = count;
   mtx_init(&chain->mutex, mtx_plain);
   cnd_init(&chain->cond);

   VkResult result = VK_SUCCESS;
   for (uint32_t i = 0; i < count && result == VK_SUCCESS; i++) {
      struct vn_net_swapchain_image *img = &chain->images[i];

      const VkImageCreateInfo image_info = {
         .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
         .imageType = VK_IMAGE_TYPE_2D,
         .format = pCreateInfo->imageFormat,
         .extent = { chain->extent.width, chain->extent.height, 1 },
         .mipLevels = 1,
         .arrayLayers = pCreateInfo->imageArrayLayers,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .tiling = VK_IMAGE_TILING_OPTIMAL,
         /* the host copies out of it */
         .usage = pCreateInfo->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
         .sharingMode = pCreateInfo->imageSharingMode,
         .queueFamilyIndexCount = pCreateInfo->queueFamilyIndexCount,
         .pQueueFamilyIndices = pCreateInfo->pQueueFamilyIndices,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      };
      result = vk->CreateImage(device, &image_info, NULL, &img->image);
      if (result != VK_SUCCESS)
         break;

      VkMemoryRequirements reqs;
      vk->GetImageMemoryRequirements(device, img->image, &reqs);

      const VkMemoryDedicatedAllocateInfo dedicated = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
         .image = img->image,
      };
      const VkMemoryAllocateInfo mem_info = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
         .pNext = &dedicated,
         .allocationSize = reqs.size,
         .memoryTypeIndex = vn_net_pick_memory_type(dev->physical_device,
                                                    reqs.memoryTypeBits),
      };
      result = vk->AllocateMemory(device, &mem_info, NULL, &img->memory);
      if (result != VK_SUCCESS)
         break;

      result = vk->BindImageMemory(device, img->image, img->memory, 0);
      if (result != VK_SUCCESS)
         break;

      img->image_id = vn_image_from_handle(img->image)->base.id;
   }

   if (result == VK_SUCCESS) {
      chain->id = vn_renderer_net_register_swapchain(
         dev->renderer, vn_net_swapchain_present_done, chain);
      if (!chain->id)
         result = VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   if (result != VK_SUCCESS) {
      vn_net_swapchain_destroy_images(dev, chain, alloc);
      mtx_destroy(&chain->mutex);
      cnd_destroy(&chain->cond);
      vk_object_base_finish(&chain->base);
      vk_free(alloc, chain);
      return vn_error(dev->instance, result);
   }

   if (VN_DEBUG(WSI)) {
      vn_log(dev->instance, "net swapchain %u: %ux%u format %d, %u images",
             chain->id, chain->extent.width, chain->extent.height,
             chain->format, count);
   }

   *pSwapchain = vn_net_swapchain_to_handle(chain);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
vn_net_DestroySwapchainKHR(VkDevice device,
                           VkSwapchainKHR swapchain,
                           const VkAllocationCallbacks *pAllocator)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_net_swapchain *chain = vn_net_swapchain_from_handle(swapchain);
   if (!chain)
      return;
   const VkAllocationCallbacks *alloc =
      pAllocator ? pAllocator : &dev->base.vk.alloc;

   /* The host waits for its copies before it drops its state, and handles
    * this before any later ring commands, so the images can go right after.
    */
   vn_renderer_net_unregister_swapchain(dev->renderer, chain->id);
   vn_net_swapchain_destroy_images(dev, chain, alloc);

   mtx_destroy(&chain->mutex);
   cnd_destroy(&chain->cond);
   vk_object_base_finish(&chain->base);
   vk_free(alloc, chain);
}

VKAPI_ATTR VkResult VKAPI_CALL
vn_net_GetSwapchainImagesKHR(VkDevice device,
                             VkSwapchainKHR swapchain,
                             uint32_t *pSwapchainImageCount,
                             VkImage *pSwapchainImages)
{
   struct vn_net_swapchain *chain = vn_net_swapchain_from_handle(swapchain);
   VK_OUTARRAY_MAKE_TYPED(VkImage, out, pSwapchainImages,
                          pSwapchainImageCount);
   for (uint32_t i = 0; i < chain->image_count; i++) {
      vk_outarray_append_typed(VkImage, &out, img)
         *img = chain->images[i].image;
   }
   return vk_outarray_status(&out);
}

/* the free image presented longest ago, or -1 */
static int
vn_net_swapchain_find_free_locked(const struct vn_net_swapchain *chain)
{
   int best = -1;
   for (uint32_t i = 0; i < chain->image_count; i++) {
      const struct vn_net_swapchain_image *img = &chain->images[i];
      if (img->acquired || img->busy)
         continue;
      if (best < 0 || img->present_seq < chain->images[best].present_seq)
         best = i;
   }
   return best;
}

VKAPI_ATTR VkResult VKAPI_CALL
vn_net_AcquireNextImage2KHR(VkDevice device,
                            const VkAcquireNextImageInfoKHR *pAcquireInfo,
                            uint32_t *pImageIndex)
{
   struct vn_device *dev = vn_device_from_handle(device);
   struct vn_net_swapchain *chain =
      vn_net_swapchain_from_handle(pAcquireInfo->swapchain);
   const uint64_t timeout = pAcquireInfo->timeout;

   struct timespec deadline;
   timespec_get(&deadline, TIME_UTC);
   const uint64_t now_ns =
      (uint64_t)deadline.tv_sec * 1000000000ull + deadline.tv_nsec;
   const uint64_t end_ns =
      timeout >= UINT64_MAX - now_ns ? UINT64_MAX : now_ns + timeout;

   VkResult result = VK_SUCCESS;
   int index;
   mtx_lock(&chain->mutex);
   while ((index = vn_net_swapchain_find_free_locked(chain)) < 0) {
      if (vn_renderer_net_is_lost(dev->renderer)) {
         result = VK_ERROR_DEVICE_LOST;
         break;
      }
      if (!timeout) {
         result = VK_NOT_READY;
         break;
      }

      /* wake up at least every 100 ms to notice a lost connection */
      timespec_get(&deadline, TIME_UTC);
      uint64_t t = (uint64_t)deadline.tv_sec * 1000000000ull + deadline.tv_nsec;
      if (t >= end_ns) {
         result = VK_TIMEOUT;
         break;
      }
      t = MIN2(end_ns, t + 100 * 1000 * 1000ull);
      const struct timespec ts = {
         .tv_sec = t / 1000000000ull,
         .tv_nsec = t % 1000000000ull,
      };
      cnd_timedwait(&chain->cond, &chain->mutex, &ts);
   }
   if (index >= 0)
      chain->images[index].acquired = true;
   mtx_unlock(&chain->mutex);

   if (result != VK_SUCCESS)
      return result;

   /* The host has finished with the image, so the semaphore and fence can be
    * signaled right away. An imported sync fd of -1 means "signaled".
    */
   if (pAcquireInfo->semaphore != VK_NULL_HANDLE) {
      const VkImportSemaphoreFdInfoKHR info = {
         .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
         .semaphore = pAcquireInfo->semaphore,
         .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
         .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
         .fd = -1,
      };
      result = vn_ImportSemaphoreFdKHR(device, &info);
   }
   if (result == VK_SUCCESS && pAcquireInfo->fence != VK_NULL_HANDLE) {
      const VkImportFenceFdInfoKHR info = {
         .sType = VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR,
         .fence = pAcquireInfo->fence,
         .flags = VK_FENCE_IMPORT_TEMPORARY_BIT,
         .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,
         .fd = -1,
      };
      result = vn_ImportFenceFdKHR(device, &info);
   }

   if (result != VK_SUCCESS) {
      mtx_lock(&chain->mutex);
      chain->images[index].acquired = false;
      mtx_unlock(&chain->mutex);
      return vn_error(dev->instance, result);
   }

   *pImageIndex = index;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
vn_net_AcquireNextImageKHR(VkDevice device,
                           VkSwapchainKHR swapchain,
                           uint64_t timeout,
                           VkSemaphore semaphore,
                           VkFence fence,
                           uint32_t *pImageIndex)
{
   const VkAcquireNextImageInfoKHR info = {
      .sType = VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR,
      .swapchain = swapchain,
      .timeout = timeout,
      .semaphore = semaphore,
      .fence = fence,
      .deviceMask = 0x1,
   };
   return vn_net_AcquireNextImage2KHR(device, &info, pImageIndex);
}

VKAPI_ATTR VkResult VKAPI_CALL
vn_net_QueuePresentKHR(VkQueue _queue, const VkPresentInfoKHR *pPresentInfo)
{
   struct vn_queue *queue = vn_queue_from_handle(_queue);
   struct vn_device *dev = vn_device_from_vk(queue->base.vk.base.device);
   const struct vk_device_dispatch_table *vk = &dev->base.vk.dispatch_table;

   /* Consume the present semaphores on this queue. The host's copy goes on
    * the same queue after this, so it also runs after the app's rendering.
    */
   VkResult result = VK_SUCCESS;
   if (pPresentInfo->waitSemaphoreCount) {
      STACK_ARRAY(VkPipelineStageFlags, stages,
                  pPresentInfo->waitSemaphoreCount);
      for (uint32_t i = 0; i < pPresentInfo->waitSemaphoreCount; i++)
         stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
      const VkSubmitInfo submit = {
         .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
         .waitSemaphoreCount = pPresentInfo->waitSemaphoreCount,
         .pWaitSemaphores = pPresentInfo->pWaitSemaphores,
         .pWaitDstStageMask = stages,
      };
      result = vk->QueueSubmit(_queue, 1, &submit, VK_NULL_HANDLE);
      STACK_ARRAY_FINISH(stages);
      if (result != VK_SUCCESS)
         return vn_error(dev->instance, result);
   }

   const uint32_t ring_seqno = queue->ring_seqno_valid ? queue->ring_seqno : 0;
   const uint64_t ring_id = vn_ring_get_id(dev->primary_ring);

   VkResult final = VK_SUCCESS;
   for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
      struct vn_net_swapchain *chain =
         vn_net_swapchain_from_handle(pPresentInfo->pSwapchains[i]);
      const uint32_t index = pPresentInfo->pImageIndices[i];

      mtx_lock(&chain->mutex);
      struct vn_net_swapchain_image *img = &chain->images[index];
      img->acquired = false;
      img->busy = true;
      img->present_seq = ++chain->present_seq;
      mtx_unlock(&chain->mutex);

      const struct nm_present msg = {
         .swapchain_id = chain->id,
         .image_index = index,
         .device_id = dev->base.id,
         .queue_id = queue->base.id,
         .image_id = img->image_id,
         .ring_id = ring_id,
         .ring_seqno = ring_seqno,
         .queue_family = queue->base.vk.queue_family_index,
         .width = chain->extent.width,
         .height = chain->extent.height,
         .format = chain->format,
         .concurrent = chain->sharing == VK_SHARING_MODE_CONCURRENT,
      };
      result = vn_renderer_net_present(dev->renderer, &msg);

      if (pPresentInfo->pResults)
         pPresentInfo->pResults[i] = result;
      if (result != VK_SUCCESS)
         final = result;
   }

   return final;
}

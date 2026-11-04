/*
 * CAMetalLayer presenter. See nm_presenter_metal.h.
 *
 * Per frame: acquire a swapchain image, blit the remote image into it
 * (format conversion and all), present after the copy's submit. FIFO, so
 * acquire paces the stream to the display.
 *
 * SPDX-License-Identifier: MIT
 */
#define VK_USE_PLATFORM_METAL_EXT 1
#include "nm_presenter_metal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_IMAGES 8

const char *const nm_presenter_metal_instance_exts[2] = {
   "VK_KHR_surface",
   "VK_EXT_metal_surface",
};
const char *const nm_presenter_metal_device_exts[1] = {
   "VK_KHR_swapchain",
};

struct mtl_sink {
   const struct nm_metal_layer_ops *ops;
   void *layer;

   VkInstance instance;
   VkPhysicalDevice physical_device;
   VkDevice device;

   PFN_vkCreateMetalSurfaceEXT CreateMetalSurfaceEXT;
   PFN_vkDestroySurfaceKHR DestroySurfaceKHR;
   PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetSurfaceCapabilities;
   PFN_vkCreateSwapchainKHR CreateSwapchainKHR;
   PFN_vkDestroySwapchainKHR DestroySwapchainKHR;
   PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR;
   PFN_vkAcquireNextImageKHR AcquireNextImageKHR;
   PFN_vkQueuePresentKHR QueuePresentKHR;
   PFN_vkCreateSemaphore CreateSemaphore;
   PFN_vkDestroySemaphore DestroySemaphore;
   PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
   PFN_vkCmdBlitImage CmdBlitImage;
   PFN_vkDeviceWaitIdle DeviceWaitIdle;

   VkSurfaceKHR surface;
   VkSwapchainKHR swapchain;
   VkFormat format;
   VkExtent2D extent;
   uint32_t image_count;
   VkImage images[MAX_IMAGES];
   VkSemaphore render_done[MAX_IMAGES];
   VkSemaphore acquired[MAX_IMAGES + 1];
   uint32_t next_acquire_sem;

   /* the image this frame goes to, between record and submitted */
   bool have_image;
   uint32_t image_index;
   uint32_t swapchain_id;
   bool shown;
};

static bool
is_srgb(VkFormat f)
{
   return f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_SRGB;
}

static void
destroy_swapchain(struct mtl_sink *s)
{
   if (!s->swapchain)
      return;
   for (uint32_t i = 0; i < MAX_IMAGES; i++) {
      if (s->render_done[i])
         s->DestroySemaphore(s->device, s->render_done[i], NULL);
      s->render_done[i] = VK_NULL_HANDLE;
   }
   for (uint32_t i = 0; i < MAX_IMAGES + 1; i++) {
      if (s->acquired[i])
         s->DestroySemaphore(s->device, s->acquired[i], NULL);
      s->acquired[i] = VK_NULL_HANDLE;
   }
   s->DestroySwapchainKHR(s->device, s->swapchain, NULL);
   s->swapchain = VK_NULL_HANDLE;
}

static bool
create_swapchain(struct mtl_sink *s, VkFormat src_format, uint32_t w,
                 uint32_t h)
{
   VkSwapchainKHR old = s->swapchain;
   VkSurfaceCapabilitiesKHR caps;
   s->GetSurfaceCapabilities(s->physical_device, s->surface, &caps);

   s->format = is_srgb(src_format) ? VK_FORMAT_B8G8R8A8_SRGB
                                   : VK_FORMAT_B8G8R8A8_UNORM;
   s->extent = (VkExtent2D){ w, h };
   uint32_t count = caps.minImageCount < 3 ? 3 : caps.minImageCount;
   if (caps.maxImageCount && count > caps.maxImageCount)
      count = caps.maxImageCount;

   const VkSwapchainCreateInfoKHR info = {
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .surface = s->surface,
      .minImageCount = count,
      .imageFormat = s->format,
      .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
      .imageExtent = s->extent,
      .imageArrayLayers = 1,
      .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
      .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      .presentMode = VK_PRESENT_MODE_FIFO_KHR,
      .clipped = VK_TRUE,
      .oldSwapchain = old,
   };
   VkSwapchainKHR swapchain;
   VkResult r = s->CreateSwapchainKHR(s->device, &info, NULL, &swapchain);
   if (old)
      destroy_swapchain(s);
   if (r != VK_SUCCESS) {
      fprintf(stderr, "metal presenter: swapchain failed (%d)\n", r);
      return false;
   }
   s->swapchain = swapchain;

   s->image_count = MAX_IMAGES;
   s->GetSwapchainImagesKHR(s->device, swapchain, &s->image_count, s->images);

   const VkSemaphoreCreateInfo sem_info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
   };
   for (uint32_t i = 0; i < s->image_count; i++)
      s->CreateSemaphore(s->device, &sem_info, NULL, &s->render_done[i]);
   for (uint32_t i = 0; i < s->image_count + 1; i++)
      s->CreateSemaphore(s->device, &sem_info, NULL, &s->acquired[i]);
   s->next_acquire_sem = 0;
   return true;
}

static void *
mtl_create(void *user, const struct nm_presenter_device *dev,
           const struct nm_presenter_frame *first)
{
   const struct nm_metal_layer_ops *ops = user;
   struct mtl_sink *s = calloc(1, sizeof(*s));
   if (!s)
      return NULL;
   s->ops = ops;
   s->instance = dev->instance;
   s->physical_device = dev->physical_device;
   s->device = dev->device;
   s->swapchain_id = first->swapchain_id;

   PFN_vkGetInstanceProcAddr gipa = dev->get_instance_proc_addr;
   PFN_vkGetDeviceProcAddr gdpa = dev->get_device_proc_addr;
#define IGET(field, name) s->field = (void *)gipa(s->instance, name)
#define DGET(field, name) s->field = (void *)gdpa(s->device, name)
   IGET(CreateMetalSurfaceEXT, "vkCreateMetalSurfaceEXT");
   IGET(DestroySurfaceKHR, "vkDestroySurfaceKHR");
   IGET(GetSurfaceCapabilities, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
   DGET(CreateSwapchainKHR, "vkCreateSwapchainKHR");
   DGET(DestroySwapchainKHR, "vkDestroySwapchainKHR");
   DGET(GetSwapchainImagesKHR, "vkGetSwapchainImagesKHR");
   DGET(AcquireNextImageKHR, "vkAcquireNextImageKHR");
   DGET(QueuePresentKHR, "vkQueuePresentKHR");
   DGET(CreateSemaphore, "vkCreateSemaphore");
   DGET(DestroySemaphore, "vkDestroySemaphore");
   DGET(CmdPipelineBarrier, "vkCmdPipelineBarrier");
   DGET(CmdBlitImage, "vkCmdBlitImage");
   DGET(DeviceWaitIdle, "vkDeviceWaitIdle");
#undef IGET
#undef DGET

   if (!s->CreateMetalSurfaceEXT || !s->CreateSwapchainKHR) {
      fprintf(stderr, "metal presenter: VK_EXT_metal_surface/VK_KHR_swapchain "
                      "not enabled on the vkr device\n");
      free(s);
      return NULL;
   }

   s->layer = ops->create_layer(ops->user, first->width, first->height);
   const VkMetalSurfaceCreateInfoEXT surface_info = {
      .sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
      .pLayer = s->layer,
   };
   if (!s->layer || s->CreateMetalSurfaceEXT(s->instance, &surface_info, NULL,
                                             &s->surface) != VK_SUCCESS ||
       !create_swapchain(s, first->format, first->width, first->height)) {
      if (s->surface)
         s->DestroySurfaceKHR(s->instance, s->surface, NULL);
      if (s->layer)
         ops->destroy_layer(ops->user, s->layer);
      free(s);
      return NULL;
   }
   return s;
}

static void
image_barrier(struct mtl_sink *s, VkCommandBuffer cmd, VkImage image,
              VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
              VkAccessFlags dst_access, VkPipelineStageFlags src_stage,
              VkPipelineStageFlags dst_stage)
{
   const VkImageMemoryBarrier b = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = src_access,
      .dstAccessMask = dst_access,
      .oldLayout = from,
      .newLayout = to,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .levelCount = 1,
         .layerCount = 1,
      },
   };
   s->CmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1,
                         &b);
}

static void
mtl_record(void *sink, VkCommandBuffer cmd, VkImage src,
           const struct nm_presenter_frame *frame,
           struct nm_presenter_submit *submit)
{
   struct mtl_sink *s = sink;
   s->have_image = false;
   if (!s->swapchain)
      return;

   VkSemaphore sem = s->acquired[s->next_acquire_sem];
   uint32_t index;
   VkResult r = s->AcquireNextImageKHR(s->device, s->swapchain, UINT64_MAX,
                                       sem, VK_NULL_HANDLE, &index);
   if (r == VK_ERROR_OUT_OF_DATE_KHR) {
      if (!create_swapchain(s, frame->format, frame->width, frame->height))
         return;
      sem = s->acquired[s->next_acquire_sem];
      r = s->AcquireNextImageKHR(s->device, s->swapchain, UINT64_MAX, sem,
                                 VK_NULL_HANDLE, &index);
   }
   if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
      return;
   s->next_acquire_sem = (s->next_acquire_sem + 1) % (s->image_count + 1);

   VkImage dst = s->images[index];
   image_barrier(s, cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                 VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT);
   const VkImageBlit region = {
      .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
      .srcOffsets = { { 0, 0, 0 },
                      { (int32_t)frame->width, (int32_t)frame->height, 1 } },
      .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
      .dstOffsets = { { 0, 0, 0 },
                      { (int32_t)s->extent.width, (int32_t)s->extent.height,
                        1 } },
   };
   s->CmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                   VK_FILTER_LINEAR);
   image_barrier(s, cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT,
                 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

   submit->wait_semaphore = sem;
   submit->wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
   submit->signal_semaphore = s->render_done[index];
   s->have_image = true;
   s->image_index = index;
}

static void
mtl_submitted(void *sink, VkQueue queue, const struct nm_presenter_frame *frame)
{
   struct mtl_sink *s = sink;
   if (!s->have_image)
      return;
   const VkPresentInfoKHR info = {
      .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &s->render_done[s->image_index],
      .swapchainCount = 1,
      .pSwapchains = &s->swapchain,
      .pImageIndices = &s->image_index,
   };
   s->QueuePresentKHR(queue, &info);
   s->have_image = false;

   if (!s->shown) {
      s->shown = true;
      if (s->ops->first_frame)
         s->ops->first_frame(s->ops->user, frame->width, frame->height);
   }
}

static void
mtl_destroy(void *sink)
{
   struct mtl_sink *s = sink;
   /* the core waited for our copies; let presents drain too */
   s->DeviceWaitIdle(s->device);
   destroy_swapchain(s);
   s->DestroySurfaceKHR(s->instance, s->surface, NULL);
   s->ops->destroy_layer(s->ops->user, s->layer);
   free(s);
}

const struct nm_presenter_ops nm_presenter_metal = {
   .create = mtl_create,
   .record = mtl_record,
   .submitted = mtl_submitted,
   .destroy = mtl_destroy,
};

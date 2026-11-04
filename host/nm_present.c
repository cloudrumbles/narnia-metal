/*
 * narnia-metal host remote WSI core. See nm_present.h and docs/design.md.
 *
 * SPDX-License-Identifier: MIT
 */
#include "nm_present.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vkr_renderer.h"

/* VK_COMMAND_TYPE_vkWaitRingSeqnoMESA_EXT */
#define NM_CMD_WAIT_RING_SEQNO 253

#define NM_MAX_IMAGES 8

static const struct nm_presenter_ops *nm_presenter;
static void *nm_presenter_user;

void
nm_host_set_presenter(const struct nm_presenter_ops *ops, void *user)
{
   nm_presenter = ops;
   nm_presenter_user = user;
}

struct nm_present_vk {
   PFN_vkDestroyCommandPool DestroyCommandPool;
   PFN_vkCreateCommandPool CreateCommandPool;
   PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
   PFN_vkResetCommandBuffer ResetCommandBuffer;
   PFN_vkBeginCommandBuffer BeginCommandBuffer;
   PFN_vkEndCommandBuffer EndCommandBuffer;
   PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
   PFN_vkCreateFence CreateFence;
   PFN_vkDestroyFence DestroyFence;
   PFN_vkResetFences ResetFences;
   PFN_vkWaitForFences WaitForFences;
   PFN_vkQueueSubmit QueueSubmit;
};

struct nm_present_slot {
   VkCommandBuffer cmd;
   VkFence fence;
   bool in_flight;
};

struct nm_present_chain {
   struct nm_present_chain *next;
   uint32_t id;

   struct nm_presenter_device pdev;
   struct nm_present_vk vk;
   VkCommandPool pool;
   struct nm_present_slot slots[NM_MAX_IMAGES];

   void *sink;
   uint64_t frames;
   uint32_t in_flight;
};

struct nm_present_job {
   struct nm_present_job *next;
   struct nm_present_chain *chain;
   struct nm_presenter_frame frame;
};

/* ------------------------------------------------------------------ */

static void *
nm_present_worker(void *arg)
{
   struct nm_present_state *state = arg;

   pthread_mutex_lock(&state->mutex);
   while (true) {
      while (!state->jobs_head && !state->stop)
         pthread_cond_wait(&state->cond, &state->mutex);
      if (!state->jobs_head && state->stop)
         break;

      struct nm_present_job *job = state->jobs_head;
      state->jobs_head = job->next;
      if (!state->jobs_head)
         state->jobs_tail = NULL;
      pthread_mutex_unlock(&state->mutex);

      struct nm_present_chain *chain = job->chain;
      struct nm_present_slot *slot = &chain->slots[job->frame.image_index];
      chain->vk.WaitForFences(chain->pdev.device, 1, &slot->fence, VK_TRUE,
                              UINT64_MAX);
      if (nm_presenter && nm_presenter->complete)
         nm_presenter->complete(chain->sink, &job->frame);

      pthread_mutex_lock(&state->mutex);
      slot->in_flight = false;
      chain->in_flight--;
      pthread_cond_broadcast(&state->cond);
      pthread_mutex_unlock(&state->mutex);

      state->done(state->done_user, job->frame.swapchain_id,
                  job->frame.image_index);
      free(job);

      pthread_mutex_lock(&state->mutex);
   }
   pthread_mutex_unlock(&state->mutex);
   return NULL;
}

void
nm_present_init(struct nm_present_state *state,
                uint32_t ctx_id,
                PFN_vkGetInstanceProcAddr gipa,
                nm_present_done_func done,
                void *done_user)
{
   memset(state, 0, sizeof(*state));
   state->ctx_id = ctx_id;
   state->get_instance_proc_addr = gipa;
   state->done = done;
   state->done_user = done_user;
   pthread_mutex_init(&state->mutex, NULL);
   pthread_cond_init(&state->cond, NULL);
}

static void
nm_present_chain_free(struct nm_present_chain *chain)
{
   for (uint32_t i = 0; i < NM_MAX_IMAGES; i++) {
      if (chain->slots[i].fence)
         chain->vk.DestroyFence(chain->pdev.device, chain->slots[i].fence,
                                NULL);
   }
   if (chain->pool)
      chain->vk.DestroyCommandPool(chain->pdev.device, chain->pool, NULL);
   if (chain->sink && nm_presenter)
      nm_presenter->destroy(chain->sink);
   free(chain);
}

static struct nm_present_chain *
nm_present_chain_create(struct nm_present_state *state,
                        const struct nm_present *p,
                        const struct vkr_renderer_present_handles *h,
                        char *err,
                        size_t err_size)
{
   struct nm_present_chain *chain = calloc(1, sizeof(*chain));
   if (!chain)
      return NULL;
   chain->id = p->swapchain_id;

   PFN_vkGetInstanceProcAddr gipa = state->get_instance_proc_addr;
   VkInstance instance = h->instance;
   PFN_vkGetDeviceProcAddr gdpa =
      (PFN_vkGetDeviceProcAddr)gipa(instance, "vkGetDeviceProcAddr");
   chain->pdev = (struct nm_presenter_device){
      .instance = instance,
      .physical_device = h->physical_device,
      .device = h->device,
      .get_instance_proc_addr = gipa,
      .get_device_proc_addr = gdpa,
      .queue_family = p->queue_family,
   };

   VkDevice device = h->device;
#define GET(name) chain->vk.name = (PFN_vk##name)gdpa(device, "vk" #name)
   GET(DestroyCommandPool);
   GET(CreateCommandPool);
   GET(AllocateCommandBuffers);
   GET(ResetCommandBuffer);
   GET(BeginCommandBuffer);
   GET(EndCommandBuffer);
   GET(CmdPipelineBarrier);
   GET(CreateFence);
   GET(DestroyFence);
   GET(ResetFences);
   GET(WaitForFences);
   GET(QueueSubmit);
#undef GET

   const VkCommandPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = p->queue_family,
   };
   if (chain->vk.CreateCommandPool(device, &pool_info, NULL, &chain->pool) !=
       VK_SUCCESS) {
      snprintf(err, err_size, "swapchain %u: command pool failed",
               p->swapchain_id);
      nm_present_chain_free(chain);
      return NULL;
   }

   const VkCommandBufferAllocateInfo cmd_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = chain->pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   const VkFenceCreateInfo fence_info = {
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
   };
   for (uint32_t i = 0; i < NM_MAX_IMAGES; i++) {
      if (chain->vk.AllocateCommandBuffers(device, &cmd_info,
                                           &chain->slots[i].cmd) !=
             VK_SUCCESS ||
          chain->vk.CreateFence(device, &fence_info, NULL,
                                &chain->slots[i].fence) != VK_SUCCESS) {
         snprintf(err, err_size, "swapchain %u: command buffer/fence failed",
                  p->swapchain_id);
         nm_present_chain_free(chain);
         return NULL;
      }
   }

   if (nm_presenter) {
      const struct nm_presenter_frame first = {
         .swapchain_id = p->swapchain_id,
         .width = p->width,
         .height = p->height,
         .format = p->format,
      };
      chain->sink = nm_presenter->create(nm_presenter_user, &chain->pdev,
                                         &first);
      if (!chain->sink) {
         snprintf(err, err_size, "swapchain %u: presenter failed",
                  p->swapchain_id);
         nm_present_chain_free(chain);
         return NULL;
      }
   }

   return chain;
}

static void
nm_present_record_barriers(struct nm_present_chain *chain,
                           VkCommandBuffer cmd,
                           const struct nm_present *p,
                           VkImage image,
                           bool acquire)
{
   /* Venus keeps presentable images in GENERAL, owned by the foreign queue
    * family (see vn_cmd_fix_image_memory_barrier_common). Take ownership,
    * copy, and hand it back the same way.
    */
   const uint32_t foreign = VK_QUEUE_FAMILY_FOREIGN_EXT;
   const uint32_t ours =
      p->concurrent ? VK_QUEUE_FAMILY_IGNORED : p->queue_family;

   const VkImageMemoryBarrier barrier = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = acquire ? 0 : VK_ACCESS_TRANSFER_READ_BIT,
      .dstAccessMask = acquire ? VK_ACCESS_TRANSFER_READ_BIT : 0,
      .oldLayout = acquire ? VK_IMAGE_LAYOUT_GENERAL
                           : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .newLayout = acquire ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                           : VK_IMAGE_LAYOUT_GENERAL,
      .srcQueueFamilyIndex = acquire ? foreign : ours,
      .dstQueueFamilyIndex = acquire ? ours : foreign,
      .image = image,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .levelCount = 1,
         .layerCount = 1,
      },
   };
   chain->vk.CmdPipelineBarrier(
      cmd,
      acquire ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
              : VK_PIPELINE_STAGE_TRANSFER_BIT,
      acquire ? VK_PIPELINE_STAGE_TRANSFER_BIT
              : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
      0, 0, NULL, 0, NULL, 1, &barrier);
}

bool
nm_present_handle(struct nm_present_state *state,
                  const struct nm_present *p,
                  char *err,
                  size_t err_size)
{
   if (p->image_index >= NM_MAX_IMAGES) {
      snprintf(err, err_size, "image index %u out of range", p->image_index);
      return false;
   }

   /* wait until the ring has handed the app's submits to the queue */
   /* type, flags, uint64 ring, uint64 seqno */
   uint32_t cmd[6];
   cmd[0] = NM_CMD_WAIT_RING_SEQNO;
   cmd[1] = 0;
   memcpy(&cmd[2], &p->ring_id, sizeof(uint64_t));
   cmd[4] = p->ring_seqno;
   cmd[5] = 0;
   if (!vkr_renderer_submit_cmd(state->ctx_id, cmd, sizeof(cmd))) {
      snprintf(err, err_size, "ring wait failed");
      return false;
   }

   struct vkr_renderer_present_handles h;
   if (!vkr_renderer_get_present_handles(state->ctx_id, p->device_id,
                                         p->queue_id, p->image_id, &h)) {
      snprintf(err, err_size, "unknown device/queue/image in present");
      return false;
   }

   pthread_mutex_lock(&state->mutex);
   struct nm_present_chain *chain = state->chains;
   while (chain && chain->id != p->swapchain_id)
      chain = chain->next;
   pthread_mutex_unlock(&state->mutex);

   if (!chain) {
      chain = nm_present_chain_create(state, p, &h, err, err_size);
      if (!chain)
         return false;
      pthread_mutex_lock(&state->mutex);
      chain->next = state->chains;
      state->chains = chain;
      if (!state->worker_started) {
         state->worker_started =
            !pthread_create(&state->worker, NULL, nm_present_worker, state);
      }
      pthread_mutex_unlock(&state->mutex);
   }

   struct nm_present_slot *slot = &chain->slots[p->image_index];

   /* the guest only presents an image after NM_PRESENT_DONE, but be safe */
   pthread_mutex_lock(&state->mutex);
   while (slot->in_flight)
      pthread_cond_wait(&state->cond, &state->mutex);
   pthread_mutex_unlock(&state->mutex);

   const struct nm_presenter_frame frame = {
      .swapchain_id = p->swapchain_id,
      .image_index = p->image_index,
      .width = p->width,
      .height = p->height,
      .format = p->format,
      .frame = chain->frames++,
   };

   VkImage image = (VkImage)(uintptr_t)h.image;
   VkCommandBuffer cmdbuf = slot->cmd;
   chain->vk.ResetFences(chain->pdev.device, 1, &slot->fence);
   chain->vk.ResetCommandBuffer(cmdbuf, 0);
   const VkCommandBufferBeginInfo begin = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   chain->vk.BeginCommandBuffer(cmdbuf, &begin);
   nm_present_record_barriers(chain, cmdbuf, p, image, true);
   if (nm_presenter)
      nm_presenter->record(chain->sink, cmdbuf, image, &frame);
   nm_present_record_barriers(chain, cmdbuf, p, image, false);
   chain->vk.EndCommandBuffer(cmdbuf);

   const VkSubmitInfo submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &cmdbuf,
   };
   if (!vkr_renderer_lock_queue(state->ctx_id, p->queue_id)) {
      snprintf(err, err_size, "queue lock failed");
      return false;
   }
   VkResult result = chain->vk.QueueSubmit(h.queue, 1, &submit, slot->fence);
   if (result == VK_SUCCESS && nm_presenter && nm_presenter->submitted)
      nm_presenter->submitted(chain->sink, h.queue, &frame);
   vkr_renderer_unlock_queue(state->ctx_id, p->queue_id);
   if (result != VK_SUCCESS) {
      snprintf(err, err_size, "present submit failed (%d)", result);
      return false;
   }

   struct nm_present_job *job = calloc(1, sizeof(*job));
   if (!job) {
      chain->vk.WaitForFences(chain->pdev.device, 1, &slot->fence, VK_TRUE,
                              UINT64_MAX);
      state->done(state->done_user, p->swapchain_id, p->image_index);
      return true;
   }
   job->chain = chain;
   job->frame = frame;

   pthread_mutex_lock(&state->mutex);
   slot->in_flight = true;
   chain->in_flight++;
   if (state->jobs_tail)
      state->jobs_tail->next = job;
   else
      state->jobs_head = job;
   state->jobs_tail = job;
   pthread_cond_broadcast(&state->cond);
   pthread_mutex_unlock(&state->mutex);

   return true;
}

void
nm_present_destroy_swapchain(struct nm_present_state *state,
                             uint32_t swapchain_id)
{
   pthread_mutex_lock(&state->mutex);
   struct nm_present_chain **link = &state->chains;
   while (*link && (*link)->id != swapchain_id)
      link = &(*link)->next;
   struct nm_present_chain *chain = *link;
   if (chain) {
      while (chain->in_flight)
         pthread_cond_wait(&state->cond, &state->mutex);
      *link = chain->next;
   }
   pthread_mutex_unlock(&state->mutex);

   if (chain)
      nm_present_chain_free(chain);
}

void
nm_present_fini(struct nm_present_state *state)
{
   while (true) {
      pthread_mutex_lock(&state->mutex);
      const uint32_t id = state->chains ? state->chains->id : 0;
      const bool any = state->chains != NULL;
      pthread_mutex_unlock(&state->mutex);
      if (!any)
         break;
      nm_present_destroy_swapchain(state, id);
   }

   pthread_mutex_lock(&state->mutex);
   state->stop = true;
   pthread_cond_broadcast(&state->cond);
   pthread_mutex_unlock(&state->mutex);
   if (state->worker_started)
      pthread_join(state->worker, NULL);

   pthread_mutex_destroy(&state->mutex);
   pthread_cond_destroy(&state->cond);
}

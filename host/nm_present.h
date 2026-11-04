/*
 * narnia-metal host remote WSI.
 *
 * The core (nm_present.c) turns NM_PRESENT messages into a copy on the app's
 * own VkQueue and reports NM_PRESENT_DONE when the copy has finished. What
 * the copy goes into is up to a presenter module:
 *   - nm_presenter_png.c: Linux test host, reads frames back and dumps PNGs
 *   - (planned) iOS: blit into a CAMetalLayer swapchain image and present
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef NM_PRESENT_H
#define NM_PRESENT_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#include "nm_proto.h"

struct nm_presenter_device {
   VkInstance instance;
   VkPhysicalDevice physical_device;
   VkDevice device;
   PFN_vkGetInstanceProcAddr get_instance_proc_addr;
   PFN_vkGetDeviceProcAddr get_device_proc_addr;
   uint32_t queue_family;
};

struct nm_presenter_frame {
   uint32_t swapchain_id;
   uint32_t image_index;
   uint32_t width;
   uint32_t height;
   VkFormat format;
   uint64_t frame; /* counts from 0 per swapchain */
};

/* semaphores the presenter wants on the copy's queue submit (optional) */
struct nm_presenter_submit {
   VkSemaphore wait_semaphore;
   VkPipelineStageFlags wait_stage;
   VkSemaphore signal_semaphore;
};

/* All callbacks for one swapchain run on one thread at a time. */
struct nm_presenter_ops {
   /* a new swapchain; returns the presenter's per-swapchain state */
   void *(*create)(void *user,
                   const struct nm_presenter_device *dev,
                   const struct nm_presenter_frame *first);
   /* record commands that read src, which is in TRANSFER_SRC_OPTIMAL;
    * may ask for semaphores on the submit
    */
   void (*record)(void *sink,
                  VkCommandBuffer cmd,
                  VkImage src,
                  const struct nm_presenter_frame *frame,
                  struct nm_presenter_submit *submit);
   /* right after the submit, with the queue still locked (optional) */
   void (*submitted)(void *sink,
                     VkQueue queue,
                     const struct nm_presenter_frame *frame);
   /* the recorded commands have finished on the GPU (optional) */
   void (*complete)(void *sink, const struct nm_presenter_frame *frame);
   void (*destroy)(void *sink);
};

/* set once, before any connection */
void
nm_host_set_presenter(const struct nm_presenter_ops *ops, void *user);

/* ---- used by nm_host.c ---- */

typedef void (*nm_present_done_func)(void *user,
                                     uint32_t swapchain_id,
                                     uint32_t image_index);
typedef void (*nm_present_log_func)(const char *fmt, ...);

struct nm_present_chain;
struct nm_present_job;

struct nm_present_state {
   uint32_t ctx_id;
   PFN_vkGetInstanceProcAddr get_instance_proc_addr;
   nm_present_done_func done;
   void *done_user;

   pthread_mutex_t mutex;
   pthread_cond_t cond;
   struct nm_present_chain *chains;
   struct nm_present_job *jobs_head, *jobs_tail;
   bool stop;
   bool worker_started;
   pthread_t worker;
};

void
nm_present_init(struct nm_present_state *state,
                uint32_t ctx_id,
                PFN_vkGetInstanceProcAddr gipa,
                nm_present_done_func done,
                void *done_user);

/* returns false and fills err on failure */
bool
nm_present_handle(struct nm_present_state *state,
                  const struct nm_present *present,
                  char *err,
                  size_t err_size);

void
nm_present_destroy_swapchain(struct nm_present_state *state,
                             uint32_t swapchain_id);

/* destroys every swapchain; call before the vkr context goes away */
void
nm_present_fini(struct nm_present_state *state);

#endif /* NM_PRESENT_H */

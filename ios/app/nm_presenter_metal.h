/*
 * Presenter module for Apple platforms: shows frames on a CAMetalLayer
 * through a VK_EXT_metal_surface swapchain on the app's own VkDevice.
 *
 * Every remote swapchain gets its own CAMetalLayer (made by the app on the
 * main thread), so two apps or a recreated swapchain never fight over one
 * layer; the newest layer sits on top.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef NM_PRESENTER_METAL_H
#define NM_PRESENTER_METAL_H

#include "nm_present.h"

struct nm_metal_layer_ops {
   /* returns a retained CAMetalLayer (as void *) shown full screen */
   void *(*create_layer)(void *user, uint32_t width, uint32_t height);
   /* removes and releases it */
   void (*destroy_layer)(void *user, void *layer);
   /* first frame of a swapchain was shown (optional) */
   void (*first_frame)(void *user, uint32_t width, uint32_t height);
   void *user;
};

extern const struct nm_presenter_ops nm_presenter_metal;

/* pass as the user pointer to nm_host_set_presenter */
static inline void *
nm_presenter_metal_user(const struct nm_metal_layer_ops *ops)
{
   return (void *)ops;
}

/* extensions vkr must enable for this presenter */
extern const char *const nm_presenter_metal_instance_exts[2];
extern const char *const nm_presenter_metal_device_exts[1];

#endif /* NM_PRESENTER_METAL_H */

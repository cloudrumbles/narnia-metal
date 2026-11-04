/*
 * Copyright 2026 Shahriyyah
 * SPDX-License-Identifier: MIT
 *
 * Extra entry points of the narnia-metal net backend, used by the remote WSI
 * in vn_net_wsi.c. Only valid when renderer->info.remote_wsi is set.
 */
#ifndef VN_RENDERER_NET_H
#define VN_RENDERER_NET_H

#include "nm_proto.h"
#include "vn_renderer.h"

typedef void (*vn_renderer_net_present_done_func)(void *data,
                                                  uint32_t image_index);

/* Returns a swapchain id. done is called from the receive thread whenever the
 * host is done with an image, until the swapchain is unregistered.
 */
uint32_t
vn_renderer_net_register_swapchain(struct vn_renderer *renderer,
                                   vn_renderer_net_present_done_func done,
                                   void *data);

/* Stops callbacks and tells the host to drop its swapchain state. */
void
vn_renderer_net_unregister_swapchain(struct vn_renderer *renderer,
                                     uint32_t swapchain_id);

VkResult
vn_renderer_net_present(struct vn_renderer *renderer,
                        const struct nm_present *present);

bool
vn_renderer_net_is_lost(struct vn_renderer *renderer);

#endif /* VN_RENDERER_NET_H */

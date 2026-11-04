/*
 * narnia-metal host Vulkan driver seam.
 *
 * Each driver module (nm_driver_loader.c for the system Vulkan loader on
 * Linux, later MoltenVK / KosmicKrisp on iOS) implements this one function.
 * Nothing else in the host depends on the driver, so swapping drivers means
 * linking a different module.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef NM_DRIVER_H
#define NM_DRIVER_H

#include "nm_host.h"

/* Returns vkGetInstanceProcAddr of the driver, or NULL on failure. */
nm_vk_proc
nm_driver_get_instance_proc_addr(void);

/* human-readable driver module name, for logs */
const char *
nm_driver_name(void);

#endif /* NM_DRIVER_H */

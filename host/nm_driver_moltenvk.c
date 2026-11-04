/*
 * Driver module: MoltenVK, linked statically (iOS app, macOS CI host).
 * No Vulkan loader is involved; vkr talks to MoltenVK directly.
 *
 * Swapping in KosmicKrisp means a module like this one that returns its
 * vkGetInstanceProcAddr instead.
 *
 * SPDX-License-Identifier: MIT
 */
#include <vulkan/vulkan.h>

#include "nm_driver.h"

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *name);

nm_vk_proc
nm_driver_get_instance_proc_addr(void)
{
   return (nm_vk_proc)vkGetInstanceProcAddr;
}

const char *
nm_driver_name(void)
{
   return "moltenvk";
}

/*
 * Driver module: the system Vulkan loader (libvulkan.so.1). On Linux the ICD
 * is picked the usual way, e.g. VK_DRIVER_FILES=.../lvp_icd.x86_64.json.
 *
 * SPDX-License-Identifier: MIT
 */
#include <dlfcn.h>
#include <stddef.h>
#include <stdio.h>

#include "nm_driver.h"

nm_vk_proc
nm_driver_get_instance_proc_addr(void)
{
   void *lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
   if (!lib)
      lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
   if (!lib) {
      fprintf(stderr, "nm-host: failed to load libvulkan: %s\n", dlerror());
      return NULL;
   }
   return (nm_vk_proc)dlsym(lib, "vkGetInstanceProcAddr");
}

const char *
nm_driver_name(void)
{
   return "vulkan-loader";
}

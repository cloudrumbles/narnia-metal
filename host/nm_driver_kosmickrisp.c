/*
 * Driver module: KosmicKrisp (Mesa's Vulkan-on-Metal driver), shipped as
 * libvulkan_kosmickrisp.dylib next to the host: in the app bundle's
 * Frameworks/ on iOS, next to the binary on macOS (or NM_KK_DYLIB=<path>).
 * It's dlopen()ed and used without a Vulkan loader; our Mesa patch 0003
 * exports vkGetInstanceProcAddr (vk_icdGetInstanceProcAddr works too).
 *
 * SPDX-License-Identifier: MIT
 */
#include <dlfcn.h>
#include <libgen.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nm_driver.h"

static void *
try_open(const char *path)
{
   void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
   if (!lib)
      fprintf(stderr, "kosmickrisp: dlopen %s: %s\n", path, dlerror());
   return lib;
}

nm_vk_proc
nm_driver_get_instance_proc_addr(void)
{
   void *lib = NULL;
   const char *env = getenv("NM_KK_DYLIB");
   if (env && *env)
      lib = try_open(env);

   if (!lib) {
      char exe[PATH_MAX], path[PATH_MAX];
      uint32_t size = sizeof(exe);
      if (!_NSGetExecutablePath(exe, &size)) {
         const char *dir = dirname(exe);
         /* iOS app bundle: <app>/Frameworks; macOS CLI: next to the binary */
         snprintf(path, sizeof(path), "%s/Frameworks/libvulkan_kosmickrisp.dylib", dir);
         lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
         if (!lib) {
            snprintf(path, sizeof(path), "%s/libvulkan_kosmickrisp.dylib", dir);
            lib = try_open(path);
         }
      }
   }
   if (!lib)
      return NULL;

   void *gipa = dlsym(lib, "vkGetInstanceProcAddr");
   if (!gipa)
      gipa = dlsym(lib, "vk_icdGetInstanceProcAddr");
   if (!gipa)
      fprintf(stderr, "kosmickrisp: no vkGetInstanceProcAddr in the dylib\n");
   return (nm_vk_proc)gipa;
}

const char *
nm_driver_name(void)
{
   return "kosmickrisp";
}

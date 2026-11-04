/*
 * narnia-metal host core: runs Venus streams from the network on vkr.
 *
 * The core knows nothing about sockets or about which Vulkan driver is used.
 * The embedder (Linux test host, iOS app) provides:
 *   - a vkGetInstanceProcAddr for the Vulkan driver (see nm_driver.h),
 *   - a transport per connection (blocking read/write of the byte stream).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef NM_HOST_H
#define NM_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct nm_transport {
   void *user;
   /* read exactly size bytes; false on EOF or error */
   bool (*read)(void *user, void *data, size_t size);
   /* write exactly size bytes; false on error. Called with a lock held, from
    * several threads, so it only needs to be safe against close.
    */
   bool (*write)(void *user, const void *data, size_t size);
};

typedef void (*nm_log_func)(const char *msg);

/* generic function pointer; really a PFN_vkGetInstanceProcAddr */
typedef void (*nm_vk_proc)(void);

/* Call once. */
bool
nm_host_init(nm_vk_proc get_instance_proc_addr, nm_log_func log);

/* Serve one guest connection until it closes. Blocking; call it from a
 * thread per connection. Each connection is one vkr context.
 */
void
nm_host_serve(const struct nm_transport *transport);

#endif /* NM_HOST_H */

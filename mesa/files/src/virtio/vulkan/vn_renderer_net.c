/*
 * Copyright 2026 Shahriyyah
 * SPDX-License-Identifier: MIT
 *
 * narnia-metal network backend for Venus.
 *
 * Talks to a remote vkr over a byte stream instead of sharing memory with a
 * local renderer. Every shmem and mappable BO has a local copy plus a shadow
 * copy. At release points (direct submits and ring tail stores) the pages
 * that differ from the shadow go to the host as XOR deltas. The host sends
 * back exactly what it writes (reply bytes, ring head and status). Renderer
 * syncs live here and are signalled by fence retirement messages.
 *
 * See docs/design.md in the narnia-metal repo.
 */

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <linux/fs.h>
#include <linux/userfaultfd.h>
#include <zlib.h>

#include "c11/threads.h"
#include "util/os_misc.h"
#include "util/u_process.h"
#include "virtio/virtio-gpu/venus_hw.h"
#include "venus-protocol/vn_protocol_driver_defines.h"

#include "nm_proto.h"
#include "vn_renderer_internal.h"
#include "vn_renderer_net.h"

#define NET_PCI_VENDOR_ID 0x1af4
#define NET_PCI_DEVICE_ID 0x1050

#define NET_PAGE_SIZE 4096

/* same values as VIRGL_RENDERER_BLOB_FLAG_* and VCMD_BLOB_FLAG_* */
#define NET_BLOB_FLAG_MAPPABLE 0x1
#define NET_BLOB_FLAG_SHAREABLE 0x2
#define NET_BLOB_FLAG_CROSS_DEVICE 0x4

#define NET_MAX_MASKS 4

/* local copy of a resource, mirrored to the host */
struct net_mem {
   uint32_t res_id;
   size_t size;
   size_t map_size; /* size rounded up to pages */
   bool tracked;    /* writes are tracked with userfaultfd */
   uint8_t *ptr;    /* NULL until mapped */
   uint8_t *shadow; /* what the host is known to have */

   /* in net->mems while mapped */
   struct list_head link;
   bool linked;

   /* 32-bit words never sent as part of a diff (ring head/tail/status) */
   uint32_t mask_count;
   uint32_t masks[NET_MAX_MASKS];
};

struct net_shmem {
   struct vn_renderer_shmem base;
   struct net_mem mem;
};

struct net_bo {
   struct vn_renderer_bo base;
   struct net_mem mem;
   uint32_t blob_flags;
};

struct net_sync {
   struct vn_renderer_sync base;
   uint64_t value; /* protected by net->sync_mutex */
};

struct net_pending_item {
   struct net_sync *sync; /* NULL once the sync is destroyed */
   uint64_t value;
};

struct net_pending {
   struct list_head link;
   uint32_t ring_idx;
   uint64_t fence_id;
   uint32_t count;
   struct net_pending_item items[];
};

struct net_slot {
   struct net_mem *mem;
};

struct net_swapchain {
   struct list_head link;
   uint32_t id;
   vn_renderer_net_present_done_func done;
   void *data;
};

struct net_buf {
   uint8_t *data;
   size_t len;
   size_t cap;
};

struct net {
   struct vn_renderer base;
   struct vn_instance *instance;

   int fd;
   atomic_bool lost;

   /* Held while building and sending a batch of messages, so batches reach
    * the host in the order their memory snapshots were taken. Lock order:
    * send_mutex, then mem_mutex, then sync_mutex.
    */
   mtx_t send_mutex;
   struct net_buf out;

   /* protects mems, slots, and the contents of ptr vs shadow */
   mtx_t mem_mutex;
   struct list_head mems;
   struct util_sparse_array slots;
   atomic_uint next_res_id;

   mtx_t sync_mutex;
   cnd_t sync_cond;
   struct list_head pending;
   uint64_t next_fence_id;
   atomic_uint next_sync_id;

   thrd_t recv_thread;
   bool recv_started;

   /* remote WSI swapchains, for NM_PRESENT_DONE */
   mtx_t swapchain_mutex;
   struct list_head swapchains;
   uint32_t next_swapchain_id;

   uint32_t max_timeline_count;
   struct virgl_renderer_capset_venus capset;

   /* dirty page tracking: userfaultfd async write-protect + PAGEMAP_SCAN */
   int uffd;
   int pagemap_fd;

   /* XOR deltas are built here, then deflated into the out buffer */
   uint8_t *delta;
   size_t delta_cap;
   bool compress;

   /* VN_NET_STATS=1: bytes of deltas before/after compression */
   bool stats;
   uint64_t stat_raw, stat_wire, stat_msgs;
   int64_t stat_last;

   struct vn_renderer_shmem_cache shmem_cache;
};

/* ------------------------------------------------------------------ */
/* socket and message helpers                                         */

static bool
net_write_all(struct net *net, const void *data, size_t size)
{
   const uint8_t *p = data;
   while (size) {
      const ssize_t ret = send(net->fd, p, size, MSG_NOSIGNAL);
      if (ret < 0) {
         if (errno == EINTR)
            continue;
         if (!net->lost)
            vn_log(net->instance, "net: lost connection on send: %s",
                   strerror(errno));
         net->lost = true;
         return false;
      }
      p += ret;
      size -= ret;
   }
   return true;
}

static bool
net_read_all(struct net *net, void *data, size_t size)
{
   uint8_t *p = data;
   while (size) {
      const ssize_t ret = recv(net->fd, p, size, 0);
      if (ret < 0 && errno == EINTR)
         continue;
      if (ret <= 0)
         return false;
      p += ret;
      size -= ret;
   }
   return true;
}

static bool
net_buf_reserve(struct net_buf *buf, size_t extra)
{
   if (buf->len + extra <= buf->cap)
      return true;
   size_t cap = MAX2(buf->cap * 2, 64 * 1024);
   while (cap < buf->len + extra)
      cap *= 2;
   uint8_t *data = realloc(buf->data, cap);
   if (!data)
      return false;
   buf->data = data;
   buf->cap = cap;
   return true;
}

/* Append a message header and return a pointer to its payload. The pointer
 * is only valid until the next append.
 */
static void *
net_append(struct net *net, uint32_t type, size_t payload_len)
{
   struct net_buf *buf = &net->out;
   if (!net_buf_reserve(buf, sizeof(struct nm_hdr) + payload_len))
      return NULL;
   const struct nm_hdr hdr = { .type = type, .len = payload_len };
   memcpy(buf->data + buf->len, &hdr, sizeof(hdr));
   void *payload = buf->data + buf->len + sizeof(hdr);
   buf->len += sizeof(hdr) + payload_len;
   return payload;
}

static bool
net_send_out_locked(struct net *net)
{
   struct net_buf *buf = &net->out;
   bool ok = true;
   if (buf->len && !net->lost)
      ok = net_write_all(net, buf->data, buf->len);
   buf->len = 0;
   /* don't hold on to huge buffers after a big upload */
   if (buf->cap > 64 * 1024 * 1024) {
      free(buf->data);
      buf->data = NULL;
      buf->cap = 0;
   }
   return ok && !net->lost;
}

/* ------------------------------------------------------------------ */
/* memory mirroring                                                   */

static inline bool
net_mask_in_range(uint32_t mask, size_t begin, size_t end)
{
   return mask + sizeof(uint32_t) > begin && mask < end;
}

static void
net_report_stats(struct net *net)
{
   const int64_t now = os_time_get_nano();
   if (now - net->stat_last < 5ll * 1000 * 1000 * 1000)
      return;
   const double secs = (now - net->stat_last) / 1e9;
   if (net->stat_last) {
      fprintf(stderr,
              "vn-net: deltas %.1f MB/s raw -> %.1f MB/s on the wire "
              "(%.0f%%), %.0f msgs/s\n",
              net->stat_raw / secs / 1e6, net->stat_wire / secs / 1e6,
              net->stat_raw ? 100.0 * net->stat_wire / net->stat_raw : 0.0,
              net->stat_msgs / secs);
   }
   net->stat_raw = net->stat_wire = net->stat_msgs = 0;
   net->stat_last = now;
}

/* Emit an XOR delta for [begin, end) and bring the shadow up to date.
 * Deltas are mostly zeros where the app rewrote unchanged bytes, so they
 * get deflated when that saves at least 10%.
 */
static void
net_emit_xor(struct net *net, struct net_mem *mem, size_t begin, size_t end)
{
   const size_t len = end - begin;
   if (len > net->delta_cap) {
      uint8_t *d = realloc(net->delta, len);
      if (!d) {
         vn_log(net->instance, "net: out of memory for a %zu byte delta", len);
         net->lost = true;
         return;
      }
      net->delta = d;
      net->delta_cap = len;
   }

   uint8_t *delta = net->delta;
   const uint8_t *cur = mem->ptr + begin;
   uint8_t *shadow = mem->shadow + begin;

   size_t i = 0;
   /* read each word of cur exactly once, since the app may be writing it */
   for (; i + 8 <= len; i += 8) {
      uint64_t c, s;
      memcpy(&c, cur + i, 8);
      memcpy(&s, shadow + i, 8);
      const uint64_t d = c ^ s;
      memcpy(delta + i, &d, 8);
      memcpy(shadow + i, &c, 8);
   }
   for (; i < len; i++) {
      const uint8_t c = cur[i];
      delta[i] = c ^ shadow[i];
      shadow[i] = c;
   }

   /* masked words never travel as deltas, even if the receive thread
    * changed them while we were reading
    */
   for (uint32_t m = 0; m < mem->mask_count; m++) {
      const uint32_t off = mem->masks[m];
      if (!net_mask_in_range(off, begin, end))
         continue;
      for (uint32_t b = 0; b < sizeof(uint32_t); b++) {
         const size_t pos = off + b;
         if (pos >= begin && pos < end)
            delta[pos - begin] = 0;
      }
   }

   size_t wire = 0;
   if (net->compress && len >= 256) {
      const uLong bound = compressBound(len);
      const size_t mark = net->out.len;
      struct nm_mem_z *msg = net_append(net, NM_MEM_XOR_Z, sizeof(*msg) + bound);
      uLongf zlen = bound;
      if (msg && compress2((Bytef *)(msg + 1), &zlen, delta, len, 1) == Z_OK &&
          zlen < len - len / 10) {
         msg->res_id = mem->res_id;
         msg->raw_len = len;
         msg->offset = begin;
         /* shrink the message to what deflate produced */
         struct nm_hdr *hdr = (struct nm_hdr *)(net->out.data + mark);
         hdr->len = sizeof(*msg) + zlen;
         net->out.len = mark + sizeof(*hdr) + sizeof(*msg) + zlen;
         wire = sizeof(*msg) + zlen;
      } else {
         /* not worth it: drop the attempt and send it raw */
         net->out.len = mark;
      }
   }

   if (!wire) {
      struct nm_mem *msg = net_append(net, NM_MEM_XOR, sizeof(*msg) + len);
      if (!msg) {
         vn_log(net->instance, "net: out of memory for a %zu byte delta", len);
         net->lost = true;
         return;
      }
      msg->res_id = mem->res_id;
      msg->pad = 0;
      msg->offset = begin;
      memcpy(msg + 1, delta, len);
      wire = sizeof(*msg) + len;
   }

   if (net->stats) {
      net->stat_raw += len;
      net->stat_wire += wire;
      net->stat_msgs++;
      net_report_stats(net);
   }
}

/* diff the pages overlapping [begin, end) against the shadow */
static void
net_diff_range(struct net *net, struct net_mem *mem, size_t begin, size_t end)
{
   begin &= ~(size_t)(NET_PAGE_SIZE - 1);
   end = MIN2(end, mem->size);

   size_t off = begin;
   while (off < end) {
      size_t len = MIN2(NET_PAGE_SIZE, mem->size - off);
      if (!memcmp(mem->ptr + off, mem->shadow + off, len)) {
         off += len;
         continue;
      }

      const size_t run_begin = off;
      do {
         off += len;
         if (off >= end)
            break;
         len = MIN2(NET_PAGE_SIZE, mem->size - off);
      } while (memcmp(mem->ptr + off, mem->shadow + off, len));

      net_emit_xor(net, mem, run_begin, off);
   }
}

#ifdef PAGEMAP_SCAN
/* Diff only the pages written since the last scan, and write-protect them
 * again in the same step. Returns false if the scan failed.
 */
static bool
net_diff_tracked(struct net *net, struct net_mem *mem)
{
   struct page_region regions[64];
   const uintptr_t base = (uintptr_t)mem->ptr;
   struct pm_scan_arg arg = {
      .size = sizeof(arg),
      .flags = PM_SCAN_WP_MATCHING | PM_SCAN_CHECK_WPASYNC,
      .start = base,
      .end = base + mem->map_size,
      .vec = (uintptr_t)regions,
      .vec_len = ARRAY_SIZE(regions),
      .category_mask = PAGE_IS_WRITTEN,
      .return_mask = PAGE_IS_WRITTEN,
   };

   while (true) {
      const int n = ioctl(net->pagemap_fd, PAGEMAP_SCAN, &arg);
      if (n < 0)
         return false;
      for (int i = 0; i < n; i++)
         net_diff_range(net, mem, regions[i].start - base,
                        regions[i].end - base);
      if (arg.walk_end >= arg.end)
         return true;
      arg.start = arg.walk_end;
   }
}
#endif

static void
net_diff_mem(struct net *net, struct net_mem *mem)
{
   if (!mem->ptr)
      return;

   /* keep masked words from making a page look dirty */
   for (uint32_t m = 0; m < mem->mask_count; m++)
      memcpy(mem->shadow + mem->masks[m], mem->ptr + mem->masks[m],
             sizeof(uint32_t));

#ifdef PAGEMAP_SCAN
   if (mem->tracked) {
      if (net_diff_tracked(net, mem))
         return;
      vn_log(net->instance, "net: PAGEMAP_SCAN failed (%s), full diffs now",
             strerror(errno));
      mem->tracked = false;
   }
#endif

   net_diff_range(net, mem, 0, mem->size);
}

/* Snapshot every guest-written change. Caller holds send_mutex. */
static void
net_diff_all_locked(struct net *net)
{
   mtx_lock(&net->mem_mutex);
   list_for_each_entry (struct net_mem, mem, &net->mems, link)
      net_diff_mem(net, mem);
   mtx_unlock(&net->mem_mutex);
}

static void
net_mem_link(struct net *net, struct net_mem *mem)
{
   mtx_lock(&net->mem_mutex);
   struct net_slot *slot = util_sparse_array_get(&net->slots, mem->res_id);
   slot->mem = mem;
   list_addtail(&mem->link, &net->mems);
   mem->linked = true;
   mtx_unlock(&net->mem_mutex);
}

static void
net_mem_unlink(struct net *net, struct net_mem *mem)
{
   mtx_lock(&net->mem_mutex);
   struct net_slot *slot = util_sparse_array_get(&net->slots, mem->res_id);
   slot->mem = NULL;
   if (mem->linked) {
      list_del(&mem->link);
      mem->linked = false;
   }
   mtx_unlock(&net->mem_mutex);
}

/* caller holds mem_mutex */
static struct net_mem *
net_lookup_locked(struct net *net, uint32_t res_id, uint64_t offset,
                  uint64_t len)
{
   if (!res_id)
      return NULL;
   struct net_slot *slot = util_sparse_array_get(&net->slots, res_id);
   struct net_mem *mem = slot->mem;
   if (!mem || !mem->ptr || offset > mem->size || len > mem->size - offset)
      return NULL;
   return mem;
}

static void *
net_map_anon(void *addr, size_t size)
{
   void *ptr = mmap(addr, size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | (addr ? MAP_FIXED : 0), -1,
                    0);
   return ptr == MAP_FAILED ? NULL : ptr;
}

static void
net_mem_track(struct net *net, struct net_mem *mem)
{
   mem->tracked = false;
#ifdef PAGEMAP_SCAN
   if (net->uffd < 0)
      return;
   struct uffdio_register reg = {
      .range = { .start = (uintptr_t)mem->ptr, .len = mem->map_size },
      .mode = UFFDIO_REGISTER_MODE_WP,
   };
   if (ioctl(net->uffd, UFFDIO_REGISTER, &reg))
      return;
   struct uffdio_writeprotect wp = {
      .range = reg.range,
      .mode = UFFDIO_WRITEPROTECT_MODE_WP,
   };
   if (ioctl(net->uffd, UFFDIO_WRITEPROTECT, &wp)) {
      struct uffdio_range range = reg.range;
      ioctl(net->uffd, UFFDIO_UNREGISTER, &range);
      return;
   }
   mem->tracked = true;
#endif
}

static bool
net_mem_map(struct net *net, struct net_mem *mem, void *placed_addr)
{
   mem->map_size = align64(mem->size, NET_PAGE_SIZE);
   mem->ptr = net_map_anon(placed_addr, mem->map_size);
   if (!mem->ptr)
      return false;
   mem->shadow = net_map_anon(NULL, mem->map_size);
   if (!mem->shadow) {
      munmap(mem->ptr, mem->map_size);
      mem->ptr = NULL;
      return false;
   }
   net_mem_track(net, mem);
   return true;
}

static void
net_mem_unmap(struct net_mem *mem)
{
   /* munmap also drops the userfaultfd registration */
   if (mem->ptr)
      munmap(mem->ptr, mem->map_size);
   if (mem->shadow)
      munmap(mem->shadow, mem->map_size);
   mem->ptr = NULL;
   mem->shadow = NULL;
}

static uint32_t
net_alloc_res_id(struct net *net)
{
   return atomic_fetch_add(&net->next_res_id, 1);
}

static void
net_append_res_create(struct net *net,
                      uint32_t res_id,
                      uint32_t blob_flags,
                      uint64_t size,
                      uint64_t blob_id)
{
   struct nm_res_create *msg =
      net_append(net, NM_RES_CREATE, sizeof(*msg));
   if (!msg) {
      net->lost = true;
      return;
   }
   *msg = (struct nm_res_create){
      .res_id = res_id,
      .blob_flags = blob_flags,
      .size = size,
      .blob_id = blob_id,
   };
}

static void
net_append_res_destroy(struct net *net, uint32_t res_id)
{
   struct nm_res_destroy *msg =
      net_append(net, NM_RES_DESTROY, sizeof(*msg));
   if (msg)
      *msg = (struct nm_res_destroy){ .res_id = res_id };
}

/* ------------------------------------------------------------------ */
/* syncs                                                              */

static bool
net_sync_check_locked(const struct vn_renderer_wait *wait)
{
   for (uint32_t i = 0; i < wait->sync_count; i++) {
      const struct net_sync *sync = (const struct net_sync *)wait->syncs[i];
      const bool done = sync->value >= wait->sync_values[i];
      if (wait->wait_any && done)
         return true;
      if (!wait->wait_any && !done)
         return false;
   }
   return !wait->wait_any;
}

static VkResult
net_wait(struct vn_renderer *renderer, const struct vn_renderer_wait *wait)
{
   struct net *net = (struct net *)renderer;

   struct timespec deadline;
   const bool infinite = wait->timeout == UINT64_MAX ||
                         wait->timeout > (uint64_t)INT64_MAX / 2;
   if (!infinite) {
      timespec_get(&deadline, TIME_UTC);
      const uint64_t ns = deadline.tv_nsec + wait->timeout;
      deadline.tv_sec += ns / 1000000000ull;
      deadline.tv_nsec = ns % 1000000000ull;
   }

   VkResult result;
   mtx_lock(&net->sync_mutex);
   while (true) {
      if (net_sync_check_locked(wait)) {
         result = VK_SUCCESS;
         break;
      }
      if (net->lost) {
         result = VK_ERROR_DEVICE_LOST;
         break;
      }
      if (!wait->timeout) {
         result = VK_TIMEOUT;
         break;
      }
      if (infinite) {
         cnd_wait(&net->sync_cond, &net->sync_mutex);
      } else if (cnd_timedwait(&net->sync_cond, &net->sync_mutex,
                               &deadline) == thrd_timedout) {
         result = net_sync_check_locked(wait) ? VK_SUCCESS : VK_TIMEOUT;
         break;
      }
   }
   mtx_unlock(&net->sync_mutex);

   return result;
}

static VkResult
net_sync_create(struct vn_renderer *renderer,
                uint64_t initial_val,
                struct vn_renderer_sync **out_sync)
{
   struct net *net = (struct net *)renderer;
   struct net_sync *sync = calloc(1, sizeof(*sync));
   if (!sync)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   sync->base.sync_id = atomic_fetch_add(&net->next_sync_id, 1);
   sync->value = initial_val;
   *out_sync = &sync->base;
   return VK_SUCCESS;
}

static void
net_sync_destroy(struct vn_renderer *renderer, struct vn_renderer_sync *_sync)
{
   struct net *net = (struct net *)renderer;
   struct net_sync *sync = (struct net_sync *)_sync;

   mtx_lock(&net->sync_mutex);
   list_for_each_entry (struct net_pending, p, &net->pending, link) {
      for (uint32_t i = 0; i < p->count; i++) {
         if (p->items[i].sync == sync)
            p->items[i].sync = NULL;
      }
   }
   mtx_unlock(&net->sync_mutex);

   free(sync);
}

static VkResult
net_sync_write(struct vn_renderer *renderer,
               struct vn_renderer_sync *_sync,
               uint64_t val)
{
   struct net *net = (struct net *)renderer;
   struct net_sync *sync = (struct net_sync *)_sync;
   mtx_lock(&net->sync_mutex);
   sync->value = val;
   cnd_broadcast(&net->sync_cond);
   mtx_unlock(&net->sync_mutex);
   return VK_SUCCESS;
}

static VkResult
net_sync_read(struct vn_renderer *renderer,
              struct vn_renderer_sync *_sync,
              uint64_t *val)
{
   struct net *net = (struct net *)renderer;
   struct net_sync *sync = (struct net_sync *)_sync;
   mtx_lock(&net->sync_mutex);
   *val = sync->value;
   mtx_unlock(&net->sync_mutex);
   return VK_SUCCESS;
}

static VkResult
net_sync_reset(struct vn_renderer *renderer, struct vn_renderer_sync *sync)
{
   return net_sync_write(renderer, sync, 0);
}

static void
net_retire_fence(struct net *net, uint32_t ring_idx, uint64_t fence_id)
{
   mtx_lock(&net->sync_mutex);
   list_for_each_entry_safe (struct net_pending, p, &net->pending, link) {
      if (p->ring_idx != ring_idx || p->fence_id > fence_id)
         continue;
      for (uint32_t i = 0; i < p->count; i++) {
         if (p->items[i].sync)
            p->items[i].sync->value = p->items[i].value;
      }
      list_del(&p->link);
      free(p);
   }
   cnd_broadcast(&net->sync_cond);
   mtx_unlock(&net->sync_mutex);
}

/* ------------------------------------------------------------------ */
/* submit                                                             */

/* caller holds send_mutex */
static VkResult
net_append_submit_locked(struct net *net,
                         const struct vn_renderer_submit_batch *batch)
{
   if (!batch->cs_size && !batch->sync_count)
      return VK_SUCCESS;

   uint64_t fence_id = 0;
   if (batch->sync_count) {
      struct net_pending *p =
         malloc(sizeof(*p) + sizeof(p->items[0]) * batch->sync_count);
      if (!p)
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      p->ring_idx = batch->ring_idx;
      p->count = batch->sync_count;
      for (uint32_t i = 0; i < batch->sync_count; i++) {
         p->items[i].sync = (struct net_sync *)batch->syncs[i];
         p->items[i].value = batch->sync_values[i];
      }

      mtx_lock(&net->sync_mutex);
      fence_id = ++net->next_fence_id;
      p->fence_id = fence_id;
      list_addtail(&p->link, &net->pending);
      mtx_unlock(&net->sync_mutex);
   }

   struct nm_submit *msg =
      net_append(net, NM_SUBMIT, sizeof(*msg) + batch->cs_size);
   if (!msg)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   msg->ring_idx = batch->ring_idx;
   msg->cs_size = batch->cs_size;
   msg->fence_id = fence_id;
   if (batch->cs_size)
      memcpy(msg + 1, batch->cs_data, batch->cs_size);

   return VK_SUCCESS;
}

static VkResult
net_submit(struct vn_renderer *renderer,
           const struct vn_renderer_submit_batch *batch)
{
   struct net *net = (struct net *)renderer;

   mtx_lock(&net->send_mutex);
   net_diff_all_locked(net);
   VkResult result = net_append_submit_locked(net, batch);
   if (!net_send_out_locked(net) && result == VK_SUCCESS)
      result = VK_ERROR_DEVICE_LOST;
   mtx_unlock(&net->send_mutex);

   return result;
}

/* ------------------------------------------------------------------ */
/* ring hooks (added to vn_renderer_ops by the narnia-metal patches)  */

static struct net_mem *
net_shmem_mem(struct vn_renderer_shmem *shmem)
{
   return &((struct net_shmem *)shmem)->mem;
}

/* caller holds send_mutex; sets the host's copy of a word to ours */
static void
net_append_mem_set_word(struct net *net, struct net_mem *mem, uint32_t off)
{
   struct nm_mem *msg =
      net_append(net, NM_MEM_SET, sizeof(*msg) + sizeof(uint32_t));
   if (!msg)
      return;
   msg->res_id = mem->res_id;
   msg->pad = 0;
   msg->offset = off;
   memcpy(msg + 1, mem->ptr + off, sizeof(uint32_t));
   memcpy(mem->shadow + off, mem->ptr + off, sizeof(uint32_t));
}

static void
net_ring_create(struct vn_renderer *renderer,
                uint64_t ring_id,
                struct vn_renderer_shmem *shmem,
                uint32_t head_offset,
                uint32_t tail_offset,
                uint32_t status_offset)
{
   struct net *net = (struct net *)renderer;
   struct net_mem *mem = net_shmem_mem(shmem);

   mtx_lock(&net->send_mutex);
   mtx_lock(&net->mem_mutex);
   mem->mask_count = 3;
   mem->masks[0] = head_offset;
   mem->masks[1] = tail_offset;
   mem->masks[2] = status_offset;
   /* vkr requires head and status to start at 0, and Mesa just zeroed them */
   for (uint32_t i = 0; i < mem->mask_count; i++)
      net_append_mem_set_word(net, mem, mem->masks[i]);
   mtx_unlock(&net->mem_mutex);
   net_send_out_locked(net);
   mtx_unlock(&net->send_mutex);
}

static void
net_ring_destroy(struct vn_renderer *renderer,
                 uint64_t ring_id,
                 struct vn_renderer_shmem *shmem)
{
   struct net *net = (struct net *)renderer;
   struct net_mem *mem = net_shmem_mem(shmem);

   /* The shmem goes back to the cache and may be reused for anything. Make
    * the host's copy of the control words equal ours so later XOR deltas
    * stay correct.
    */
   mtx_lock(&net->send_mutex);
   mtx_lock(&net->mem_mutex);
   for (uint32_t i = 0; i < mem->mask_count; i++)
      net_append_mem_set_word(net, mem, mem->masks[i]);
   mem->mask_count = 0;
   mtx_unlock(&net->mem_mutex);
   net_send_out_locked(net);
   mtx_unlock(&net->send_mutex);
}

static void
net_ring_tail(struct vn_renderer *renderer,
              uint64_t ring_id,
              struct vn_renderer_shmem *shmem,
              uint32_t tail_offset,
              uint32_t tail)
{
   struct net *net = (struct net *)renderer;
   struct net_mem *mem = net_shmem_mem(shmem);

   mtx_lock(&net->send_mutex);
   net_diff_all_locked(net);
   struct nm_ring_word *msg = net_append(net, NM_RING_TAIL, sizeof(*msg));
   if (msg) {
      *msg = (struct nm_ring_word){
         .res_id = mem->res_id,
         .offset = tail_offset,
         .value = tail,
         .ring_id = ring_id,
      };
   }
   net_send_out_locked(net);
   mtx_unlock(&net->send_mutex);
}

/* ------------------------------------------------------------------ */
/* shmem                                                              */

static void
net_shmem_destroy_now(struct vn_renderer *renderer,
                      struct vn_renderer_shmem *_shmem)
{
   struct net *net = (struct net *)renderer;
   struct net_shmem *shmem = (struct net_shmem *)_shmem;

   mtx_lock(&net->send_mutex);
   net_mem_unlink(net, &shmem->mem);
   net_append_res_destroy(net, shmem->mem.res_id);
   net_send_out_locked(net);
   mtx_unlock(&net->send_mutex);

   net_mem_unmap(&shmem->mem);
   free(shmem);
}

static void
net_shmem_destroy(struct vn_renderer *renderer,
                  struct vn_renderer_shmem *shmem)
{
   struct net *net = (struct net *)renderer;
   if (vn_renderer_shmem_cache_add(&net->shmem_cache, shmem))
      return;
   net_shmem_destroy_now(&net->base, shmem);
}

static struct vn_renderer_shmem *
net_shmem_create(struct vn_renderer *renderer, size_t size)
{
   struct net *net = (struct net *)renderer;

   struct vn_renderer_shmem *cached =
      vn_renderer_shmem_cache_get(&net->shmem_cache, size);
   if (cached) {
      cached->refcount = VN_REFCOUNT_INIT(1);
      return cached;
   }

   struct net_shmem *shmem = calloc(1, sizeof(*shmem));
   if (!shmem)
      return NULL;

   shmem->mem.res_id = net_alloc_res_id(net);
   shmem->mem.size = size;
   if (!net_mem_map(net, &shmem->mem, NULL)) {
      free(shmem);
      return NULL;
   }

   shmem->base = (struct vn_renderer_shmem){
      .refcount = VN_REFCOUNT_INIT(1),
      .res_id = shmem->mem.res_id,
      .mmap_size = size,
      .mmap_ptr = shmem->mem.ptr,
   };

   mtx_lock(&net->send_mutex);
   net_append_res_create(net, shmem->mem.res_id, NET_BLOB_FLAG_MAPPABLE, size,
                         0);
   net_mem_link(net, &shmem->mem);
   net_send_out_locked(net);
   mtx_unlock(&net->send_mutex);

   return &shmem->base;
}

/* ------------------------------------------------------------------ */
/* bo                                                                 */

static void
net_bo_invalidate(struct vn_renderer *renderer,
                  struct vn_renderer_bo *bo,
                  VkDeviceSize offset,
                  VkDeviceSize size)
{
   /* GPU writes to host-visible memory are not mirrored back (v1) */
}

static void
net_bo_flush(struct vn_renderer *renderer,
             struct vn_renderer_bo *bo,
             VkDeviceSize offset,
             VkDeviceSize size)
{
   /* the next release point picks the writes up */
}

static void *
net_bo_map(struct vn_renderer *renderer,
           struct vn_renderer_bo *_bo,
           void *placed_addr)
{
   struct net *net = (struct net *)renderer;
   struct net_bo *bo = (struct net_bo *)_bo;

   if (!(bo->blob_flags & NET_BLOB_FLAG_MAPPABLE))
      return NULL;

   if (!bo->base.mmap_ptr) {
      if (!net_mem_map(net, &bo->mem, placed_addr)) {
         vn_log(net->instance, "net: failed to map bo of size %zu",
                bo->mem.size);
         return NULL;
      }
      bo->base.mmap_ptr = bo->mem.ptr;
      net_mem_link(net, &bo->mem);
   }

   return bo->base.mmap_ptr;
}

static int
net_bo_export_dma_buf(struct vn_renderer *renderer,
                      struct vn_renderer_bo *bo)
{
   return -1;
}

static bool
net_bo_destroy(struct vn_renderer *renderer, struct vn_renderer_bo *_bo)
{
   struct net *net = (struct net *)renderer;
   struct net_bo *bo = (struct net_bo *)_bo;

   mtx_lock(&net->send_mutex);
   net_mem_unlink(net, &bo->mem);
   net_append_res_destroy(net, bo->mem.res_id);
   net_send_out_locked(net);
   mtx_unlock(&net->send_mutex);

   net_mem_unmap(&bo->mem);
   free(bo);
   return true;
}

static uint32_t
net_bo_blob_flags(VkMemoryPropertyFlags flags,
                  VkExternalMemoryHandleTypeFlags external_handles)
{
   uint32_t blob_flags = 0;
   if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
      blob_flags |= NET_BLOB_FLAG_MAPPABLE;
   if (external_handles)
      blob_flags |= NET_BLOB_FLAG_SHAREABLE;
   if (external_handles & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT)
      blob_flags |= NET_BLOB_FLAG_CROSS_DEVICE;
   return blob_flags;
}

static VkResult
net_bo_create_from_device_memory(
   struct vn_renderer *renderer,
   struct vn_renderer_submit_batch *batch,
   VkDeviceSize size,
   vn_object_id mem_id,
   VkMemoryPropertyFlags flags,
   VkExternalMemoryHandleTypeFlags external_handles,
   struct vn_renderer_bo **out_bo)
{
   struct net *net = (struct net *)renderer;

   struct net_bo *bo = calloc(1, sizeof(*bo));
   if (!bo)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   bo->blob_flags = net_bo_blob_flags(flags, external_handles);
   bo->mem.res_id = net_alloc_res_id(net);
   bo->mem.size = size;
   bo->base = (struct vn_renderer_bo){
      .refcount = VN_REFCOUNT_INIT(1),
      .res_id = bo->mem.res_id,
      .mmap_size = size,
   };

   mtx_lock(&net->send_mutex);
   VkResult result = VK_SUCCESS;
   if (batch) {
      net_diff_all_locked(net);
      result = net_append_submit_locked(net, batch);
   }
   if (result == VK_SUCCESS)
      net_append_res_create(net, bo->mem.res_id, bo->blob_flags, size,
                            mem_id);
   if (!net_send_out_locked(net) && result == VK_SUCCESS)
      result = VK_ERROR_DEVICE_LOST;
   mtx_unlock(&net->send_mutex);

   if (result != VK_SUCCESS) {
      free(bo);
      return result;
   }

   *out_bo = &bo->base;
   return VK_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* receive thread                                                     */

static void
net_apply_mem_write(struct net *net,
                    uint32_t res_id,
                    uint64_t offset,
                    const uint8_t *data,
                    uint32_t size)
{
   mtx_lock(&net->mem_mutex);
   struct net_mem *mem = net_lookup_locked(net, res_id, offset, size);
   if (mem) {
      memcpy(mem->ptr + offset, data, size);
      memcpy(mem->shadow + offset, data, size);
   }
   mtx_unlock(&net->mem_mutex);
}

static void
net_recv_mem_write(struct net *net, const uint8_t *payload, uint32_t len)
{
   struct nm_mem msg;
   if (len < sizeof(msg))
      return;
   memcpy(&msg, payload, sizeof(msg));
   net_apply_mem_write(net, msg.res_id, msg.offset, payload + sizeof(msg),
                       len - sizeof(msg));
}

/* NM_MEM_WRITE_Z: the host deflated a large reply */
static void
net_recv_mem_write_z(struct net *net,
                     const uint8_t *payload,
                     uint32_t len,
                     uint8_t **inflate_buf,
                     size_t *inflate_cap)
{
   struct nm_mem_z msg;
   if (len < sizeof(msg))
      return;
   memcpy(&msg, payload, sizeof(msg));
   if (msg.raw_len > *inflate_cap) {
      uint8_t *b = realloc(*inflate_buf, msg.raw_len);
      if (!b)
         return;
      *inflate_buf = b;
      *inflate_cap = msg.raw_len;
   }
   uLongf out_len = msg.raw_len;
   if (uncompress(*inflate_buf, &out_len, payload + sizeof(msg),
                  len - sizeof(msg)) != Z_OK ||
       out_len != msg.raw_len) {
      vn_log(net->instance, "net: bad compressed memory write");
      return;
   }
   net_apply_mem_write(net, msg.res_id, msg.offset, *inflate_buf, msg.raw_len);
}

static void
net_recv_ring_word(struct net *net, uint32_t type, const uint8_t *payload,
                   uint32_t len)
{
   struct nm_ring_word msg;
   if (len < sizeof(msg))
      return;
   memcpy(&msg, payload, sizeof(msg));

   mtx_lock(&net->mem_mutex);
   struct net_mem *mem =
      net_lookup_locked(net, msg.res_id, msg.offset, sizeof(uint32_t));
   if (mem && !(msg.offset & 3)) {
      atomic_uint *word = (atomic_uint *)(mem->ptr + msg.offset);
      if (type == NM_RING_HEAD) {
         atomic_store_explicit(word, msg.value, memory_order_release);
      } else {
         /* Take IDLE/FATAL from the host. ALIVE is only ever set from here;
          * the guest's watchdog clears it locally.
          */
         const uint32_t alive = VK_RING_STATUS_ALIVE_BIT_MESA;
         uint32_t old = atomic_load_explicit(word, memory_order_relaxed);
         uint32_t val;
         do {
            val = (msg.value & ~alive) | (old & alive) | (msg.value & alive);
         } while (!atomic_compare_exchange_weak_explicit(
            word, &old, val, memory_order_seq_cst, memory_order_relaxed));
      }
   }
   mtx_unlock(&net->mem_mutex);
}

static int
net_recv_thread_func(void *arg)
{
   struct net *net = arg;
   u_thread_setname("vn-net-recv");

   uint8_t *payload = NULL;
   size_t payload_cap = 0;
   uint8_t *inflate_buf = NULL;
   size_t inflate_cap = 0;

   while (true) {
      struct nm_hdr hdr;
      if (!net_read_all(net, &hdr, sizeof(hdr)))
         break;
      if (hdr.len > payload_cap) {
         size_t cap = MAX2(hdr.len, 4096);
         uint8_t *p = realloc(payload, cap);
         if (!p)
            break;
         payload = p;
         payload_cap = cap;
      }
      if (hdr.len && !net_read_all(net, payload, hdr.len))
         break;

      switch (hdr.type) {
      case NM_MEM_WRITE:
         net_recv_mem_write(net, payload, hdr.len);
         break;
      case NM_MEM_WRITE_Z:
         net_recv_mem_write_z(net, payload, hdr.len, &inflate_buf,
                              &inflate_cap);
         break;
      case NM_RING_HEAD:
      case NM_RING_STATUS:
         net_recv_ring_word(net, hdr.type, payload, hdr.len);
         break;
      case NM_FENCE_RETIRED: {
         struct nm_fence msg;
         if (hdr.len >= sizeof(msg)) {
            memcpy(&msg, payload, sizeof(msg));
            net_retire_fence(net, msg.ring_idx, msg.fence_id);
         }
         break;
      }
      case NM_PRESENT_DONE: {
         struct nm_present_done msg;
         if (hdr.len >= sizeof(msg)) {
            memcpy(&msg, payload, sizeof(msg));
            mtx_lock(&net->swapchain_mutex);
            list_for_each_entry (struct net_swapchain, sc, &net->swapchains,
                                 link) {
               if (sc->id == msg.swapchain_id) {
                  sc->done(sc->data, msg.image_index);
                  break;
               }
            }
            mtx_unlock(&net->swapchain_mutex);
         }
         break;
      }
      case NM_ERROR: {
         struct nm_error msg = { 0 };
         if (hdr.len >= sizeof(msg))
            memcpy(&msg, payload, sizeof(msg));
         const char *text = hdr.len > sizeof(msg)
                               ? (const char *)payload + sizeof(msg)
                               : "";
         vn_log(net->instance, "net: host error %u: %.*s", msg.code,
                (int)(hdr.len - MIN2(hdr.len, sizeof(msg))), text);
         break;
      }
      default:
         vn_log(net->instance, "net: unknown message %u", hdr.type);
         break;
      }
   }

   free(payload);
   free(inflate_buf);

   if (!net->lost)
      vn_log(net->instance, "net: lost connection to the host");
   net->lost = true;
   mtx_lock(&net->sync_mutex);
   cnd_broadcast(&net->sync_cond);
   mtx_unlock(&net->sync_mutex);
   return 0;
}

/* ------------------------------------------------------------------ */
/* remote WSI                                                         */

uint32_t
vn_renderer_net_register_swapchain(struct vn_renderer *renderer,
                                   vn_renderer_net_present_done_func done,
                                   void *data)
{
   struct net *net = (struct net *)renderer;
   struct net_swapchain *sc = calloc(1, sizeof(*sc));
   if (!sc)
      return 0;
   sc->done = done;
   sc->data = data;
   mtx_lock(&net->swapchain_mutex);
   sc->id = net->next_swapchain_id++;
   list_addtail(&sc->link, &net->swapchains);
   mtx_unlock(&net->swapchain_mutex);
   return sc->id;
}

void
vn_renderer_net_unregister_swapchain(struct vn_renderer *renderer,
                                     uint32_t swapchain_id)
{
   struct net *net = (struct net *)renderer;

   mtx_lock(&net->swapchain_mutex);
   list_for_each_entry (struct net_swapchain, sc, &net->swapchains, link) {
      if (sc->id == swapchain_id) {
         list_del(&sc->link);
         free(sc);
         break;
      }
   }
   mtx_unlock(&net->swapchain_mutex);

   mtx_lock(&net->send_mutex);
   struct nm_swapchain_destroy *msg =
      net_append(net, NM_SWAPCHAIN_DESTROY, sizeof(*msg));
   if (msg)
      *msg = (struct nm_swapchain_destroy){ .swapchain_id = swapchain_id };
   net_send_out_locked(net);
   mtx_unlock(&net->send_mutex);
}

VkResult
vn_renderer_net_present(struct vn_renderer *renderer,
                        const struct nm_present *present)
{
   struct net *net = (struct net *)renderer;

   mtx_lock(&net->send_mutex);
   /* the app may have written memory the copy depends on (unlikely) */
   net_diff_all_locked(net);
   struct nm_present *msg = net_append(net, NM_PRESENT, sizeof(*msg));
   if (msg)
      *msg = *present;
   const bool ok = net_send_out_locked(net);
   mtx_unlock(&net->send_mutex);

   return ok ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;
}

bool
vn_renderer_net_is_lost(struct vn_renderer *renderer)
{
   return ((struct net *)renderer)->lost;
}

/* ------------------------------------------------------------------ */
/* init                                                               */

static int
net_connect(struct net *net, const char *addr)
{
   if (!strncmp(addr, "unix:", 5)) {
      const char *path = addr + 5;
      struct sockaddr_un un = { .sun_family = AF_UNIX };
      if (strlen(path) >= sizeof(un.sun_path))
         return -1;
      strcpy(un.sun_path, path);
      int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      if (fd < 0)
         return -1;
      if (connect(fd, (struct sockaddr *)&un, sizeof(un))) {
         vn_log(net->instance, "net: connect %s: %s", addr, strerror(errno));
         close(fd);
         return -1;
      }
      return fd;
   }

   if (!strncmp(addr, "tcp:", 4))
      addr += 4;

   char host[256];
   snprintf(host, sizeof(host), "%s", addr);
   char port[16];
   snprintf(port, sizeof(port), "%d", NM_DEFAULT_PORT);
   char *colon = strrchr(host, ':');
   if (colon && !strchr(colon, ']')) {
      snprintf(port, sizeof(port), "%s", colon + 1);
      *colon = '\0';
   }
   char *h = host;
   if (h[0] == '[') {
      h++;
      char *end = strchr(h, ']');
      if (end)
         *end = '\0';
   }

   const struct addrinfo hints = {
      .ai_family = AF_UNSPEC,
      .ai_socktype = SOCK_STREAM,
   };
   struct addrinfo *res;
   int err = getaddrinfo(h, port, &hints, &res);
   if (err) {
      vn_log(net->instance, "net: resolve %s: %s", addr, gai_strerror(err));
      return -1;
   }

   int fd = -1;
   for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
      fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC,
                  ai->ai_protocol);
      if (fd < 0)
         continue;
      if (!connect(fd, ai->ai_addr, ai->ai_addrlen))
         break;
      close(fd);
      fd = -1;
   }
   freeaddrinfo(res);

   if (fd < 0) {
      vn_log(net->instance, "net: connect %s: %s", addr, strerror(errno));
      return -1;
   }

   const int one = 1;
   setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
   return fd;
}

static VkResult
net_handshake(struct net *net)
{
   const char *name = util_get_process_name();
   if (!name)
      name = "venus";
   const size_t name_len = strlen(name) + 1;

   struct nm_hello *hello =
      net_append(net, NM_HELLO, sizeof(*hello) + name_len);
   if (!hello)
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   hello->magic = NM_MAGIC;
   hello->version = NM_VERSION;
   memcpy(hello + 1, name, name_len);
   if (!net_send_out_locked(net))
      return VK_ERROR_INITIALIZATION_FAILED;

   struct nm_hdr hdr;
   struct nm_hello_reply reply;
   if (!net_read_all(net, &hdr, sizeof(hdr)) ||
       hdr.type != NM_HELLO_REPLY || hdr.len < sizeof(reply) ||
       !net_read_all(net, &reply, sizeof(reply))) {
      vn_log(net->instance, "net: bad hello reply");
      return VK_ERROR_INITIALIZATION_FAILED;
   }
   if (reply.magic != NM_MAGIC || reply.version != NM_VERSION ||
       hdr.len != sizeof(reply) + reply.capset_size) {
      vn_log(net->instance, "net: host speaks version %u, we speak %u",
             reply.version, NM_VERSION);
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   /* copy what fits, skip the rest */
   uint32_t left = reply.capset_size;
   const uint32_t take = MIN2(left, sizeof(net->capset));
   if (!net_read_all(net, &net->capset, take))
      return VK_ERROR_INITIALIZATION_FAILED;
   left -= take;
   while (left) {
      char tmp[256];
      const uint32_t n = MIN2(left, sizeof(tmp));
      if (!net_read_all(net, tmp, n))
         return VK_ERROR_INITIALIZATION_FAILED;
      left -= n;
   }

   net->max_timeline_count = reply.max_timeline_count;
   if (!net->max_timeline_count) {
      vn_log(net->instance, "net: host has no timeline support");
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   return VK_SUCCESS;
}

static void
net_init_dirty_tracking(struct net *net)
{
   net->uffd = -1;
   net->pagemap_fd = -1;
#ifdef PAGEMAP_SCAN
   const char *env = os_get_option("VN_NET_DIRTY_TRACKING");
   if (env && !strcmp(env, "0"))
      return;

   /* user-mode-only works without vm.unprivileged_userfaultfd */
   int uffd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK |
                                          UFFD_USER_MODE_ONLY);
   if (uffd < 0)
      uffd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK);
   if (uffd < 0)
      goto fail;

   struct uffdio_api api = {
      .api = UFFD_API,
      .features = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_UNPOPULATED,
   };
   if (ioctl(uffd, UFFDIO_API, &api)) {
      close(uffd);
      goto fail;
   }

   int pagemap_fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
   if (pagemap_fd < 0) {
      close(uffd);
      goto fail;
   }

   net->uffd = uffd;
   net->pagemap_fd = pagemap_fd;
   if (VN_DEBUG(INIT))
      vn_log(net->instance, "net: dirty page tracking on");
   return;

fail:
   if (VN_DEBUG(INIT))
      vn_log(net->instance, "net: no dirty page tracking (%s), diffing all "
             "pages", strerror(errno));
#endif
}

static void
net_init_renderer_info(struct net *net)
{
   struct vn_renderer_info *info = &net->base.info;

   info->pci.vendor_id = NET_PCI_VENDOR_ID;
   info->pci.device_id = NET_PCI_DEVICE_ID;

   info->has_dma_buf_import = false;
   info->has_external_sync = false;
   info->has_timeline_sync = !VN_PERF(NO_TIMELINE_SYNC);
   info->has_implicit_fencing = false;

   const struct virgl_renderer_capset_venus *capset = &net->capset;
   info->wire_format_version = capset->wire_format_version;
   info->vk_xml_version = capset->vk_xml_version;
   info->vk_ext_command_serialization_spec_version =
      capset->vk_ext_command_serialization_spec_version;
   info->vk_mesa_venus_protocol_spec_version =
      capset->vk_mesa_venus_protocol_spec_version;

   STATIC_ASSERT(sizeof(info->vk_extension_mask) >=
                 sizeof(capset->vk_extension_mask1));
   memcpy(info->vk_extension_mask, capset->vk_extension_mask1,
          sizeof(capset->vk_extension_mask1));

   info->max_timeline_count = net->max_timeline_count;

   /* swapchains are implemented by vn_net_wsi.c and shown on the host */
   info->remote_wsi = true;
}

static void
net_destroy(struct vn_renderer *renderer, const VkAllocationCallbacks *alloc)
{
   struct net *net = (struct net *)renderer;

   if (net->shmem_cache.initialized)
      vn_renderer_shmem_cache_fini(&net->shmem_cache);

   /* the receive thread sees EOF next; this is not a lost connection */
   net->lost = true;
   if (net->fd >= 0)
      shutdown(net->fd, SHUT_RDWR);
   if (net->recv_started)
      thrd_join(net->recv_thread, NULL);
   if (net->fd >= 0)
      close(net->fd);

   list_for_each_entry_safe (struct net_pending, p, &net->pending, link)
      free(p);

   if (net->uffd >= 0)
      close(net->uffd);
   if (net->pagemap_fd >= 0)
      close(net->pagemap_fd);

   free(net->out.data);
   free(net->delta);
   util_sparse_array_finish(&net->slots);
   mtx_destroy(&net->send_mutex);
   mtx_destroy(&net->mem_mutex);
   mtx_destroy(&net->sync_mutex);
   cnd_destroy(&net->sync_cond);
   list_for_each_entry_safe (struct net_swapchain, sc, &net->swapchains, link)
      free(sc);
   mtx_destroy(&net->swapchain_mutex);

   vk_free(alloc, net);
}

static VkResult
net_init(struct net *net, const char *addr)
{
   util_sparse_array_init(&net->slots, sizeof(struct net_slot), 1024);
   mtx_init(&net->send_mutex, mtx_plain);
   mtx_init(&net->mem_mutex, mtx_plain);
   mtx_init(&net->sync_mutex, mtx_plain);
   cnd_init(&net->sync_cond);
   mtx_init(&net->swapchain_mutex, mtx_plain);
   list_inithead(&net->swapchains);
   net->next_swapchain_id = 1;
   list_inithead(&net->mems);
   list_inithead(&net->pending);
   net->next_res_id = 1;
   net->next_sync_id = 1;

   net_init_dirty_tracking(net);

   net->fd = net_connect(net, addr);
   if (net->fd < 0)
      return VK_ERROR_INITIALIZATION_FAILED;

   VkResult result = net_handshake(net);
   if (result != VK_SUCCESS)
      return result;

   const char *compress = os_get_option("VN_NET_COMPRESS");
   net->compress = !compress || strcmp(compress, "0");
   const char *stats = os_get_option("VN_NET_STATS");
   net->stats = stats && strcmp(stats, "0");

   if (thrd_create(&net->recv_thread, net_recv_thread_func, net) !=
       thrd_success)
      return VK_ERROR_INITIALIZATION_FAILED;
   net->recv_started = true;

   /* Feedback buffers are written by the GPU and polled by the CPU. We don't
    * mirror GPU writes back, so make Venus ask the renderer instead.
    */
   vn_env.perf |= VN_PERF_NO_EVENT_FEEDBACK | VN_PERF_NO_SEMAPHORE_FEEDBACK |
                  VN_PERF_NO_QUERY_FEEDBACK;

   vn_renderer_shmem_cache_init(&net->shmem_cache, &net->base,
                                net_shmem_destroy_now);

   net_init_renderer_info(net);

   net->base.ops.destroy = net_destroy;
   net->base.ops.submit = net_submit;
   net->base.ops.wait = net_wait;
   net->base.ops.ring_create = net_ring_create;
   net->base.ops.ring_destroy = net_ring_destroy;
   net->base.ops.ring_tail = net_ring_tail;

   net->base.shmem_ops.create = net_shmem_create;
   net->base.shmem_ops.destroy = net_shmem_destroy;

   net->base.bo_ops.create_from_device_memory =
      net_bo_create_from_device_memory;
   net->base.bo_ops.create_from_dma_buf = NULL;
   net->base.bo_ops.destroy = net_bo_destroy;
   net->base.bo_ops.export_dma_buf = net_bo_export_dma_buf;
   net->base.bo_ops.export_sync_file =
      vn_renderer_bo_export_sync_file_internal;
   net->base.bo_ops.map = net_bo_map;
   net->base.bo_ops.flush = net_bo_flush;
   net->base.bo_ops.invalidate = net_bo_invalidate;

   net->base.sync_ops.create = net_sync_create;
   net->base.sync_ops.create_from_syncobj = NULL;
   net->base.sync_ops.destroy = net_sync_destroy;
   net->base.sync_ops.export_syncobj = NULL;
   net->base.sync_ops.reset = net_sync_reset;
   net->base.sync_ops.read = net_sync_read;
   net->base.sync_ops.write = net_sync_write;

   return VK_SUCCESS;
}

VkResult
vn_renderer_create_net(struct vn_instance *instance,
                       const VkAllocationCallbacks *alloc,
                       struct vn_renderer **renderer)
{
   const char *addr = os_get_option("VN_NET");
   if (!addr || !*addr)
      return VK_ERROR_INITIALIZATION_FAILED;

   struct net *net = vk_zalloc(alloc, sizeof(*net), VN_DEFAULT_ALIGN,
                               VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!net)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   net->instance = instance;
   net->fd = -1;
   net->uffd = -1;
   net->pagemap_fd = -1;

   VkResult result = net_init(net, addr);
   if (result != VK_SUCCESS) {
      net_destroy(&net->base, alloc);
      return result;
   }

   *renderer = &net->base;
   if (VN_DEBUG(INIT))
      vn_log(instance, "net backend connected to %s", addr);
   return VK_SUCCESS;
}

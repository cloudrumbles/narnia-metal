/*
 * narnia-metal host core. See nm_host.h and docs/design.md.
 *
 * SPDX-License-Identifier: MIT
 */
#include "nm_host.h"

#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

#include "nm_audio.h"
#include "nm_present.h"
#include "nm_proto.h"
#include "vkr_renderer.h"

/* VIRTGPU_DRM_CAPSET_VENUS */
#define NM_CAPSET_VENUS 4
/* vkr_context sync_queues[] size */
#define NM_MAX_TIMELINE_COUNT 64
/* VK_COMMAND_TYPE_vkNotifyRingMESA_EXT */
#define NM_CMD_NOTIFY_RING 190
/* refuse single messages above this */
#define NM_MAX_MSG (512u * 1024 * 1024)

struct nm_res {
   uint8_t *data;
   uint64_t size;
};

struct nm_conn {
   const struct nm_transport *transport;
   uint32_t ctx_id;
   bool dead;

   pthread_mutex_t send_mutex;

   /* res_id -> mapping; guest allocates ids from 1 upwards */
   pthread_rwlock_t res_lock;
   struct nm_res *res;
   uint32_t res_cap;

   struct nm_present_state present;
   bool present_ready;

   /* inflated NM_MEM_XOR_Z payloads */
   uint8_t *inflate_buf;
   size_t inflate_cap;
};

static nm_log_func nm_log_cb;
static PFN_vkGetInstanceProcAddr nm_gipa;

/* ctx_id -> conn, read-locked by the vkr hooks while they send */
static pthread_rwlock_t nm_conns_lock = PTHREAD_RWLOCK_INITIALIZER;
static struct nm_conn *nm_conns[256];
static uint32_t nm_next_ctx_id = 1;

static void
nm_log(const char *fmt, ...)
{
   char buf[1024];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(buf, sizeof(buf), fmt, ap);
   va_end(ap);
   if (nm_log_cb)
      nm_log_cb(buf);
   else
      fprintf(stderr, "nm-host: %s\n", buf);
}

/* ------------------------------------------------------------------ */
/* sending                                                            */

/* send header + fixed part + optional tail bytes as one message.
 *
 * The three parts go out in one write when they are small. With TCP_NODELAY
 * every write is its own segment, TCP's initial window counts segments, and a
 * burst of replies (header, fixed part and payload each separate) used up the
 * window after a few messages, which cost an extra round trip on a long link.
 */
static bool
nm_send(struct nm_conn *conn, uint32_t type, const void *fixed,
        uint32_t fixed_size, const void *extra, uint32_t extra_size)
{
   const struct nm_hdr hdr = { .type = type, .len = fixed_size + extra_size };
   uint8_t small[1024];
   const size_t total = sizeof(hdr) + fixed_size + extra_size;
   uint8_t *buf = NULL;
   if (total <= sizeof(small))
      buf = small;
   else if (total <= 1u << 20)
      buf = malloc(total);
   if (buf) {
      memcpy(buf, &hdr, sizeof(hdr));
      if (fixed_size)
         memcpy(buf + sizeof(hdr), fixed, fixed_size);
      if (extra_size)
         memcpy(buf + sizeof(hdr) + fixed_size, extra, extra_size);
   }

   pthread_mutex_lock(&conn->send_mutex);
   bool ok = !conn->dead;
   if (ok && buf) {
      ok = conn->transport->write(conn->transport->user, buf, total);
   } else if (ok) {
      ok = conn->transport->write(conn->transport->user, &hdr, sizeof(hdr));
      if (ok && fixed_size)
         ok = conn->transport->write(conn->transport->user, fixed, fixed_size);
      if (ok && extra_size)
         ok = conn->transport->write(conn->transport->user, extra, extra_size);
   }
   if (!ok)
      conn->dead = true;
   pthread_mutex_unlock(&conn->send_mutex);
   if (buf && buf != small)
      free(buf);
   return ok;
}

static void
nm_send_error(struct nm_conn *conn, uint32_t code, const char *fmt, ...)
{
   char text[512];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(text, sizeof(text), fmt, ap);
   va_end(ap);
   nm_log("ctx %u: error: %s", conn->ctx_id, text);
   const struct nm_error err = { .code = code };
   nm_send(conn, NM_ERROR, &err, sizeof(err), text, strlen(text) + 1);
}

/* ------------------------------------------------------------------ */
/* resource table                                                     */

static bool
nm_res_get(struct nm_conn *conn, uint32_t res_id, uint64_t offset,
           uint64_t size, uint8_t **out)
{
   bool ok = false;
   pthread_rwlock_rdlock(&conn->res_lock);
   if (res_id < conn->res_cap) {
      const struct nm_res *res = &conn->res[res_id];
      if (res->data && offset <= res->size && size <= res->size - offset) {
         *out = res->data + offset;
         ok = true;
      }
   }
   pthread_rwlock_unlock(&conn->res_lock);
   return ok;
}

static bool
nm_res_set(struct nm_conn *conn, uint32_t res_id, void *data, uint64_t size)
{
   pthread_rwlock_wrlock(&conn->res_lock);
   if (res_id >= conn->res_cap) {
      uint32_t cap = conn->res_cap ? conn->res_cap : 1024;
      while (cap <= res_id)
         cap *= 2;
      struct nm_res *res = realloc(conn->res, sizeof(*res) * cap);
      if (!res) {
         pthread_rwlock_unlock(&conn->res_lock);
         return false;
      }
      memset(res + conn->res_cap, 0, sizeof(*res) * (cap - conn->res_cap));
      conn->res = res;
      conn->res_cap = cap;
   }
   conn->res[res_id] = (struct nm_res){ .data = data, .size = size };
   pthread_rwlock_unlock(&conn->res_lock);
   return true;
}

/* ------------------------------------------------------------------ */
/* vkr callbacks and hooks (vkr threads)                              */

/* returns the conn with nm_conns_lock read-held, or NULL (lock released) */
static struct nm_conn *
nm_conn_get(uint32_t ctx_id)
{
   pthread_rwlock_rdlock(&nm_conns_lock);
   struct nm_conn *conn =
      ctx_id < sizeof(nm_conns) / sizeof(nm_conns[0]) ? nm_conns[ctx_id] : NULL;
   if (!conn)
      pthread_rwlock_unlock(&nm_conns_lock);
   return conn;
}

static void
nm_conn_put(void)
{
   pthread_rwlock_unlock(&nm_conns_lock);
}

static void
nm_hook_mem_written(uint32_t ctx_id, uint32_t res_id, uint64_t offset,
                    uint64_t size)
{
   struct nm_conn *conn = nm_conn_get(ctx_id);
   if (!conn)
      return;
   uint8_t *data;
   if (nm_res_get(conn, res_id, offset, size, &data)) {
      /* Replies the guest is blocked on (extension lists, format tables)
       * are big and compress well. On a long link the time to receive them
       * is dominated by TCP slow start, so smaller is faster.
       */
      if (size >= 1024 && size <= UINT32_MAX) {
         uLongf bound = compressBound(size);
         uint8_t *z = malloc(bound);
         if (z && compress2(z, &bound, data, size, 1) == Z_OK &&
             bound < size - size / 8) {
            const struct nm_mem_z msg = {
               .res_id = res_id, .raw_len = (uint32_t)size, .offset = offset
            };
            nm_send(conn, NM_MEM_WRITE_Z, &msg, sizeof(msg), z, bound);
            free(z);
            nm_conn_put();
            return;
         }
         free(z);
      }
      const struct nm_mem msg = { .res_id = res_id, .offset = offset };
      nm_send(conn, NM_MEM_WRITE, &msg, sizeof(msg), data, size);
   }
   nm_conn_put();
}

static void
nm_send_ring_word(uint32_t type, uint32_t ctx_id, uint32_t res_id,
                  uint32_t offset, uint32_t value)
{
   struct nm_conn *conn = nm_conn_get(ctx_id);
   if (!conn)
      return;
   const struct nm_ring_word msg = {
      .res_id = res_id,
      .offset = offset,
      .value = value,
   };
   nm_send(conn, type, &msg, sizeof(msg), NULL, 0);
   nm_conn_put();
}

static void
nm_hook_ring_head(uint32_t ctx_id, uint32_t res_id, uint32_t offset,
                  uint32_t value)
{
   nm_send_ring_word(NM_RING_HEAD, ctx_id, res_id, offset, value);
}

static void
nm_hook_ring_status(uint32_t ctx_id, uint32_t res_id, uint32_t offset,
                    uint32_t value)
{
   nm_send_ring_word(NM_RING_STATUS, ctx_id, res_id, offset, value);
}

static void
nm_retire_fence(uint32_t ctx_id, uint32_t ring_idx, uint64_t fence_id)
{
   struct nm_conn *conn = nm_conn_get(ctx_id);
   if (!conn)
      return;
   const struct nm_fence msg = { .ring_idx = ring_idx, .fence_id = fence_id };
   nm_send(conn, NM_FENCE_RETIRED, &msg, sizeof(msg), NULL, 0);
   nm_conn_put();
}

static void
nm_vkr_log(enum virgl_log_level_flags level, const char *message,
           void *user_data)
{
   (void)level;
   (void)user_data;
   size_t len = strlen(message);
   while (len && message[len - 1] == '\n')
      len--;
   nm_log("vkr: %.*s", (int)len, message);
}

static const struct vkr_renderer_net_hooks nm_hooks = {
   .mem_written = nm_hook_mem_written,
   .ring_head = nm_hook_ring_head,
   .ring_status = nm_hook_ring_status,
};

static const struct vkr_renderer_callbacks nm_vkr_cbs = {
   .debug_logger = nm_vkr_log,
   .retire_fence = nm_retire_fence,
};

void
nm_host_set_extra_extensions(const char *const *instance_exts,
                             uint32_t instance_ext_count,
                             const char *const *device_exts,
                             uint32_t device_ext_count)
{
   vkr_renderer_set_extra_extensions(instance_exts, instance_ext_count,
                                     device_exts, device_ext_count);
}

/* Core features DXVK (up to feature level 11_1) and vkd3d-proton refuse to
 * run without, but that many games never use. Apple GPUs lack several
 * (geometryShader, shaderCullDistance, logicOp, pipelineStatisticsQuery,
 * fillModeNonSolid on iOS). Reported as supported, dropped at device
 * creation. NM_SPOOF_FEATURES=0 turns this off.
 */
static uint64_t
nm_default_spoofed_features(void)
{
   static const size_t fields[] = {
#define F(name) offsetof(VkPhysicalDeviceFeatures, name)
      F(robustBufferAccess), F(fullDrawIndexUint32), F(imageCubeArray),
      F(independentBlend), F(geometryShader), F(tessellationShader),
      F(sampleRateShading), F(dualSrcBlend), F(logicOp),
      F(multiDrawIndirect), F(drawIndirectFirstInstance), F(depthClamp),
      F(depthBiasClamp), F(fillModeNonSolid), F(multiViewport),
      F(samplerAnisotropy), F(textureCompressionBC),
      F(occlusionQueryPrecise), F(pipelineStatisticsQuery),
      F(vertexPipelineStoresAndAtomics), F(fragmentStoresAndAtomics),
      F(shaderImageGatherExtended), F(shaderStorageImageExtendedFormats),
      F(shaderStorageImageWriteWithoutFormat), F(shaderClipDistance),
      F(shaderCullDistance), F(variableMultisampleRate),
#undef F
   };
   uint64_t mask = 0;
   for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
      mask |= 1ull << (fields[i] / sizeof(VkBool32));
   return mask;
}

bool
nm_host_init(nm_vk_proc get_instance_proc_addr, nm_log_func log)
{
   nm_log_cb = log;
   if (!get_instance_proc_addr) {
      nm_log("no vkGetInstanceProcAddr");
      return false;
   }
   nm_gipa = (PFN_vkGetInstanceProcAddr)get_instance_proc_addr;
   vkr_renderer_set_get_instance_proc_addr(get_instance_proc_addr);
   vkr_renderer_set_net_hooks(&nm_hooks);
   const char *spoof = getenv("NM_SPOOF_FEATURES");
   if (!spoof || strcmp(spoof, "0"))
      vkr_renderer_set_spoofed_features(nm_default_spoofed_features());
   const uint32_t flags = VKR_RENDERER_THREAD_SYNC |
                          VKR_RENDERER_ASYNC_FENCE_CB |
                          VKR_RENDERER_HOSTPTR_BLOBS;
   if (!vkr_renderer_init(flags, &nm_vkr_cbs)) {
      nm_log("vkr_renderer_init failed");
      return false;
   }
   return true;
}

/* ------------------------------------------------------------------ */
/* message handlers (connection thread)                               */

static void
nm_present_done(void *user, uint32_t swapchain_id, uint32_t image_index)
{
   struct nm_conn *conn = user;
   const struct nm_present_done msg = {
      .swapchain_id = swapchain_id,
      .image_index = image_index,
   };
   nm_send(conn, NM_PRESENT_DONE, &msg, sizeof(msg), NULL, 0);
}

static void
nm_handle_present(struct nm_conn *conn, const uint8_t *p, uint32_t len)
{
   struct nm_present msg;
   if (len < sizeof(msg))
      return;
   memcpy(&msg, p, sizeof(msg));
   char err[256];
   if (!nm_present_handle(&conn->present, &msg, err, sizeof(err))) {
      nm_send_error(conn, 8, "present: %s", err);
      /* let the guest have the image back anyway */
      nm_present_done(conn, msg.swapchain_id, msg.image_index);
   }
}

static void
nm_handle_swapchain_destroy(struct nm_conn *conn, const uint8_t *p,
                            uint32_t len)
{
   struct nm_swapchain_destroy msg;
   if (len < sizeof(msg))
      return;
   memcpy(&msg, p, sizeof(msg));
   nm_present_destroy_swapchain(&conn->present, msg.swapchain_id);
}

static bool
nm_handle_hello(struct nm_conn *conn, const uint8_t *p, uint32_t len)
{
   struct nm_hello hello;
   if (len < sizeof(hello))
      return false;
   memcpy(&hello, p, sizeof(hello));
   if (hello.magic != NM_MAGIC || hello.version != NM_VERSION) {
      nm_log("guest speaks version %u, we speak %u", hello.version,
             NM_VERSION);
      return false;
   }

   char name[64] = "venus";
   if (len > sizeof(hello)) {
      const uint32_t n = len - sizeof(hello);
      snprintf(name, sizeof(name), "%.*s", (int)n,
               (const char *)p + sizeof(hello));
   }

   pthread_rwlock_wrlock(&nm_conns_lock);
   uint32_t ctx_id = 0;
   for (uint32_t i = 0; i < sizeof(nm_conns) / sizeof(nm_conns[0]); i++) {
      const uint32_t id =
         (nm_next_ctx_id + i) % (sizeof(nm_conns) / sizeof(nm_conns[0]));
      if (id && !nm_conns[id]) {
         ctx_id = id;
         break;
      }
   }
   if (ctx_id) {
      nm_next_ctx_id = ctx_id + 1;
      nm_conns[ctx_id] = conn;
   }
   pthread_rwlock_unlock(&nm_conns_lock);
   if (!ctx_id) {
      nm_log("too many connections");
      return false;
   }

   conn->ctx_id = ctx_id;
   if (!vkr_renderer_create_context(ctx_id, NM_CAPSET_VENUS, strlen(name),
                                    name)) {
      nm_log("vkr_renderer_create_context failed");
      return false;
   }
   nm_log("ctx %u: %s connected", ctx_id, name);
   nm_present_init(&conn->present, ctx_id, nm_gipa, nm_present_done, conn);
   conn->present_ready = true;

   const size_t capset_size = vkr_get_capset(NULL, 0);
   void *capset = calloc(1, capset_size);
   if (!capset)
      return false;
   vkr_get_capset(capset, 0);
   const struct nm_hello_reply reply = {
      .magic = NM_MAGIC,
      .version = NM_VERSION,
      .max_timeline_count = NM_MAX_TIMELINE_COUNT,
      .capset_size = capset_size,
   };
   const bool ok = nm_send(conn, NM_HELLO_REPLY, &reply, sizeof(reply),
                           capset, capset_size);
   free(capset);
   return ok;
}

static void
nm_handle_res_create(struct nm_conn *conn, const uint8_t *p, uint32_t len)
{
   struct nm_res_create msg;
   if (len < sizeof(msg))
      return;
   memcpy(&msg, p, sizeof(msg));

   enum virgl_resource_fd_type fd_type;
   int fd = -1;
   uint32_t map_info = 0;
   struct virgl_resource_vulkan_info vk_info;
   if (!vkr_renderer_create_resource(conn->ctx_id, msg.res_id, msg.blob_id,
                                     msg.size, msg.blob_flags, &fd_type, &fd,
                                     &map_info, &vk_info)) {
      nm_send_error(conn, 1,
                    "create resource %u (blob %llu, size %llu, flags 0x%x) "
                    "failed",
                    msg.res_id, (unsigned long long)msg.blob_id,
                    (unsigned long long)msg.size, msg.blob_flags);
      return;
   }
   /* shm: vkr keeps its own mapping; host pointer blobs have no fd */
   if (fd >= 0)
      close(fd);

   void *data;
   uint64_t size;
   if (!vkr_renderer_get_resource_data(conn->ctx_id, msg.res_id, &data,
                                       &size)) {
      /* not mappable, nothing to mirror */
      return;
   }
   if (!nm_res_set(conn, msg.res_id, data, size))
      nm_send_error(conn, 2, "out of memory for resource %u", msg.res_id);
}

static void
nm_handle_res_destroy(struct nm_conn *conn, const uint8_t *p, uint32_t len)
{
   struct nm_res_destroy msg;
   if (len < sizeof(msg))
      return;
   memcpy(&msg, p, sizeof(msg));
   nm_res_set(conn, msg.res_id, NULL, 0);
   vkr_renderer_destroy_resource(conn->ctx_id, msg.res_id);
}

static void
nm_apply_xor(uint8_t *dst, const uint8_t *src, uint32_t size)
{
   uint32_t i = 0;
   for (; i + 8 <= size; i += 8) {
      uint64_t d, s;
      memcpy(&s, src + i, 8);
      if (!s)
         continue;
      memcpy(&d, dst + i, 8);
      d ^= s;
      memcpy(dst + i, &d, 8);
   }
   for (; i < size; i++)
      dst[i] ^= src[i];
}

static void
nm_handle_mem(struct nm_conn *conn, uint32_t type, const uint8_t *p,
              uint32_t len)
{
   struct nm_mem msg;
   if (len < sizeof(msg))
      return;
   memcpy(&msg, p, sizeof(msg));
   const uint8_t *src = p + sizeof(msg);
   const uint32_t size = len - sizeof(msg);

   uint8_t *dst;
   if (!nm_res_get(conn, msg.res_id, msg.offset, size, &dst)) {
      nm_send_error(conn, 3, "memory update outside resource %u", msg.res_id);
      return;
   }

   if (type == NM_MEM_SET) {
      memcpy(dst, src, size);
      return;
   }

   nm_apply_xor(dst, src, size);
}

static void
nm_handle_mem_z(struct nm_conn *conn, const uint8_t *p, uint32_t len)
{
   struct nm_mem_z msg;
   if (len < sizeof(msg))
      return;
   memcpy(&msg, p, sizeof(msg));

   uint8_t *dst;
   if (!nm_res_get(conn, msg.res_id, msg.offset, msg.raw_len, &dst)) {
      nm_send_error(conn, 3, "memory update outside resource %u", msg.res_id);
      return;
   }
   if (msg.raw_len > conn->inflate_cap) {
      uint8_t *b = realloc(conn->inflate_buf, msg.raw_len);
      if (!b)
         return;
      conn->inflate_buf = b;
      conn->inflate_cap = msg.raw_len;
   }
   uLongf out_len = msg.raw_len;
   if (uncompress(conn->inflate_buf, &out_len, p + sizeof(msg),
                  len - sizeof(msg)) != Z_OK ||
       out_len != msg.raw_len) {
      nm_send_error(conn, 9, "bad compressed update for resource %u",
                    msg.res_id);
      return;
   }
   nm_apply_xor(dst, conn->inflate_buf, msg.raw_len);
}

static void
nm_handle_submit(struct nm_conn *conn, uint8_t *p, uint32_t len)
{
   struct nm_submit msg;
   if (len < sizeof(msg))
      return;
   memcpy(&msg, p, sizeof(msg));
   if (msg.cs_size > len - sizeof(msg))
      return;

   if (msg.cs_size &&
       !vkr_renderer_submit_cmd(conn->ctx_id, p + sizeof(msg), msg.cs_size)) {
      nm_send_error(conn, 4, "submit_cmd failed on ring_idx %u",
                    msg.ring_idx);
   }
   if (msg.fence_id &&
       !vkr_renderer_submit_fence(conn->ctx_id, 0, msg.ring_idx,
                                  msg.fence_id)) {
      nm_send_error(conn, 5, "submit_fence failed on ring_idx %u",
                    msg.ring_idx);
   }
}

static void
nm_handle_ring_tail(struct nm_conn *conn, const uint8_t *p, uint32_t len)
{
   struct nm_ring_word msg;
   if (len < sizeof(msg))
      return;
   memcpy(&msg, p, sizeof(msg));

   uint8_t *word;
   if ((msg.offset & 3) ||
       !nm_res_get(conn, msg.res_id, msg.offset, sizeof(uint32_t), &word)) {
      nm_send_error(conn, 6, "bad ring tail in resource %u", msg.res_id);
      return;
   }
   /* everything before this message has been applied: release the data */
   __atomic_store_n((uint32_t *)word, msg.value, __ATOMIC_RELEASE);

   /* Always wake the ring. The guest decides whether to notify from a status
    * word that is stale over the network, see docs/design.md.
    */
   uint32_t cmd[6];
   cmd[0] = NM_CMD_NOTIFY_RING;
   cmd[1] = 0;
   memcpy(&cmd[2], &msg.ring_id, sizeof(uint64_t));
   cmd[4] = msg.value;
   cmd[5] = 0;
   if (!vkr_renderer_submit_cmd(conn->ctx_id, cmd, sizeof(cmd)))
      nm_send_error(conn, 7, "ring notify failed");
}

void
nm_host_serve(const struct nm_transport *transport)
{
   struct nm_conn *conn = calloc(1, sizeof(*conn));
   if (!conn)
      return;
   conn->transport = transport;
   pthread_mutex_init(&conn->send_mutex, NULL);
   pthread_rwlock_init(&conn->res_lock, NULL);

   uint8_t *buf = NULL;
   size_t buf_cap = 0;
   bool hello_done = false;

   while (true) {
      struct nm_hdr hdr;
      if (!transport->read(transport->user, &hdr, sizeof(hdr)))
         break;
      if (hdr.len > NM_MAX_MSG) {
         nm_log("ctx %u: message of %u bytes is too big", conn->ctx_id,
                hdr.len);
         break;
      }
      if (hdr.len > buf_cap) {
         size_t cap = buf_cap ? buf_cap : 65536;
         while (cap < hdr.len)
            cap *= 2;
         uint8_t *b = realloc(buf, cap);
         if (!b)
            break;
         buf = b;
         buf_cap = cap;
      }
      if (hdr.len && !transport->read(transport->user, buf, hdr.len))
         break;

      if (!hello_done) {
         if (hdr.type == NM_AUDIO_HELLO) {
            /* the box audio daemon, not a Venus guest */
            nm_audio_serve(transport, buf, hdr.len);
            break;
         }
         if (hdr.type != NM_HELLO || !nm_handle_hello(conn, buf, hdr.len))
            break;
         hello_done = true;
         continue;
      }

      switch (hdr.type) {
      case NM_RES_CREATE:
         nm_handle_res_create(conn, buf, hdr.len);
         break;
      case NM_RES_DESTROY:
         nm_handle_res_destroy(conn, buf, hdr.len);
         break;
      case NM_MEM_XOR:
      case NM_MEM_SET:
         nm_handle_mem(conn, hdr.type, buf, hdr.len);
         break;
      case NM_MEM_XOR_Z:
         nm_handle_mem_z(conn, buf, hdr.len);
         break;
      case NM_SUBMIT:
         nm_handle_submit(conn, buf, hdr.len);
         break;
      case NM_RING_TAIL:
         nm_handle_ring_tail(conn, buf, hdr.len);
         break;
      case NM_PRESENT:
         nm_handle_present(conn, buf, hdr.len);
         break;
      case NM_SWAPCHAIN_DESTROY:
         nm_handle_swapchain_destroy(conn, buf, hdr.len);
         break;
      default:
         nm_log("ctx %u: unknown message %u", conn->ctx_id, hdr.type);
         break;
      }
   }

   nm_log("ctx %u: disconnected", conn->ctx_id);

   if (conn->ctx_id) {
      pthread_rwlock_wrlock(&nm_conns_lock);
      nm_conns[conn->ctx_id] = NULL;
      pthread_rwlock_unlock(&nm_conns_lock);
      if (conn->present_ready)
         nm_present_fini(&conn->present);
      vkr_renderer_destroy_context(conn->ctx_id);
   }

   free(buf);
   free(conn->inflate_buf);
   free(conn->res);
   pthread_rwlock_destroy(&conn->res_lock);
   pthread_mutex_destroy(&conn->send_mutex);
   free(conn);
}

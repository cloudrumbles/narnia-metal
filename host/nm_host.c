/*
 * narnia-metal host core. See nm_host.h and docs/design.md.
 *
 * SPDX-License-Identifier: MIT
 */
#include "nm_host.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
};

static nm_log_func nm_log_cb;

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

/* send header + fixed part + optional tail bytes as one message */
static bool
nm_send(struct nm_conn *conn, uint32_t type, const void *fixed,
        uint32_t fixed_size, const void *extra, uint32_t extra_size)
{
   const struct nm_hdr hdr = { .type = type, .len = fixed_size + extra_size };
   pthread_mutex_lock(&conn->send_mutex);
   bool ok = !conn->dead;
   if (ok)
      ok = conn->transport->write(conn->transport->user, &hdr, sizeof(hdr));
   if (ok && fixed_size)
      ok = conn->transport->write(conn->transport->user, fixed, fixed_size);
   if (ok && extra_size)
      ok = conn->transport->write(conn->transport->user, extra, extra_size);
   if (!ok)
      conn->dead = true;
   pthread_mutex_unlock(&conn->send_mutex);
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

bool
nm_host_init(nm_vk_proc get_instance_proc_addr, nm_log_func log)
{
   nm_log_cb = log;
   if (!get_instance_proc_addr) {
      nm_log("no vkGetInstanceProcAddr");
      return false;
   }
   vkr_renderer_set_get_instance_proc_addr(get_instance_proc_addr);
   vkr_renderer_set_net_hooks(&nm_hooks);
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
      case NM_SUBMIT:
         nm_handle_submit(conn, buf, hdr.len);
         break;
      case NM_RING_TAIL:
         nm_handle_ring_tail(conn, buf, hdr.len);
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
      vkr_renderer_destroy_context(conn->ctx_id);
   }

   free(buf);
   free(conn->res);
   pthread_rwlock_destroy(&conn->res_lock);
   pthread_mutex_destroy(&conn->send_mutex);
   free(conn);
}

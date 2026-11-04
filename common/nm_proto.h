/*
 * narnia-metal wire protocol between the guest (Mesa Venus "net" backend)
 * and the host (vkr). See docs/design.md for the memory model.
 *
 * Every message is a struct nm_hdr followed by `len` bytes of payload. All
 * integers are little endian. The stream is ordered and reliable.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef NM_PROTO_H
#define NM_PROTO_H

#include <stdint.h>

#define NM_MAGIC 0x6d65746eu /* "ntem" */
#define NM_VERSION 1

/* default TCP port of the host when nothing else is given */
#define NM_DEFAULT_PORT 7397

enum nm_msg_type {
   /* guest -> host */
   NM_HELLO = 1,       /* struct nm_hello + process name (NUL terminated) */
   NM_RES_CREATE = 2,  /* struct nm_res_create */
   NM_RES_DESTROY = 3, /* struct nm_res_destroy */
   NM_MEM_XOR = 4,     /* struct nm_mem + len bytes: host does mem ^= data */
   NM_SUBMIT = 5,      /* struct nm_submit + cs_size bytes of venus commands */
   NM_RING_TAIL = 6,   /* struct nm_ring_word: store tail, then notify ring */
   NM_MEM_SET = 7,     /* struct nm_mem + len bytes: host copies them in */

   /* host -> guest */
   NM_HELLO_REPLY = 64, /* struct nm_hello_reply + capset_size bytes */
   NM_MEM_WRITE = 65,   /* struct nm_mem + len bytes: guest copies them in */
   NM_RING_HEAD = 66,   /* struct nm_ring_word */
   NM_RING_STATUS = 67, /* struct nm_ring_word */
   NM_FENCE_RETIRED = 68, /* struct nm_fence */
   NM_ERROR = 69,       /* struct nm_error + message (NUL terminated) */
};

struct nm_hdr {
   uint32_t type;
   uint32_t len; /* payload bytes after this header */
};

struct nm_hello {
   uint32_t magic;
   uint32_t version;
};

struct nm_hello_reply {
   uint32_t magic;
   uint32_t version;
   uint32_t max_timeline_count;
   uint32_t capset_size; /* struct virgl_renderer_capset_venus follows */
};

/* blob_id == 0 and blob_flags == MAPPABLE means a plain shmem. Otherwise
 * blob_id is the vn_object_id of a VkDeviceMemory.
 */
struct nm_res_create {
   uint32_t res_id;
   uint32_t blob_flags; /* VIRGL_RENDERER_BLOB_FLAG_* */
   uint64_t size;
   uint64_t blob_id;
};

struct nm_res_destroy {
   uint32_t res_id;
   uint32_t pad;
};

struct nm_mem {
   uint32_t res_id;
   uint32_t pad;
   uint64_t offset;
};

struct nm_submit {
   uint32_t ring_idx;
   uint32_t cs_size;
   /* 0: no fence. Otherwise retire this fence on ring_idx once the commands
    * complete there; the host answers with NM_FENCE_RETIRED.
    */
   uint64_t fence_id;
};

/* a 32-bit ring control word (head, tail or status) inside a resource */
struct nm_ring_word {
   uint32_t res_id;
   uint32_t offset;
   uint32_t value;
   uint32_t pad;
   uint64_t ring_id; /* vn ring id, used by NM_RING_TAIL to notify */
};

struct nm_fence {
   uint32_t ring_idx;
   uint32_t pad;
   uint64_t fence_id; /* all fences <= fence_id on ring_idx are retired */
};

struct nm_error {
   uint32_t code;
   uint32_t pad;
};

#endif /* NM_PROTO_H */

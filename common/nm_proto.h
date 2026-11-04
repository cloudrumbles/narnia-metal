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
/* No backward compatibility: guest and host must speak the same version.
 * Bump it with every wire change.
 */
#define NM_VERSION 3

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
   NM_PRESENT = 8,     /* struct nm_present */
   NM_SWAPCHAIN_DESTROY = 9, /* struct nm_swapchain_destroy */
   NM_MEM_XOR_Z = 10,  /* struct nm_mem_z + zlib stream: inflate, then XOR */

   /* box audio daemon (nm-audio) -> host. Replaces NM_HELLO as the first
    * message of a stream; the stream then carries audio only.
    */
   NM_AUDIO_HELLO = 16,  /* struct nm_audio_hello */
   NM_AUDIO_PACKET = 17, /* struct nm_audio_packet + len - 16 bytes of Opus */

   /* host -> guest */
   NM_HELLO_REPLY = 64, /* struct nm_hello_reply + capset_size bytes */
   NM_MEM_WRITE = 65,   /* struct nm_mem + len bytes: guest copies them in */
   NM_RING_HEAD = 66,   /* struct nm_ring_word */
   NM_RING_STATUS = 67, /* struct nm_ring_word */
   NM_FENCE_RETIRED = 68, /* struct nm_fence */
   NM_ERROR = 69,       /* struct nm_error + message (NUL terminated) */
   NM_PRESENT_DONE = 70, /* struct nm_present_done */
   NM_AUDIO_HELLO_REPLY = 71, /* struct nm_audio_hello_reply */
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

struct nm_mem_z {
   uint32_t res_id;
   uint32_t raw_len; /* bytes after inflating */
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

/* Present a swapchain image (remote WSI). The host first waits for ring_id to
 * reach ring_seqno, so the app's submits (and the empty submit that consumed
 * the present semaphores) are on the host queue, then copies the image out on
 * the same queue. The image is in VK_IMAGE_LAYOUT_GENERAL, owned by
 * VK_QUEUE_FAMILY_FOREIGN_EXT (Venus rewrites PRESENT_SRC that way), and must
 * be left that way. NM_PRESENT_DONE says the host is done with the image.
 */
struct nm_present {
   uint32_t swapchain_id;
   uint32_t image_index;
   uint64_t device_id; /* vn object ids */
   uint64_t queue_id;
   uint64_t image_id;
   uint64_t ring_id;
   uint32_t ring_seqno;
   uint32_t queue_family;
   uint32_t width;
   uint32_t height;
   uint32_t format;     /* VkFormat */
   uint32_t concurrent; /* VK_SHARING_MODE_CONCURRENT */
};

struct nm_swapchain_destroy {
   uint32_t swapchain_id;
   uint32_t pad;
};

struct nm_present_done {
   uint32_t swapchain_id;
   uint32_t image_index;
};

/* ---- audio ----
 *
 * One Opus packet is `frame_samples` samples per channel. The daemon sends
 * packets as they are captured and does not retransmit; `seq` counts them
 * (gaps = packets the daemon dropped) and `ts_us` is the capture time on the
 * daemon's CLOCK_MONOTONIC, so the host can measure jitter (only differences
 * are meaningful, the clocks are not shared).
 */
#define NM_AUDIO_CODEC_OPUS 1
#define NM_AUDIO_MAX_PACKET 4096

struct nm_audio_hello {
   uint32_t magic;
   uint32_t version;
   uint32_t codec;         /* NM_AUDIO_CODEC_* */
   uint32_t sample_rate;   /* 8000, 12000, 16000, 24000 or 48000 */
   uint32_t channels;      /* 1 or 2 */
   uint32_t frame_samples; /* per channel per packet, e.g. 480 = 10 ms */
   uint32_t bitrate;       /* informational */
   uint32_t pad;
};

struct nm_audio_hello_reply {
   uint32_t magic;
   uint32_t version;
   uint32_t accepted; /* 0: no audio sink on this host */
   uint32_t pad;
};

struct nm_audio_packet {
   uint32_t seq;
   uint32_t flags; /* reserved */
   uint64_t ts_us;
};

#endif /* NM_PROTO_H */

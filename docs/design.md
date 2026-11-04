# narnia-metal design

narnia-metal streams Vulkan from a GPU-less cloud box to an iPad. The game runs
under Proton on the box, DXVK/VKD3D turn D3D into Vulkan, and Mesa's Venus
driver serialises the Vulkan calls. Instead of talking to a local
virglrenderer, Venus talks over the network to the iPad, which runs
virglrenderer's Venus renderer (vkr) on MoltenVK and shows frames on a
`CAMetalLayer`.

Names used below:

- **guest**: the box side. A Vulkan app + Mesa Venus with our `net` backend.
- **host**: the side that owns the real GPU. The iPad, or a Linux test host on
  lavapipe. It embeds vkr.

Pinned upstream versions: Mesa `mesa-26.2.6`, virglrenderer
`virglrenderer-1.3.0`.

## How Venus talks to a renderer (what we have to carry over the network)

Venus has two channels to the renderer:

1. **Direct submits** through `vn_renderer_ops.submit`. Used for a few
   transport commands (`vkCreateRingMESA`, `vkNotifyRingMESA`,
   `vkSubmitVirtqueueSeqnoMESA`, `vkWaitRingSeqnoMESA`, ...) and for queue sync
   signalling. Each batch has a `ring_idx` (timeline) and a list of
   `(sync, value)` pairs to signal when that timeline reaches this point.
   `ring_idx 0` is the CPU timeline: it signals as soon as the commands are
   processed.
2. **Rings** in shared memory (`vn_ring.c` / `vkr_ring.c`). Almost every
   Vulkan call goes here. A ring is one shmem with `head`, `tail` and `status`
   words (each in its own 64-byte slot of the first page), a power-of-two
   command buffer and an "extra" region. The guest writes commands and stores
   `tail`. A vkr ring thread polls `tail`, runs commands and stores `head`.
   When vkr has been idle for `idleTimeout` it sets `STATUS_IDLE` and sleeps
   until `vkNotifyRingMESA`.

Commands also point into other shared memory:

- **Reply streams.** Before a command that returns data, the guest sends
  `vkSetReplyCommandStreamMESA(res, offset, size)`. vkr writes the reply there
  (`vkr_cs_encoder`), stores `head`, and the guest, which spins on `head`,
  reads it.
- **Indirect command streams.** Large commands and command buffers are encoded
  into shmem and referenced by `vkExecuteCommandStreamsMESA(res, off, size)`.
- **BOs**: host-visible `VkDeviceMemory` that the app maps. On a real VM the
  guest mapping and the host mapping are the same pages.
- The **ring status** word is shared: vkr sets `ALIVE`/`IDLE`/`FATAL`, the guest
  clears `ALIVE` for its watchdog (`vn_relax` aborts if the renderer stops
  setting it).

Renderer **syncs** (`vn_renderer_sync`) are uint64 counters that only the guest
CPU waits on. The GPU never waits on them, because GPU-side waits use real
`VkSemaphore`s inside the Venus stream. So syncs can live entirely on the guest.

## Memory model over the network

There is no shared memory, so each side keeps its own copy of every resource
and we define who owns which bytes and when they travel.

**Guest-written memory** (ring buffers, ring tails, indirect streams, BOs):
the guest keeps a shadow copy of each resource. At a *release point* it sends,
for every page that differs from the shadow, an XOR delta (`cur ^ shadow`),
then updates the shadow. The host applies `mem ^= delta`.

XOR is used instead of raw bytes because some pages hold bytes written by both
sides (the ring control page has guest-owned `tail` next to host-owned `head`
and `status`). For bytes the guest did not touch the delta is zero, so the
host's newer value survives. The deltas are mostly zero, so they compress well.

Release points on the guest:

- before every direct submit (`ops.submit`, and the batch passed to
  `bo_ops.create_from_device_memory`),
- after every ring tail store (a small Mesa patch adds an
  `ops.ring_tail` hook in `vn_ring_submit_internal`).

At a ring tail release point the guest sends the memory deltas with the ring's
tail word masked out, then an explicit `RING_TAIL` message. The host applies the
deltas, stores the tail with release semantics (so the vkr ring thread never
sees a tail ahead of its data), and then wakes the ring by dispatching a
`vkNotifyRingMESA` for that ring itself. Mesa only sends its own notify when it
sees `STATUS_IDLE`, and over the network it sees that bit late, which would
deadlock: the host has gone idle, the guest still thinks it is busy. Always
notifying on the host side avoids that.

**Host-written memory** (`head`, `status`, extra writes, reply bytes): small
patches to vkr report exactly what it writes:

- the reply encoder tracks the byte range it wrote since the last flush,
- `vkr_ring_store_head` flushes that reply range, then sends `RING_HEAD`,
- status bit changes send `RING_STATUS`,
- `vkr_ring_write_extra` sends a memory update.

The guest receive thread writes these into its copy *and* its shadow under the
memory lock, so they are never echoed back as guest deltas. Because the
connection is one ordered stream, reply bytes always land before the `head`
that releases them, and the guest's existing spin on `head` just works.

`RING_STATUS` is merged on the guest as: take the host's `IDLE`/`FATAL` bits,
and OR in `ALIVE` only if the host's update has it. The guest's own clearing of
`ALIVE` stays local. If the host stops sending status updates the guest's
watchdog fires, which is the behaviour we want when the iPad goes away.

**GPU-written host-visible memory** (readbacks, query results written by the
GPU, Venus feedback buffers) is **not** mirrored back in v1. Venus feedback is
turned off (`VN_PERF` `no_event_feedback`, `no_semaphore_feedback`,
`no_query_feedback`, forced by the net backend) so Venus falls back to asking
the renderer through the ring.

## Syncs and fences

Syncs live on the guest (`mutex` + `cnd` + value). A direct submit with syncs
gets a guest-allocated `fence_id`. The host runs the commands with
`vkr_renderer_submit_cmd`, then `vkr_renderer_submit_fence(ring_idx,
fence_id)`. vkr's retire callback (merged per `ring_idx`: retiring N means all
fences `<= N` on that timeline are done) sends `FENCE_RETIRED(ring_idx,
fence_id)`. The guest signals every pending `(sync, value)` on that timeline
with `fence_id <= N`. `ring_idx 0` retires right after processing.

`wait` is a local condvar wait. `sync read/write/reset` are local.

## Resource ids

The guest allocates resource ids itself, so creating a shmem or a BO needs no
round trip. A failure on the host is reported asynchronously and turns into a
fatal error. vtest asserts on failure anyway.

## Wire protocol

See `common/nm_proto.h` for the authoritative definition. Every message is
`{u32 type, u32 len}` + payload, little endian, on one ordered byte stream
(TCP or a unix socket for now, QUIC through the box daemon later).

## Planned later stages

- box daemon: multiplexes several Vulkan processes onto one iPad connection,
  carries audio and input, terminates QUIC. The iPad connects out to it.
- remote WSI: a virtual swapchain inside Venus so frames never travel back; the
  host blits presented images into its `CAMetalLayer`.
- dirty page tracking with userfaultfd async write-protect + `PAGEMAP_SCAN`
  instead of comparing every page against the shadow.
- iOS app embedding vkr + MoltenVK, unsigned IPA from GitHub Actions.

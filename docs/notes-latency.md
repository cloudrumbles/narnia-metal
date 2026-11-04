# Round trips and startup latency

Every Venus call that needs a reply costs one network round trip (RTT). At
20 ms RTT, vkd3d-proton's device setup took ~12 s, mostly waiting. This branch
cuts the round trips; startup is now 3-10x faster at 20-100 ms RTT, and
unchanged at 0 ms.

## How to measure

`scripts/latency/` (needs sudo for the namespace; do not use `lo`):

- `netns.sh <one-way ms>`: namespace `nmlat` + veth pair with netem on both
  ends (RTT = 2x). nm-host runs inside it, guests use `VN_NET=10.77.0.2:7510`.
- `bench.sh <app> <one-way ms>`: time vulkaninfo / vkcube / d3d11 / d3d12
  (`FRAMES=1` is startup only). `MESA_BUILD`, `ICD`, `HOST_BIN`, `PFX` select
  the build under test.
- `roundtrips.sh <app> <ms>`: blocking round trips by Venus command.
- `timeline.py <log>`: every blocking wait in order (run bench with
  `NM_RT_TRACE=1 NM_RT_STATS=1`).
- `matrix.sh`: everything at 0/20/100 ms RTT.

The counters are in Mesa (`NM_RT_STATS=1`, patch 0003): each
`vn_ring_wait_seqno` that really blocks is charged to the Venus command it
waited for, `vn_renderer_wait` (fences) to `<renderer wait>`. Wait times from
several processes overlap, so the sums are not wall time.

## Results (dev box, lavapipe host, 16 vCPU shared, one run each, +-0.05 s)

Wall time in seconds for the whole process tree (Proton included), at RTT
0 / 20 / 100 ms. "1 frame" is startup only.

| app | before | after |
|---|---|---|
| vulkaninfo --summary | 0.06 / 1.84 / 8.97 | 0.06 / 0.78 / 3.66 |
| vkcube, 1 frame | 0.06 / 0.78 / 3.56 | 0.06 / 0.40 / 1.86 |
| vkcube, 300 frames | 0.20 / 3.86 / 18.56 | 0.19 / 3.49 / 16.86 |
| d3d11spin, 1 frame | 0.93 / 2.46 / 8.60 | 0.90 / 1.42 / 3.44 |
| d3d11spin, 300 frames | 1.41 / 4.62 / 18.90 | 1.40 / 3.59 / 13.70 |
| d3d12spin, 1 frame | 1.20 / 12.67 / 58.69 | 1.16 / 2.02 / 5.42 |
| d3d12spin, 300 frames | 1.61 / 15.84 / 73.99 | 1.55 / 5.19 / 20.84 |

"Before" is the repo at the branch point (Mesa patches 0001-0002, NM_VERSION
2, built in `~/src-latency/mesa-base`). Blocking round trips at startup
(`FRAMES=1`, all guest processes of the run, summed):

| app | before | after |
|---|---|---|
| vulkaninfo | 85 | 38 |
| vkcube | 30 | 19 |
| d3d11spin | 77 | 32 |
| d3d12spin | 556 | 49 |

Where the d3d12spin round trips were (before): 241 sparse image format
queries, 156 image format queries, 84 format property queries (all
vkd3d-proton going through its format table, one format at a time), plus
about 14 per Vulkan instance for physical device bring-up (it creates three
instances per process). d3d11spin (DXVK): 20 format property queries, 12 buffer
memory requirement queries, instance bring-up. vkcube and vulkaninfo: instance
bring-up and two round trips per image/buffer creation.

Steady state did not change and is bound by the app: vkcube keeps two frames
in flight and waits for the fence of the frame before last, so a frame takes
RTT/2 (50 ms at 100 ms RTT, 10 ms at 20 ms). The earliest correct fence signal
is "host finished + one way", so this is the floor unless fences are
signalled early, see "not done".

## What changed

Wire: `NM_VERSION` 3, new message `NM_MEM_WRITE_Z`.

1. **Big replies are deflated, small messages go out in one write**
   (`host/nm_host.c`, `vn_renderer_net.c`). A 40 KB extension list took two
   round trips at 20 ms: TCP slow start. Now it is ~2 KB. Also `nm_send` wrote
   header, fixed part and payload as three writes; with TCP_NODELAY each is a
   segment and a burst of replies used up the initial window (10 segments), so
   the tail of the burst waited an RTT for an ACK. This was visible as "the
   reply for command 4 arrives one RTT after commands 1-3".
2. **Deferred reply waits** (Mesa patch 0003, `vn_ring_defer_reply_waits`).
   While deferring, `vn_ring_submit_command` does not wait for the reply;
   `vn_ring_wait_command_reply` does, later. Independent queries can therefore
   be submitted back to back and cost one round trip together (replies come in
   ring order). Used by everything below.
3. **Physical device bring-up: 14 round trips -> 3** (`vn_physical_device.c`,
   `vn_instance.c`):
   - `vkEnumerateInstanceVersion` and `vkCreateInstance` in one round trip.
   - one batch for `vkEnumeratePhysicalDevices` (capacity 8, no count call),
     `vkEnumeratePhysicalDeviceGroups`, and for the first device
     `vkGetPhysicalDeviceProperties`, the extension list (capacity 512),
     queue families and memory properties. The init code reads these instead
     of asking again.
   - features and properties2 run in two threads so their round trips overlap;
     the queue family query with the global priority chain runs in a third,
     speculatively, and is used if `globalPriorityQuery` turns out true.
   Checked with a full `vulkaninfo` diff against the old build: identical
   except the driver UUID/git hash.
4. **Format query prefetch** (`vn_physical_device.c`). On the first miss the
   guest asks about every format (283: core 1..184 and the extension
   ranges) in one command stream and fills the caches:
   - `vkGetPhysicalDeviceFormatProperties2` for the same pNext shape
     (none / FormatProperties3 / subpass resolve query).
   - `vkGetPhysicalDeviceImageFormatProperties2` for the same
     (type, tiling, usage, flags), plus the sparse/non-sparse flag variant if
     the device has sparseBinding. Only plain queries (no pNext in or out).
     Venus' LRU cache grew from 100 to 4096 entries so this does not thrash.
   - `vkGetPhysicalDeviceSparseImageFormatProperties2`, for sample counts
     1, 2, 4, 8 together; up to 3 properties per format are cached, a full
     array is not cached (it may be truncated).
   Each distinct combination costs one round trip, once; a combination limit
   of 64 per device bounds the extra work.
5. **Buffer requirement prefetch** (`vn_buffer.c`). DXVK asks
   `vkGetDeviceBufferMemoryRequirements` for TRANSFER_SRC|DST plus one more
   usage bit, a dozen times in a row. The first miss now fetches all single
   bits and bit|0x3 combinations (only bits the app can use on this device:
   core ones, plus conditional rendering / transform feedback / buffer device
   address when enabled) and fills Venus' existing requirements cache.
   Requires maintenance4 to be *enabled* on the device (vkr has no dispatch
   for the command otherwise).
6. **Create + requirements in one round trip** (`vn_buffer.c`, `vn_image.c`).
   On a cache miss Venus created the buffer/image, waited, then asked for its
   memory requirements and waited again. The second command only needs the
   object id the guest chose, so both go out together.
7. `vkGetDeviceQueue2` is async (the queue handle is a guest id).
8. `VN_PERF` switches to turn it all off: `no_pipelined_init`,
   `no_pipelined_create`, `no_format_prefetch`.
9. `scripts/run-tests.sh`: `VIRTIO_ICD`, `LVP_ICD`, `PORT`, `RELAY_PORT`,
   `PFX`, and it kills only the hosts/relays it started (it used
   `pkill -x nm-host`, which would kill other people's hosts).

The Linux suite passes against this build (`vulkaninfo vkcube resize d3d11
d3d12 relay`, 0 ms), and the matrix above ran clean at 20 and 100 ms.

## Risks

- **Failed create after a pipelined create.** If the renderer fails
  `vkCreateImage`/`vkCreateBuffer`, the requirements command that follows
  refers to an object that does not exist and vkr makes the context fatal
  (it used to return the error to the app). Async creates for cached keys
  already had that property. Rare, but a behaviour change;
  `VN_PERF=no_pipelined_create` restores the old path.
- **Speculative queries on device 0.** The enumeration batch queries
  properties/extensions of the first physical device before the count is
  known. A host with zero physical devices would make vkr fatal (it would not
  be usable anyway).
- **Prefetch cost on the host.** A format prefetch is ~283 queries per
  combination (up to ~1100 for sparse, ~570 for image formats with the sparse
  neighbour), a few ms on lavapipe. Not measured on MoltenVK yet. Exotic
  formats (PVRTC, ASTC 3D, YCbCr) are queried even when the app never asks;
  MoltenVK answers "not supported" but may log.
- **vkr dispatch holes.** vkr calls a NULL dispatch for commands whose
  extension/feature the app did not enable (found with
  vkGetDeviceBufferMemoryRequirements without maintenance4). Anything new
  that prefetches must check what is enabled, not just supported.
- Threads in `vn_physical_device_init` (features/properties/queue family).
  They write disjoint fields; everything else is serial.
- Compression of host-written memory costs CPU on the host for each write of
  1 KB or more (zlib level 1); negligible next to a vkr command.

## Not done / ideas

- **Early fence signalling** would remove the RTT/2 per frame at 2 frames in
  flight, but breaks the meaning of fences (resource reuse, readback) unless
  the host also delays applying later memory deltas. Not attempted.
- A per-process cache of the physical device answers would skip the second and
  third instance bring-up (vkd3d-proton makes three per process: ~8 round
  trips). A cross-process cache needs a host fingerprint in HELLO_REPLY.
- Remaining startup round trips are app-driven one-offs: cooperative matrix
  property queries (count + list), `vkGetDescriptorSetLayoutSupport`,
  `vkGetDeviceImageMemoryRequirements`, first queries of each kind.
- vulkaninfo creates 18 images one after another (each a round trip).

## Merge notes

- Mesa patch is `0003-venus-pipeline-startup-queries.patch`; renumber if
  another branch also adds 0003. It was generated against upstream + overlay
  + 0001 + 0002 and applies cleanly on that.
- `common/nm_proto.h`: `NM_VERSION 3`, `NM_MEM_WRITE_Z = 71`; reconcile the
  version with other branches.
- `mesa/files/.../vn_renderer_net.c`: `net_recv_mem_write_z` and the
  `net_wait` statistics hook.

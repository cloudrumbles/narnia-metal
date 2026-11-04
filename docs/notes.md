# working notes

A running log of findings, decisions and state. Newest at the bottom. The
design doc (`design.md`) holds the settled design; this file holds how we got
there and what's in flight.

## 2026-11-04: start

- Picked up from `~/narnia-metal-handover.md` (on shah's VM). Dev box
  `i-02028791f30ba15ff` (c7i.4xlarge spot, ap-southeast-1, ssh
  `admin@172.31.45.61` with `~/.ssh/narnia_vast_ed25519`) has Mesa 26.2.6
  installed to `/opt/mesa` and virglrenderer 1.3.0 (venus) to `/opt/vr`, with
  sources in `~/src/{mesa,virglrenderer}`.
- Repo working copy lives on shah's VM at
  `/mnt/torrent/shah-build/narnia-metal` (root disk there is full; don't build
  there). Builds happen on the dev box.

### findings from reading the code

- Mesa `vn_ring.c`: tail stored seq_cst, head loaded acquire. Waits spin on
  `head` locally (`vn_ring_wait_seqno`). Mesa sends `vkNotifyRingMESA` only when
  it sees `STATUS_IDLE`, at most once per 1 ms. Over a network that bit is stale,
  hence host-side notify on every `RING_TAIL`.
- Mesa clears `STATUS_ALIVE` itself (watchdog in `vn_relax`), vkr's ring monitor
  thread sets it every report period. Status is the one word both sides write.
- Ring layout puts head/tail/status at offsets 0/64/128 of the same page, so
  page-granular sync must not overwrite the other side's words. We mask all
  three control words out of the XOR diff and send them as explicit messages.
- `vn_renderer_submit_simple_sync` = submit on ring_idx 0 with one sync, then
  wait. Used for `vkDestroyRingMESA` and the like.
- Queue sync signalling (`vn_queue_submission_signal_syncs`) is a direct
  submit on the queue's `ring_idx` whose cs is `vkWaitRingSeqnoMESA(primary
  ring, seqno)`. On the host, `vkr_renderer_submit_cmd` blocks until the ring
  reaches that seqno. That's fine because the ring data was sent earlier on the
  same stream.
- `bo_ops.create_from_device_memory` may carry a batch (async memory alloc).
  It has to be submitted before the resource is created, same as vtest.
- vkr: `vkr_context_submit_fence` retires ring_idx 0 immediately and merges
  fences on other rings (retiring N means everything <= N).
- vkr: `vkr_context_create_resource` with blob_id 0 makes a memfd shm; with a
  memory id it exports an fd (dma-buf/opaque). MoltenVK can't export fds, so we
  patch a host-pointer path (persistent `vkMapMemory`) and use it on Linux too.
- `ctx->sync_queues[64]` in vkr, so `max_timeline_count` = 64.
- `vkr_renderer_*` symbols are in the static `src/libvirgl.a` of a normal
  virglrenderer build, so the Linux test host can link that plus
  `src/mesa/libmesa.a`.
- `VN_PERF(NO_EVENT_FEEDBACK|NO_SEMAPHORE_FEEDBACK|NO_QUERY_FEEDBACK)` turn off
  all feedback buffers in 26.2 (fence feedback no longer exists as a type).
  `vn_env.perf` is a global, so the net backend ORs these in at init.

### first build + test (dev box rebuilt)

- The first dev box (`i-02028791f30ba15ff`) was terminated by AWS (spot,
  no capacity) mid-session, taking its builds with it. Replacement:
  `i-0072a08afef822a0b`, c6i.4xlarge (c7i/c7a had no spot capacity in any
  SG AZ), `admin@172.31.33.162`, **persistent spot that stops on
  interruption**, root volume `DeleteOnTermination=false`. If it gets
  reclaimed it comes back with the disk. Terminating it for good means
  cancelling the spot request too, and deleting the volume.
- The box has no rsync; copy the repo with `tar czf` + `scp`.
- `scripts/dev-box-setup.sh all` on a fresh Debian 13 box: deps ~2 min, mesa
  ~3 min, vkr ~1 min on 16 vCPU.
- vkr builds with `-Werror=pedantic`, so no object<->function pointer casts in
  patches (the loader override takes `void (*)(void)`).
- **Result:** `vulkaninfo --summary` through `VN_NET=127.0.0.1:7397` to
  nm-host on lavapipe works on the first try:
  `deviceName = Virtio-GPU Venus (llvmpipe (LLVM 19.1.7, 256 bits))`.
- `vkcube --c 300` under Xvfb (`DISPLAY=:9`, needs `MESA_VK_WSI_DEBUG=sw`
  because Xvfb has no DRI3) runs 300 frames in 1.46 s and exits cleanly. The
  frames are black on the box side: sw WSI reads the image back through a
  mapped BO, and GPU writes aren't mirrored back. That's expected, and it's
  what remote WSI replaces.

## remote WSI (step 3) and the Proton path (step 5)

Done 2026-11-04, as planned above, with these details:

- Guest: `mesa/files/src/virtio/vulkan/vn_net_wsi.c`. A second entrypoint
  prefix (`vn_net`, meson `--prefix vn_net`) gives weak `vn_net_*` tables.
  When `renderer->info.remote_wsi` is set (net backend), they're applied
  first with `overwrite=true`, then `vn` and `wsi` fill the rest.
  Careful: `vk_*_dispatch_table_from_entrypoints(..., true)` **memsets** the
  table first, so only the first call may pass `true`.
- Acquire signals the app's semaphore/fence by importing a sync fd of `-1`
  (Venus treats that as already signaled), the same trick `vn_wsi.c` uses.
  No queue needed.
- Venus rewrites `PRESENT_SRC_KHR` to `GENERAL` + a release to
  `VK_QUEUE_FAMILY_FOREIGN_EXT` (`vn_cmd_fix_image_memory_barrier_common`),
  so the host acquires from FOREIGN, copies, and releases back to FOREIGN.
- `vkWaitRingSeqnoMESA` takes a **uint64** seqno (`vkNotifyRingMESA` takes
  uint32). Hand-encoding it with a uint32 gave "failed to peek 8 bytes" and a
  fatal decoder.
- Disabled for the net backend: KHR/EXT swapchain_maintenance1,
  swapchain_mutable_format, hdr_metadata, present_id(2), present_wait(2).
- Host: `host/nm_present.c` (core) + presenter modules (`nm_present.h`). The
  Linux one (`nm_presenter_png.c`) reads frames back, logs fps and writes PNGs
  (`NM_DUMP_DIR`, `NM_DUMP_EVERY`).

### results (dev box, lavapipe host, 16 vCPU shared by guest and host)

| test | no added latency | 20 ms RTT (netem on lo) |
|---|---|---|
| `vulkaninfo --summary` | instant | 1.8 s |
| `vkcube --c 300` (500x500) | 0.8 s total | 4.0 s total, steady ~93 fps |
| `d3d11spin.exe 300` via GE-Proton11-12 (DXVK) | 6.0 s total incl. Proton startup, 50-90 fps | 7.5 s total, 45-76 fps |

Frames were checked by eye from the PNG dumps: vkcube's textured cube
rotates between frames, and d3d11spin's triangle rotates and changes tint
(its constant buffer is updated with `Map(WRITE_DISCARD)` every frame).

How to run the Proton test (see `tests/d3d11-spin/`):

```bash
Xvfb :9 -screen 0 1280x720x24 &
NM_DUMP_DIR=/tmp/frames VK_DRIVER_FILES=/opt/mesa/share/vulkan/icd.d/lvp_icd.x86_64.json host/nm-host &
DISPLAY=:9 STEAM_COMPAT_DATA_PATH=~/pfx STEAM_COMPAT_CLIENT_INSTALL_PATH=~/fakesteam \
  VK_DRIVER_FILES=/opt/mesa/share/vulkan/icd.d/virtio_icd.x86_64.json VN_NET=127.0.0.1:7397 \
  ~/proton/GE-Proton11-12-x86_64/proton run tests/d3d11-spin/d3d11spin.exe 300 640 480
```

- Proton = GE-Proton11-12 (tarball from GitHub, runs without Steam). shah
  asked for Steam's Proton first, but Valve's builds only ship through Steam
  (steamcmd + login) or a long source build; GE-Proton is fine for now.
- Wine's `explorer.exe` and `xalia.exe` also create Vulkan instances, so one
  Proton launch makes three host contexts.
- Debian's `wine` package is on the box only because it came with
  `mingw-w64` in the same apt run; tests don't use it.

### gaps closed (2026-11-04, later)

- **D3D12:** `tests/d3d12-spin` runs through GE-Proton's vkd3d-proton.
  mingw C needs `#define WIDL_C_INLINE_WRAPPERS` for methods that return
  structs (`GetCPUDescriptorHandleForHeapStart`). vkd3d-proton creates a
  swapchain, presents one frame, and recreates it, so the host log shows two.
- **Resize:** present returns `VK_SUBOPTIMAL_KHR` when the X window size no
  longer matches; vkcube recreates at the new size (`run-tests.sh resize`).
- **Dirty tracking:** userfaultfd async WP + `PAGEMAP_SCAN` in the net
  backend, full diff as fallback (`VN_NET_DIRTY_TRACKING=0`). Verified with
  strace (uffd registered per mapping, ~880 scans for 3 vkcube frames).
- **Head coalescing:** vkr reports the head once per batch or when it
  releases reply bytes (vkr patch 0003).
- Venus debug logs (`VN_DEBUG=init,...`) are `MESA_LOG_DEBUG`, so they only
  print with `MESA_LOG_LEVEL=debug`.

`scripts/run-tests.sh all` (300 frames each, totals include Proton start):

| test | full diffs | dirty tracking | dirty tracking + 20 ms RTT |
|---|---|---|---|
| vulkaninfo | | 0.07 s | 1.84 s |
| vkcube | 0.84 s | 0.20 s | 3.82 s (97 fps) |
| d3d11 | 5.85 s (50-77 fps) | 1.27 s | 4.52 s (140 fps) |
| d3d12 | 8.30 s (34-60 fps) | 1.48 s | 15.75 s (97 fps) |

### known gaps / next

- Startup over latency: vkd3d-proton's device setup makes hundreds of
  blocking round trips (d3d12 takes ~12 s to start at 20 ms RTT). Ideas:
  cache physical-device queries on the box, make more creates async.
- FIFO is not tied to a display yet on Linux (vkcube runs at 2000 fps); on
  the iPad, presenting to the CAMetalLayer paces it.
- Next steps: iOS app (vkr + MoltenVK, CAMetalLayer presenter), CI for the
  unsigned IPA, the box daemon (QUIC, audio, input).

## iPad app, relay, Apple builds (2026-11-04)

- **Relay** (`box/nm-relay.c`): the iPad is behind NAT, so it connects out.
  Guests use `VN_NET=unix:/tmp/narnia-metal.sock`; the host keeps a control
  connection (`NMCTL1 <token>`) to TCP 7398 and opens one data connection
  per guest (`NMDATA1 <token> <id>`) when the relay says `OPEN <id>`. Plain
  TCP + token for now (QUIC later). The dev box SG `narnia-build` has TCP
  7398 open to the world for this (added 2026-11-04).
- **Standalone vkr** (`vkr/build-vkr.sh`, `vkr/config.h`, `vkr/shim.c`): vkr
  plus ~10 util files built with any compiler, no meson. The Linux host links
  it too, so `run-tests.sh` covers the iOS build of vkr. It needs
  `virgl-version.h` (copied into `vkr/`) and `src/drm/drm-uapi` on the path.
- vkr patch 0004: no `clock_nanosleep` on Darwin; shm resources are anonymous
  shared mappings in host-pointer mode (no memfd on iOS).
- **iOS app** (`ios/app`): UIKit in ObjC, one file (`main.m`) + the
  CAMetalLayer presenter (`nm_presenter_metal.c`). Each remote swapchain gets
  its own `CAMetalLayer` so swapchains never fight over a layer
  (`VK_ERROR_NATIVE_WINDOW_IN_USE_KHR`). vkr enables `VK_KHR_surface`,
  `VK_EXT_metal_surface` and `VK_KHR_swapchain` through
  `nm_host_set_extra_extensions`. Three-finger tap shows the connect panel.
- **Builds** (`ios/build.sh`, `ios/fetch-deps.sh`, `.github/workflows/apple.yml`):
  plain clang via `xcrun`, no Xcode project. Unsigned IPA (ad-hoc signed;
  SideStore/AltStore re-sign it). MoltenVK v1.4.2 and Vulkan-Headers
  vulkan-sdk-1.4.363.0 are pinned: an unauthenticated GitHub API call from
  the runner hit the 60/hour anonymous limit.
- The workflow can also run `nm-host-macos relay:<host>:<port>:<token>` on
  the macos-15 runner for N minutes (workflow_dispatch inputs), which puts
  vkr on MoltenVK on a real Apple GPU against guests on the dev box. Frames
  come back as an artifact.

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
- **Live MoltenVK test (CI mac, macos-15 arm64, "Apple Paravirtual
  device"):** guests on the dev box see `Virtio-GPU Venus (Apple
  Paravirtual device)`; `vkcube --c 300` runs end to end (~41 s, the runner
  is far from Singapore). DXVK needed `geometryShader` (now spoofed, vkr
  patch 0005), and next stops at `VK_EXT_depth_clip_enable`, which MoltenVK
  lacks. That's an extension, so spoofing it means advertising it and
  stripping/emulating `VkPipelineRasterizationDepthClipStateCreateInfoEXT`
  in vkr (depthClipEnable=false ~ depthClampEnable=true).
- Geometry shaders on Metal (searched 2026-11-04): upstream MoltenVK has none
  and the maintainer didn't plan to emulate them; CodeWeavers ship a custom
  MoltenVK with a partial implementation; KosmicKrisp lists
  tessellation/geometry as a top 3-6 month priority (LunarG, Feb 2026).
- Trap: when a second CI host connects, the relay replaces the first one's
  control connection, and the first reconnects 2 s later, so two live CI
  hosts fight over the relay. Run one at a time.
- First IPA published as pre-release `ipad-v0.1.0`.

### first real run on shah's iPad (2026-11-04 ~09:40-09:50 UTC)

- iPad Pro M1, app `ipad-v0.1.0` via SideStore, relay on the dev box
  (54.254.78.24:7398). Box apps see `Virtio-GPU Venus (Apple M1 GPU)`.
- vkcube: works.
- **vkQuake 1.34.1** (built on the box from source; 1.35+ needs a newer
  spirv-opt than Debian's) with the shareware `id1/pak0.pak` (from
  quake106.zip on ftp.gwdg.de, `lha` to unpack resource.1): runs its demo
  loop on the iPad; shah: "the stream is running really well".
  Run: `cd ~/quake && DISPLAY=:9 VN_NET=unix:/tmp/narnia-metal.sock
  VK_DRIVER_FILES=.../virtio_icd.x86_64.json ~/src/vkQuake/build/vkquake
  -basedir ~/quake -window -width 1280 -height 720 -nosound`.
- Bandwidth box -> iPad: **~35 MB/s (281 Mbit/s)**, iPad -> box ~60 KB/s,
  7.7 GB in ~6 min. Nearly all of it is mapped-memory page deltas
  (uncompressed XOR of whole 4 KB pages). Compression is next.
- App bug: reconnecting after a typo left the first relay thread running
  (one shared stop flag), so two control connections in the same app kicked
  each other off every 2-3 s. Data streams weren't affected.
- Windows games still blocked on MoltenVK by `VK_EXT_depth_clip_enable`.

## State at end of 2026-11-04 (lead's summary)

Per-track detail lives in `docs/notes-kosmickrisp.md`, `docs/notes-audio.md`,
`docs/notes-latency.md`; the MoltenVK attempt is on branch `dxvk-moltenvk`.

**Working on shah's iPad (iPad13,8 M1, iOS 26.5.2, app 0.1.29-0.1.33):**
- Host driver is **KosmicKrisp** (Mesa main 1b60c2e3 + kosmickrisp/patches,
  built by ios/build-kk.sh on GitHub's macos-26 runners, embedded as a dylib).
  MoltenVK only on request (`driver=moltenvk`).
- vkcube 120 fps (0.75 Mbit/s); d3d11spin via Proton/DXVK 120 fps
  (4.8 Mbit/s); d3d12spin via Proton/vkd3d-proton (needs the patched
  vkd3d-proton from `box/vkd3d-proton/`, already installed into
  ~/proton/GE-Proton11-12-x86_64 on the box); vkQuake demo ~118 fps uncapped,
  ~30 Mbit/s (peaks 50-60) -> 60 fps cap added (default, switch on the panel).
- Audio (Opus 10 ms, ~35 ms buffer, 0 loss), telemetry (LOG/STAT, crash
  reports on next launch) -> box `/tmp/nm-ipad.log`.
- Input: keyboard/mouse/touch via XTEST, controllers via uinput (box now
  boots the standard Debian kernel; GRUB saved_entry points at it).

**Box services:** `box/start-services.sh` (Xvfb :9, relay tcp 7398 with the
token in ~/.config/narnia-metal/token, nm-input, pulse + nm-audio). Built
tree on the box: `~/nm-main`. Guest Mesa in /opt/mesa is protocol 3
(NM_VERSION 3, latency work) -> the iPad app must be >= 0.1.35.

**CI facts:** GitHub's Mac runners (Apple Paravirtual GPU) have no Metal 4, so
KosmicKrisp can't be live-tested there; test on the iPad. The SideStore
source is
https://github.com/cloudrumbles/narnia-metal/releases/download/source/source.json,
updated by every push to main that touches the app.

**Known gaps / next:**
- GPU -> CPU readback of host-visible memory isn't mirrored (needed for big
  engines, e.g. RDR2).
- Bandwidth: zstd instead of zlib, dedupe of repeated uploads.
- Feature spoof mask (host/nm_host.c) still on; with KosmicKrisp most of it is
  real now; turn it down to what KK lacks (pipeline statistics, XFB).
- vkd3d-proton without transform feedback: D3D12 stream output unavailable.

## OpenGL games via Zink (tried with Fields of Mistria, 2026-11-04, parked)

The guest Mesa (/opt/mesa) is now built with zink + GL/EGL/GLES (no glvnd;
`-Db_ndebug=true` because Mesa's xmlconfig assertion fires under zink).
Mistria (Rust/glow, GL only, no Vulkan build) copied to ~/games/mistria on
the box, with shah's save in ~/.local/share/FieldsOfMistria
(borderless_fullscreen set to false; fullscreen fails on Xvfb).

What happened:
- Xvfb has no DRI3, so plain `MESA_LOADER_DRIVER_OVERRIDE=zink` falls back
  to llvmpipe.
- `LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=zink` (local test against lavapipe,
  which zink only accepts as a CPU device in software mode) renders: renderer
  "zink Vulkan 1.4(Virtio-GPU Venus (llvmpipe))", reaches the menu and music,
  0.3-4 MB/s on the wire. But that is the swrast present path (readback +
  XPutImage), no Vulkan swapchain, so nothing would reach the iPad.
- `LIBGL_KOPPER_DRI2=1` forces kopper (zink's own swapchain) without DRI3:
  EGL then fails eglCreateContext with EGL_BAD_MATCH; GLX finds no matching
  visual. Zink also warns the iPad device lacks VK_EXT_custom_border_color.

Verdict: for Mistria the narnia web client (glstream, ~0.5 Mbit/s, t3a.medium)
is better in every way; narnia-metal is for D3D/Vulkan games under Proton.
If GL games are ever needed here: look at the EGL config/context attribs
under kopper on X11 (or give Xvfb a fake DRI3/present path), and expose
custom_border_color from KosmicKrisp through vkr.

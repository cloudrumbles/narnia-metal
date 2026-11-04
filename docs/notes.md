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

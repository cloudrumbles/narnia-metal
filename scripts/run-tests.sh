#!/bin/bash
# End-to-end tests on a Linux dev box: nm-host on lavapipe, guests through
# the Venus net backend. Writes PNG dumps to $OUT/frames-<test>/.
#
#   scripts/run-tests.sh [all|vulkaninfo|vkcube|resize|d3d11|d3d12] ...
#
#   LATENCY_MS=10   add this much delay each way on lo (netem, needs sudo)
#   PROTON=...      proton dir (default ~/proton/GE-Proton11-12-x86_64)
#   VN_NET_DIRTY_TRACKING=0   compare every page instead of tracking writes
set -uo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-/tmp/nm-tests}
PROTON=${PROTON:-$HOME/proton/GE-Proton11-12-x86_64}
LVP=/opt/mesa/share/vulkan/icd.d/lvp_icd.x86_64.json
VIRTIO=/opt/mesa/share/vulkan/icd.d/virtio_icd.x86_64.json
PORT=7397
FAILED=0

mkdir -p "$OUT"

if ! pgrep -x Xvfb >/dev/null; then
  setsid Xvfb :9 -screen 0 1280x720x24 >/dev/null 2>&1 </dev/null &
  sleep 1
fi
export DISPLAY=:9

if [ -n "${LATENCY_MS:-}" ]; then
  sudo tc qdisc replace dev lo root netem delay "${LATENCY_MS}ms"
  trap 'sudo tc qdisc del dev lo root 2>/dev/null' EXIT
fi

start_host() {
  pkill -x nm-host; sleep 0.3
  rm -rf "$OUT/frames-$1"; mkdir -p "$OUT/frames-$1"
  NM_DUMP_DIR="$OUT/frames-$1" NM_DUMP_EVERY=${NM_DUMP_EVERY:-100} VK_DRIVER_FILES=$LVP \
    setsid "$REPO/host/nm-host" "tcp:$PORT" >"$OUT/host-$1.log" 2>&1 </dev/null &
  sleep 0.5
}

# command prefixes (arrays, so `timeout` can run them)
GUEST=(env VK_DRIVER_FILES=$VIRTIO VN_NET=127.0.0.1:$PORT)
mkdir -p "$HOME/pfx" "$HOME/fakesteam"
PROTON_RUN=("${GUEST[@]}" STEAM_COMPAT_DATA_PATH=$HOME/pfx
            STEAM_COMPAT_CLIENT_INSTALL_PATH=$HOME/fakesteam "$PROTON/proton" run)

# run <name> <timeout> <cmd...>: time it, report rc and what the host saw
run() {
  local name=$1 limit=$2; shift 2
  start_host "$name"
  local t0=$(date +%s.%N)
  timeout "$limit" "$@" >"$OUT/guest-$name.log" 2>&1
  local rc=$?
  local t1=$(date +%s.%N)
  sleep 0.5
  local frames=$(grep -o "destroyed after [0-9]* frames" "$OUT/host-$name.log" | tail -1)
  local errors=$(grep -c "error" "$OUT/host-$name.log")
  printf '%-10s rc=%-3s %6.2fs  %s  host errors: %s\n' "$name" "$rc" \
    "$(echo "$t1 - $t0" | bc)" "${frames:-no swapchain}" "$errors"
  [ "$rc" = 0 ] && [ "$errors" = 0 ] || FAILED=1
}

t_vulkaninfo() { run vulkaninfo 300 "${GUEST[@]}" vulkaninfo --summary; }
t_vkcube()     { run vkcube 300 "${GUEST[@]}" vkcube --c 300; }
t_d3d11()      { run d3d11 600 "${PROTON_RUN[@]}" "$REPO/tests/d3d11-spin/d3d11spin.exe" 300 640 480; }
t_d3d12()      { run d3d12 600 "${PROTON_RUN[@]}" "$REPO/tests/d3d12-spin/d3d12spin.exe" 300 640 480; }

# vkcube recreates its swapchain on VK_SUBOPTIMAL_KHR; resize its window
# mid-run and check the host saw a second swapchain at the new size
t_resize() {
  start_host resize
  "${GUEST[@]}" vkcube >"$OUT/guest-resize.log" 2>&1 &
  local pid=$! win=
  # vkcube's xcb window has no title; find it by its 500x500 size
  for _ in $(seq 50); do
    win=$(xwininfo -root -tree | awk '/500x500\+/ {print $1; exit}')
    [ -n "$win" ] && break
    sleep 0.1
  done
  sleep 0.5
  [ -n "$win" ] && xdotool windowsize "$win" 800 400
  sleep 1.5
  kill $pid; wait $pid 2>/dev/null
  sleep 0.5
  if grep -q "800x400" "$OUT/host-resize.log"; then
    printf '%-10s swapchain recreated at 800x400\n' resize
  else
    printf '%-10s NO swapchain at 800x400 (window %s)\n' resize "${win:-not found}"
    FAILED=1
  fi
}

# the iPad's path: guest -> unix socket -> nm-relay <- host connecting in
t_relay() {
  pkill -x nm-host
  for p in $(pgrep -f "^$REPO/box/nm-relay -p 7399"); do kill $p; done
  sleep 0.3
  rm -rf "$OUT/frames-relay"; mkdir -p "$OUT/frames-relay"
  rm -f "$OUT/telemetry.log"
  NM_TOKEN=test-token setsid "$REPO/box/nm-relay" -p 7399 -s /tmp/nm-test.sock -t "$OUT/telemetry.log" \
    >"$OUT/relay.log" 2>&1 </dev/null &
  sleep 0.3
  NM_DUMP_DIR="$OUT/frames-relay" VK_DRIVER_FILES=$LVP \
    setsid "$REPO/host/nm-host" relay:127.0.0.1:7399:test-token \
    >"$OUT/host-relay.log" 2>&1 </dev/null &
  sleep 0.5
  local t0=$(date +%s.%N)
  timeout 300 env VK_DRIVER_FILES=$VIRTIO VN_NET=unix:/tmp/nm-test.sock \
    vkcube --c 300 >"$OUT/guest-relay.log" 2>&1
  local rc=$?
  local t1=$(date +%s.%N)
  sleep 0.5
  local frames=$(grep -o "destroyed after [0-9]* frames" "$OUT/host-relay.log" | tail -1)
  local tel=$(grep -c "LOG" "$OUT/telemetry.log" 2>/dev/null)
  printf '%-10s rc=%-3s %6.2fs  %s  telemetry lines: %s\n' relay "$rc" "$(echo "$t1 - $t0" | bc)" "${frames:-no swapchain}" "${tel:-0}"
  for p in $(pgrep -f "^$REPO/box/nm-relay -p 7399"); do kill $p; done
  [ "$rc" = 0 ] && [ -n "$frames" ] && [ "${tel:-0}" -gt 0 ] || FAILED=1
}

tests=("$@")
[ ${#tests[@]} = 0 ] || [ "${tests[0]}" = all ] && tests=(vulkaninfo vkcube resize d3d11 d3d12 relay)
for t in "${tests[@]}"; do "t_$t"; done
pkill -x nm-host
exit $FAILED

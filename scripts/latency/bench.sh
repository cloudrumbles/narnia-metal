#!/bin/bash
# Time one app through nm-host in the latency namespace (scripts/latency/netns.sh).
#
#   scripts/latency/bench.sh <vulkaninfo|vkcube|d3d11|d3d12> <one-way ms>
#
#   FRAMES=300        frames to render (1 = startup only)
#   MESA_BUILD=...    Mesa build dir (default ~/src-latency/mesa/build)
#   ICD=...           guest ICD json (default: that build's devenv ICD)
#   HOST_BIN=...      nm-host (default <repo>/host/nm-host)
#   LVP_ICD=...       host's lavapipe ICD json
#   PFX=...           Wine prefix for the Proton apps (default ~/pfx-lat)
#   PROTON=...        proton dir (default ~/proton/GE-Proton11-12-x86_64)
#   NM_RT_STATS=1     also print the round-trip counters of each guest process
#                     at exit (NM_RT_STATS_FILE=path to append them to a file)
#   NM_RT_TRACE=1     print every blocking wait: pid, ms, tag, duration
#
# Prints: app, delay, exit code, wall time, frames the host saw.
app=$1; ms=$2
REPO=$(cd "$(dirname "$0")/../.." && pwd)
MESA_BUILD=${MESA_BUILD:-$HOME/src-latency/mesa/build}
ICD=${ICD:-$MESA_BUILD/src/virtio/vulkan/virtio_devenv_icd.x86_64.json}
LVP=${LVP_ICD:-/opt/mesa/share/vulkan/icd.d/lvp_icd.x86_64.json}
HOST=${HOST_BIN:-$REPO/host/nm-host}
PORT=${PORT:-7510}
OUT=${OUT:-$HOME/lat-out}; mkdir -p "$OUT"
PFX=${PFX:-$HOME/pfx-lat}; mkdir -p "$PFX" "$HOME/fakesteam"
PROTON=${PROTON:-$HOME/proton/GE-Proton11-12-x86_64}
FRAMES=${FRAMES:-300}

"$REPO/scripts/latency/netns.sh" "$ms"
pgrep -x Xvfb >/dev/null || { setsid Xvfb :9 -screen 0 1280x720x24 >/dev/null 2>&1 </dev/null & sleep 1; }
export DISPLAY=:9
sudo ip netns exec nmlat env VK_DRIVER_FILES=$LVP setsid "$HOST" "tcp:$PORT" >"$OUT/host-$app.log" 2>&1 </dev/null &
HOSTPID=$!
sleep 0.7
G=(env VK_DRIVER_FILES=$ICD VN_NET=10.77.0.2:$PORT)
PR=(env STEAM_COMPAT_DATA_PATH=$PFX STEAM_COMPAT_CLIENT_INSTALL_PATH=$HOME/fakesteam "$PROTON/proton" run)
case $app in
  vulkaninfo) cmd=("${G[@]}" vulkaninfo --summary);;
  vkcube) cmd=("${G[@]}" vkcube --c "$FRAMES");;
  d3d11) cmd=("${G[@]}" "${PR[@]}" "$REPO/tests/d3d11-spin/d3d11spin.exe" "$FRAMES" 640 480);;
  d3d12) cmd=("${G[@]}" "${PR[@]}" "$REPO/tests/d3d12-spin/d3d12spin.exe" "$FRAMES" 640 480);;
  *) echo "unknown app $app"; exit 2;;
esac
t0=$(date +%s.%N)
timeout 600 "${cmd[@]}" >"$OUT/guest-$app.log" 2>&1; rc=$?
t1=$(date +%s.%N)
printf '%s ow=%sms rc=%s %.2fs  %s\n' "$app" "$ms" "$rc" "$(echo "$t1-$t0" | bc)" \
  "$(grep -o 'destroyed after [0-9]* frames' "$OUT/host-$app.log" | tail -1)"
# the namespace's nm-host runs under sudo; kill it by pid
for p in $(pgrep -f "^$HOST tcp:$PORT"); do sudo kill "$p"; done

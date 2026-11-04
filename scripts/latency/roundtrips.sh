#!/bin/bash
# Count the blocking round trips an app makes, by Venus command.
#
#   scripts/latency/roundtrips.sh <app> <one-way ms>     (FRAMES=1 for startup)
#
# Runs bench.sh with NM_RT_STATS, then sums the per-process counters. "<renderer
# wait>" is vkWaitForFences etc. (the guest waiting for a FENCE_RETIRED),
# "<roundtrip-async>" is only counted, it does not block the guest.
REPO=$(cd "$(dirname "$0")/../.." && pwd)
MESA_SRC=${MESA_SRC:-$HOME/src-latency/mesa}
OUT=${OUT:-$HOME/lat-out}; mkdir -p "$OUT"
rm -f "$OUT/rt-$1.txt"
NM_RT_STATS=1 NM_RT_STATS_FILE="$OUT/rt-$1.txt" "$REPO/scripts/latency/bench.sh" "$1" "$2"
python3 - "$MESA_SRC/src/virtio/venus-protocol/vn_protocol_driver_defines.h" "$OUT/rt-$1.txt" <<'PY'
import collections, os, re, sys
names = {}
for l in open(sys.argv[1]):
    m = re.match(r'\s*VK_COMMAND_TYPE_(\w+?)_EXT = (\d+)', l)
    if m:
        names[int(m.group(2))] = m.group(1)
names.update({8000: '<roundtrip-async>', 8001: '<wait_all>', 8002: '<renderer wait>', 8003: '<other>'})
agg = collections.defaultdict(lambda: [0, 0.0])
for l in open(sys.argv[2]):
    m = re.match(r'NMRT pid=(\d+) tag=(\d+) calls=(\d+) waits=(\d+) ms=([\d.]+)', l)
    if m:
        t = int(m.group(2))
        agg[t][0] += int(m.group(4))
        agg[t][1] += float(m.group(5))
for t, (w, ms) in sorted(agg.items(), key=lambda x: -x[1][1])[:int(os.environ.get('TOP', '20'))]:
    print(f'{w:6d} {ms:9.1f}ms {names.get(t, t)}')
blocking = [v for k, v in agg.items() if k != 8000]
print('blocking waits', sum(v[0] for v in blocking), ' total ms', round(sum(v[1] for v in blocking)))
PY

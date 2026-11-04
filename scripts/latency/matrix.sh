#!/bin/bash
# All apps at 0 / 20 / 100 ms RTT. Prints "frames=N app ow=..." lines (also to
# $MATRIX_OUT). Takes a few minutes; do not run other tests meanwhile.
REPO=$(cd "$(dirname "$0")/../.." && pwd)
OUTF=${MATRIX_OUT:-$HOME/lat-out/matrix.txt}; mkdir -p "$(dirname "$OUTF")"
: > "$OUTF"
for ow in 0 10 50; do
  for spec in "vulkaninfo 1" "vkcube 1" "vkcube 300" "d3d11 1" "d3d11 300" "d3d12 1" "d3d12 300"; do
    set -- $spec
    FRAMES=$2 "$REPO/scripts/latency/bench.sh" "$1" "$ow" 2>&1 | sed "s/^/frames=$2 /" | tee -a "$OUTF"
  done
done
"$REPO/scripts/latency/netns.sh" 0

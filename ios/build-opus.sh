#!/bin/bash
# Build libopus (floating point, no SIMD, no config.h) as a static library
# with plain clang/gcc, the same way vkr/build-vkr.sh does vkr.
#
#   ios/build-opus.sh <opus src> <out dir>
#   env: CC (default cc), CFLAGS (extra, e.g. -arch arm64 -isysroot ...), AR
#
# Produces <out>/libopus.a; headers are used from <opus src>/include.
set -euo pipefail

SRC=$(cd "$1" && pwd)
OUT=$(mkdir -p "$2" && cd "$2" && pwd)
CC=${CC:-cc}
AR=${AR:-ar}

mkdir -p "$OUT/obj"
FLAGS=(
  -std=gnu99 -O2 -g -fPIC -fvisibility=hidden -w
  -DOPUS_BUILD -DUSE_ALLOCA -DHAVE_LRINTF -DHAVE_LRINT -DFLOATING_POINT
  -DPACKAGE_VERSION='"1.5.2"'
  -I"$SRC/include" -I"$SRC/celt" -I"$SRC/silk" -I"$SRC/silk/float" -I"$SRC"
)

SRCS=()
for f in "$SRC"/celt/*.c "$SRC"/silk/*.c "$SRC"/silk/float/*.c "$SRC"/src/*.c; do
  case $(basename "$f") in
    opus_custom_demo.c|opus_demo.c|opus_compare.c|repacketizer_demo.c) continue ;;
  esac
  SRCS+=("$f")
done

objs=()
pids=()
for f in "${SRCS[@]}"; do
  rel=${f#"$SRC"/}
  obj="$OUT/obj/$(echo "${rel%.c}" | tr / _).o"
  objs+=("$obj")
  # shellcheck disable=SC2086
  $CC "${FLAGS[@]}" ${CFLAGS:-} -c "$f" -o "$obj" &
  pids+=($!)
  # keep the process count sane on small runners
  while [ "$(jobs -rp | wc -l)" -ge 8 ]; do sleep 0.05; done
done
fail=0
for p in "${pids[@]}"; do wait "$p" || fail=1; done
[ $fail = 0 ] || { echo "build-opus: compile failed" >&2; exit 1; }

rm -f "$OUT/libopus.a"
$AR rcs "$OUT/libopus.a" "${objs[@]}"
echo "built $OUT/libopus.a (${#objs[@]} objects)"

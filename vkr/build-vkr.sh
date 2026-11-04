#!/bin/bash
# Build vkr (virglrenderer's Venus renderer) as a static library without
# meson, from an already patched virglrenderer checkout.
#
#   vkr/build-vkr.sh <virglrenderer src> <out dir>
#   env: CC (default cc), CFLAGS (extra), AR (default ar)
#
# Produces <out>/libvkr.a; headers are used straight from the source tree.
set -euo pipefail

VR=$(cd "$1" && pwd)
OUT=$(mkdir -p "$2" && cd "$2" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
CC=${CC:-cc}
AR=${AR:-ar}

mkdir -p "$OUT/gen" "$OUT/obj"
python3 "$VR/src/venus/vkr_device_object.py" -o "$OUT/gen" \
  "$VR/src/venus/vkr_device_object.json"

SRCS=(
  src/venus/vkr_acceleration_structure.c src/venus/vkr_allocator.c
  src/venus/vkr_buffer.c src/venus/vkr_command_buffer.c src/venus/vkr_common.c
  src/venus/vkr_context.c src/venus/vkr_cs.c src/venus/vkr_descriptor_set.c
  src/venus/vkr_device.c src/venus/vkr_device_memory.c src/venus/vkr_host_copy.c
  src/venus/vkr_image.c src/venus/vkr_instance.c src/venus/vkr_library.c
  src/venus/vkr_physical_device.c src/venus/vkr_pipeline.c
  src/venus/vkr_query_pool.c src/venus/vkr_queue.c src/venus/vkr_render_pass.c
  src/venus/vkr_renderer.c src/venus/vkr_ring.c src/venus/vkr_transport.c
  src/virgl_util.c
  src/mesa/util/anon_file.c src/mesa/util/bitscan.c src/mesa/util/hash_table.c
  src/mesa/util/os_file.c src/mesa/util/os_misc.c src/mesa/util/ralloc.c
  src/mesa/util/u_debug.c src/mesa/util/u_math.c
)

FLAGS=(
  -std=gnu11 -O2 -g -fPIC -pthread -fvisibility=hidden -fno-strict-aliasing
  -Wno-unused-parameter -Wno-missing-field-initializers
  -DHAVE_CONFIG_H=1 -imacros "$HERE/config.h"
  -I"$HERE" -I"$OUT/gen" -I"$VR/src" -I"$VR/src/venus" -I"$VR/src/mesa"
  -I"$VR/src/mesa/util" -I"$VR/src/mesa/compat" -I"$VR/src/mesa/pipe"
  -I"$VR/src/gallium/include" -I"$VR/src/gallium/auxiliary"
  -I"$VR/src/gallium/auxiliary/util" -I"$VR/src/drm" -I"$VR/src/drm/drm-uapi"
)

objs=()
pids=()
for src in "${SRCS[@]}" shim; do
  if [ "$src" = shim ]; then path="$HERE/shim.c"; else path="$VR/$src"; fi
  obj="$OUT/obj/$(echo "${src%.c}" | tr / _).o"
  objs+=("$obj")
  # shellcheck disable=SC2086
  $CC "${FLAGS[@]}" ${CFLAGS:-} -c "$path" -o "$obj" &
  pids+=($!)
done
fail=0
for p in "${pids[@]}"; do wait "$p" || fail=1; done
[ $fail = 0 ] || { echo "build-vkr: compile failed" >&2; exit 1; }

rm -f "$OUT/libvkr.a"
$AR rcs "$OUT/libvkr.a" "${objs[@]}"
echo "build-vkr: $OUT/libvkr.a"

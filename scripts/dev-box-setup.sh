#!/bin/bash
# Set up a Debian 13 build box for narnia-metal: build deps, then Mesa (venus
# with the net backend + lavapipe) and virglrenderer (venus) from the pinned
# tags with our patches applied.
#
# Usage, on the box, from a checkout of this repo:
#   scripts/dev-box-setup.sh deps     # apt packages (needs sudo)
#   scripts/dev-box-setup.sh fetch    # clone mesa + virglrenderer into ~/src
#   scripts/dev-box-setup.sh mesa     # patch, build, install to /opt/mesa
#   scripts/dev-box-setup.sh vr       # patch, build, install to /opt/vr
#   scripts/dev-box-setup.sh host     # build the linux test host
#   scripts/dev-box-setup.sh all
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
SRC=${SRC:-$HOME/src}
MESA_TAG=mesa-26.2.6
VR_TAG=virglrenderer-1.3.0
JOBS=$(nproc)

deps() {
  sudo sed -i 's/^Types: deb$/Types: deb deb-src/' /etc/apt/sources.list.d/debian.sources 2>/dev/null || true
  sudo apt-get update
  sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
    build-essential git meson ninja-build pkg-config cmake clang llvm-dev \
    libclang-dev libclang-cpp-dev python3-mako python3-yaml python3-packaging \
    python3-ply bison flex glslang-tools libelf-dev libexpat1-dev zlib1g-dev \
    libzstd-dev libdrm-dev libx11-dev libx11-xcb-dev libxext-dev libxfixes-dev \
    libxcb1-dev libxcb-dri3-dev libxcb-present-dev libxcb-randr0-dev \
    libxcb-shm0-dev libxcb-sync-dev libxcb-xfixes0-dev libxcb-keysyms1-dev \
    libxshmfence-dev libxrandr-dev libxxf86vm-dev libwayland-dev \
    wayland-protocols libwayland-egl-backend-dev libvulkan-dev vulkan-tools \
    mesa-vulkan-drivers libepoxy-dev libgbm-dev xvfb spirv-tools \
    libllvmspirvlib-19-dev libclc-19-dev rsync
}

fetch() {
  mkdir -p "$SRC"
  [ -d "$SRC/mesa" ] || git clone --depth 1 -b "$MESA_TAG" https://gitlab.freedesktop.org/mesa/mesa.git "$SRC/mesa"
  [ -d "$SRC/virglrenderer" ] || git clone --depth 1 -b "$VR_TAG" https://gitlab.freedesktop.org/virgl/virglrenderer.git "$SRC/virglrenderer"
}

# apply_tree <upstream checkout> <overlay dir in this repo>
# Resets the checkout, copies overlay files in, applies patches/*.patch.
apply_tree() {
  local tree=$1 overlay=$2
  git -C "$tree" checkout -q -- .
  git -C "$tree" clean -qfd -e build
  if [ -d "$overlay/files" ]; then
    cp -r "$overlay/files/." "$tree/"
  fi
  for p in "$overlay"/patches/*.patch; do
    [ -e "$p" ] || continue
    git -C "$tree" apply --whitespace=nowarn "$p"
  done
}

mesa() {
  apply_tree "$SRC/mesa" "$REPO/mesa"
  cp "$REPO/common/nm_proto.h" "$SRC/mesa/src/virtio/vulkan/"
  cd "$SRC/mesa"
  [ -d build ] || meson setup build --prefix=/opt/mesa -Dbuildtype=debugoptimized \
    -Dvulkan-drivers=virtio,swrast -Dgallium-drivers=llvmpipe -Dplatforms=x11,wayland \
    -Dglx=disabled -Degl=disabled -Dgles1=disabled -Dgles2=disabled -Dopengl=false \
    -Dllvm=enabled -Dvideo-codecs=
  ninja -C build -j"$JOBS"
  sudo ninja -C build install >/dev/null
}

vr() {
  apply_tree "$SRC/virglrenderer" "$REPO/virglrenderer"
  cd "$SRC/virglrenderer"
  [ -d build ] || meson setup build --prefix=/opt/vr -Dbuildtype=debugoptimized \
    -Dvenus=true -Dplatforms=
  ninja -C build -j"$JOBS"
  sudo ninja -C build install >/dev/null
}

host() {
  make -C "$REPO/host" -j"$JOBS" VR_SRC="$SRC/virglrenderer"
  make -C "$REPO/box"
}

case ${1:-all} in
  deps) deps ;;
  fetch) fetch ;;
  mesa) mesa ;;
  vr) vr ;;
  host) host ;;
  all) deps; fetch; mesa; vr; host ;;
  *) echo "unknown step $1" >&2; exit 1 ;;
esac

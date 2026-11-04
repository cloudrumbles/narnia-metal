#!/bin/bash
# Fetch what the Apple builds need into $DEPS (default ios/deps):
#   virglrenderer (pinned tag, our patches applied), MoltenVK (built
#   from source), Vulkan-Headers.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
DEPS=${DEPS:-$REPO/ios/deps}
VR_TAG=virglrenderer-1.3.0
VKH_TAG=${VKH_TAG:-vulkan-sdk-1.4.363.0}
mkdir -p "$DEPS"

if [ ! -d "$DEPS/virglrenderer" ]; then
  git clone -q --depth 1 -b "$VR_TAG" https://gitlab.freedesktop.org/virgl/virglrenderer.git "$DEPS/virglrenderer"
  for p in "$REPO"/virglrenderer/patches/*.patch; do
    git -C "$DEPS/virglrenderer" apply --whitespace=nowarn "$p"
  done
fi

if [ ! -d "$DEPS/Vulkan-Headers" ]; then
  git clone -q --depth 1 -b "$VKH_TAG" https://github.com/KhronosGroup/Vulkan-Headers.git "$DEPS/Vulkan-Headers" ||
    git clone -q --depth 1 https://github.com/KhronosGroup/Vulkan-Headers.git "$DEPS/Vulkan-Headers"
fi

# MoltenVK is built from source (geometry shaders, robustness2): slow, so CI
# caches ios/deps/moltenvk-* keyed on moltenvk/ and ios/build-moltenvk.sh.
if [ ! -d "$DEPS/moltenvk-ios" ] || [ ! -d "$DEPS/moltenvk-macos" ]; then
  "$REPO/ios/build-moltenvk.sh"
fi
echo "deps: virglrenderer $VR_TAG, MoltenVK $(cat "$DEPS/moltenvk-version" 2>/dev/null), Vulkan-Headers $(git -C "$DEPS/Vulkan-Headers" describe --tags 2>/dev/null || echo ?)"

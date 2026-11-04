#!/bin/bash
# Fetch what the Apple builds need into $DEPS (default ios/deps):
#   virglrenderer (pinned tag, our patches applied), MoltenVK (release
#   tarballs), Vulkan-Headers, libopus (pinned tag).
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
DEPS=${DEPS:-$REPO/ios/deps}
VR_TAG=virglrenderer-1.3.0
MVK_TAG=${MVK_TAG:-v1.4.2}
VKH_TAG=${VKH_TAG:-vulkan-sdk-1.4.363.0}
OPUS_TAG=${OPUS_TAG:-v1.5.2}
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

if [ ! -d "$DEPS/opus" ]; then
  git clone -q --depth 1 -b "$OPUS_TAG" https://github.com/xiph/opus.git "$DEPS/opus"
fi

for plat in ios macos; do
  if [ ! -d "$DEPS/moltenvk-$plat" ]; then
    mkdir -p "$DEPS/moltenvk-$plat"
    curl -fsSL "https://github.com/KhronosGroup/MoltenVK/releases/download/$MVK_TAG/MoltenVK-$plat.tar" |
      tar x -C "$DEPS/moltenvk-$plat"
  fi
done
echo "$MVK_TAG" > "$DEPS/moltenvk-version"
echo "deps: virglrenderer $VR_TAG, MoltenVK $MVK_TAG, Vulkan-Headers $(git -C "$DEPS/Vulkan-Headers" describe --tags 2>/dev/null || echo ?)"

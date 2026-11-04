#!/bin/bash
# Fetch what the Apple builds need into $DEPS (default ios/deps):
#   virglrenderer (pinned tag, our patches applied), MoltenVK (release
#   tarballs), Vulkan-Headers.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
DEPS=${DEPS:-$REPO/ios/deps}
VR_TAG=virglrenderer-1.3.0
MVK_TAG=${MVK_TAG:-$(curl -fsSL https://api.github.com/repos/KhronosGroup/MoltenVK/releases/latest | python3 -c 'import json,sys; print(json.load(sys.stdin)["tag_name"])')}
VKH_TAG=${VKH_TAG:-v1.4.330}
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

for plat in ios macos; do
  if [ ! -d "$DEPS/moltenvk-$plat" ]; then
    mkdir -p "$DEPS/moltenvk-$plat"
    curl -fsSL "https://github.com/KhronosGroup/MoltenVK/releases/download/$MVK_TAG/MoltenVK-$plat.tar" |
      tar x -C "$DEPS/moltenvk-$plat"
  fi
done
echo "$MVK_TAG" > "$DEPS/moltenvk-version"
echo "deps: virglrenderer $VR_TAG, MoltenVK $MVK_TAG, Vulkan-Headers $(git -C "$DEPS/Vulkan-Headers" describe --tags 2>/dev/null || echo ?)"

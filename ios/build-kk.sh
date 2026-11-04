#!/bin/bash
# Build KosmicKrisp (Mesa's Vulkan-on-Metal driver, src/kosmickrisp) as a
# dylib for the Apple hosts. Run on macOS with Xcode 26 (Metal 4 SDKs).
#
#   ios/build-kk.sh tools   -> build-machine tools in $OUT/kk-tools/bin
#   ios/build-kk.sh macos   -> tools + $OUT/kk-macos/libvulkan_kosmickrisp.dylib
#   ios/build-kk.sh ios     -> $OUT/kk-ios/libvulkan_kosmickrisp.dylib
#                              (runs the tools from the tools/macos step)
#
# GitHub's macOS runners have a paravirtual GPU without Metal 4, so the macOS
# driver is only useful on a real Mac; CI builds tools + ios.
#
# Like every Mesa driver with OpenCL C helper kernels, KosmicKrisp needs three
# compilers that run on the build machine: mesa_clc (OpenCL C -> SPIR-V, needs
# LLVM/clang + SPIRV-LLVM-Translator + SPIRV-Tools), vtn_bindgen2 and kk_clc
# (SPIR-V -> precompiled MSL). The macos step builds them natively together
# with the macOS driver; the iOS step cross-compiles with
# -Dmesa-clc=system -Dprecomp-compiler=system. (Same structure as Madeira's
# build-kosmickrisp.sh; this script is our own.)
#
# Mesa commit: kosmickrisp/PIN. Patches: kosmickrisp/patches/*.patch.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
DEPS=${DEPS:-$REPO/ios/deps}
OUT=${OUT:-$REPO/ios/out}
TARGET=${1:-macos}
PIN=$(cat "$REPO/kosmickrisp/PIN")
MESA="$DEPS/mesa-kk"
JOBS=$(sysctl -n hw.ncpu)

tools() {
  brew install -q llvm spirv-llvm-translator spirv-tools bison flex pkg-config >/dev/null
  LLVM=$(brew --prefix llvm)
  export PATH="$LLVM/bin:$(brew --prefix bison)/bin:$(brew --prefix flex)/bin:$PATH"
  export PKG_CONFIG_PATH="$(brew --prefix spirv-tools)/lib/pkgconfig:$(brew --prefix spirv-llvm-translator)/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
  if [ ! -x "$DEPS/venv/bin/meson" ]; then
    python3 -m venv "$DEPS/venv"
    "$DEPS/venv/bin/pip" install -q mako packaging pyyaml 'meson>=1.9.1' ninja
  fi
  export PATH="$DEPS/venv/bin:$PATH"
  echo "llvm $(llvm-config --version), meson $(meson --version)"
}

fetch() {
  if [ ! -d "$MESA/.git" ] || [ "$(git -C "$MESA" rev-parse HEAD)" != "$PIN" ]; then
    rm -rf "$MESA"
    git init -q "$MESA"
    git -C "$MESA" fetch -q --depth 1 https://gitlab.freedesktop.org/mesa/mesa.git "$PIN"
    git -C "$MESA" checkout -q FETCH_HEAD
  fi
  git -C "$MESA" checkout -q -- . && git -C "$MESA" clean -qfd
  for p in "$REPO"/kosmickrisp/patches/*.patch; do
    git -C "$MESA" apply --whitespace=nowarn "$p"
  done
}

# options shared by both builds
OPTS=(
  --buildtype=release -Db_ndebug=true
  -Dvulkan-drivers=kosmickrisp -Dgallium-drivers= -Dopengl=false
  -Dgles1=disabled -Dgles2=disabled -Dglx=disabled -Degl=disabled
  -Dgbm=disabled -Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled
  -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled
  -Dbuild-tests=false -Dvideo-codecs= -Dshader-cache=disabled
)

build_native() {
  local with_driver=$1
  local b="$DEPS/build-kk-macos"
  printf "[binaries]\nllvm-config = '%s'\n" "$(command -v llvm-config)" > "$DEPS/native-llvm.ini"
  [ -f "$b/build.ninja" ] || meson setup "$b" "$MESA" --native-file "$DEPS/native-llvm.ini" \
    "${OPTS[@]}" -Dplatforms=macos \
    -Dllvm=enabled -Dshared-llvm=enabled -Dmesa-clc=enabled -Dprecomp-compiler=enabled \
    -Dmesa-clc-bundle-headers=enabled
  ninja -C "$b" -j"$JOBS" src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2 \
    src/kosmickrisp/clc/kk_clc
  mkdir -p "$OUT/kk-tools/bin"
  cp "$b/src/compiler/clc/mesa_clc" "$b/src/compiler/spirv/vtn_bindgen2" \
     "$b/src/kosmickrisp/clc/kk_clc" "$OUT/kk-tools/bin/"
  ls -la "$OUT/kk-tools/bin"
  if [ "$with_driver" = yes ]; then
    ninja -C "$b" -j"$JOBS" src/kosmickrisp/vulkan/libvulkan_kosmickrisp.dylib
    mkdir -p "$OUT/kk-macos"
    cp "$b/src/kosmickrisp/vulkan/libvulkan_kosmickrisp.dylib" "$OUT/kk-macos/"
    install_name_tool -id @rpath/libvulkan_kosmickrisp.dylib "$OUT/kk-macos/libvulkan_kosmickrisp.dylib"
  fi
}

build_ios() {
  local sdk b="$DEPS/build-kk-ios"
  sdk=$(xcrun --sdk iphoneos --show-sdk-path)
  mkdir -p "$b"
  local flags="'-arch', 'arm64', '-isysroot', '$sdk', '-miphoneos-version-min=26.0'"
  cat > "$DEPS/ios-arm64.cross" <<EOT
[binaries]
c = ['xcrun', '-sdk', 'iphoneos', 'clang']
cpp = ['xcrun', '-sdk', 'iphoneos', 'clang++']
objc = ['xcrun', '-sdk', 'iphoneos', 'clang']
ar = ['xcrun', '-sdk', 'iphoneos', 'ar']
strip = ['xcrun', '-sdk', 'iphoneos', 'strip']
pkg-config = 'false'

[built-in options]
c_args = [$flags]
cpp_args = [$flags]
objc_args = [$flags]
c_link_args = [$flags]
cpp_link_args = [$flags]
objc_link_args = [$flags]

[host_machine]
system = 'darwin'
subsystem = 'ios'
kernel = 'xnu'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
EOT
  local t="$OUT/kk-tools/bin"
  printf "[binaries]\nmesa_clc = '%s'\nvtn_bindgen2 = '%s'\nkk_clc = '%s'\n" \
    "$t/mesa_clc" "$t/vtn_bindgen2" "$t/kk_clc" > "$DEPS/native-tools.ini"
  [ -f "$b/build.ninja" ] || meson setup "$b" "$MESA" --cross-file "$DEPS/ios-arm64.cross" \
    --native-file "$DEPS/native-tools.ini" "${OPTS[@]}" --default-library=shared \
    -Dplatforms=macos -Dllvm=disabled -Dmesa-clc=system -Dprecomp-compiler=system
  ninja -C "$b" -j"$JOBS" src/kosmickrisp/vulkan/libvulkan_kosmickrisp.dylib
  mkdir -p "$OUT/kk-ios"
  cp "$b/src/kosmickrisp/vulkan/libvulkan_kosmickrisp.dylib" "$OUT/kk-ios/"
  install_name_tool -id @rpath/libvulkan_kosmickrisp.dylib "$OUT/kk-ios/libvulkan_kosmickrisp.dylib"
  xcrun -sdk iphoneos strip -x "$OUT/kk-ios/libvulkan_kosmickrisp.dylib"
  ls -la "$OUT/kk-ios"
}

tools
fetch
case $TARGET in
  tools) build_native no ;;
  macos) build_native yes ;;
  ios) build_ios ;;
  *) echo "usage: $0 tools|macos|ios" >&2; exit 1 ;;
esac

#!/bin/bash
# Build the Apple targets (run on macOS after ios/fetch-deps.sh):
#   ios/build.sh ios     -> $OUT/NarniaMetal.ipa (unsigned, for SideStore/AltStore)
#   ios/build.sh macos   -> $OUT/nm-host-macos (CLI host on MoltenVK, for CI)
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
DEPS=${DEPS:-$REPO/ios/deps}
OUT=${OUT:-$REPO/ios/out}
TARGET=${1:-ios}
VR="$DEPS/virglrenderer"
VKH="$DEPS/Vulkan-Headers/include"
VERSION=${VERSION:-0.1.0}
BUILD=${BUILD:-1}
mkdir -p "$OUT"

case $TARGET in
  ios)
    SDK=iphoneos
    ARCH_FLAGS="-arch arm64 -miphoneos-version-min=16.0"
    MVK_LIB=$(find "$DEPS/moltenvk-ios" -path '*static/MoltenVK.xcframework/ios-arm64/*' -name libMoltenVK.a | head -1)
    FRAMEWORKS="-framework UIKit -framework Foundation -framework QuartzCore -framework Metal -framework IOSurface -framework CoreGraphics"
    ;;
  macos)
    SDK=macosx
    ARCH_FLAGS="-arch arm64 -mmacosx-version-min=13.0"
    MVK_LIB=$(find "$DEPS/moltenvk-macos" -path '*static/MoltenVK.xcframework/macos-*' -name libMoltenVK.a | head -1)
    FRAMEWORKS="-framework AppKit -framework Foundation -framework QuartzCore -framework Metal -framework IOSurface -framework IOKit -framework CoreGraphics"
    ;;
  *) echo "usage: $0 ios|macos" >&2; exit 1 ;;
esac
[ -n "$MVK_LIB" ] || { echo "MoltenVK static lib not found under $DEPS" >&2; exit 1; }

SYSROOT=$(xcrun --sdk $SDK --show-sdk-path)
CC="xcrun --sdk $SDK clang"
CXX="xcrun --sdk $SDK clang++"
COMMON="$ARCH_FLAGS -isysroot $SYSROOT"

echo "== vkr ($TARGET)"
CC="$CC" CFLAGS="$COMMON" AR="xcrun --sdk $SDK ar" \
  "$REPO/vkr/build-vkr.sh" "$VR" "$OUT/vkr-$TARGET"

OBJ="$OUT/obj-$TARGET"
mkdir -p "$OBJ"
CFLAGS=(
  $COMMON -std=gnu11 -O2 -g -Wall -Wno-unused-parameter -pthread
  -I"$REPO/common" -I"$REPO/host" -I"$REPO/ios/app" -I"$REPO/vkr"
  -I"$VR/src" -I"$VR/src/venus" -I"$VR/src/mesa" -I"$VR/src/mesa/util"
  -I"$VKH"
)
SRCS=("$REPO/host/nm_host.c" "$REPO/host/nm_present.c"
      "$REPO/host/nm_relay_client.c" "$REPO/host/nm_driver_moltenvk.c")
if [ "$TARGET" = ios ]; then
  SRCS+=("$REPO/ios/app/nm_presenter_metal.c" "$REPO/ios/app/main.m")
else
  SRCS+=("$REPO/host/nm_presenter_png.c" "$REPO/host/main_cli.c")
fi

echo "== host + app"
objs=()
for src in "${SRCS[@]}"; do
  o="$OBJ/$(basename "${src%.*}").o"
  case $src in
    *.m) $CC $COMMON -fobjc-arc -O2 -g -Wall -I"$REPO/common" -I"$REPO/host" \
           -I"$REPO/ios/app" -I"$VKH" -c "$src" -o "$o" ;;
    *)   $CC "${CFLAGS[@]}" -c "$src" -o "$o" ;;
  esac
  objs+=("$o")
done

echo "== link"
if [ "$TARGET" = ios ]; then
  APP="$OUT/Payload/NarniaMetal.app"
  rm -rf "$OUT/Payload"
  mkdir -p "$APP"
  $CXX $COMMON -o "$APP/NarniaMetal" "${objs[@]}" "$OUT/vkr-ios/libvkr.a" \
    "$MVK_LIB" $FRAMEWORKS -lz
  sed -e "s/__VERSION__/$VERSION/" -e "s/__BUILD__/$BUILD/" \
    "$REPO/ios/app/Info.plist" > "$APP/Info.plist"
  plutil -convert binary1 "$APP/Info.plist"
  printf 'APPL????' > "$APP/PkgInfo"
  # ad-hoc signature; SideStore/AltStore re-sign with your Apple ID
  codesign --force --sign - --timestamp=none "$APP" || true
  (cd "$OUT" && rm -f NarniaMetal.ipa && zip -qr NarniaMetal.ipa Payload)
  echo "built $OUT/NarniaMetal.ipa ($(du -h "$OUT/NarniaMetal.ipa" | cut -f1))"
else
  $CXX $COMMON -o "$OUT/nm-host-macos" "${objs[@]}" "$OUT/vkr-macos/libvkr.a" \
    "$MVK_LIB" $FRAMEWORKS -lz
  echo "built $OUT/nm-host-macos"
fi

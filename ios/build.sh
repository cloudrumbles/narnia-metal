#!/bin/bash
# Build the Apple targets (run on macOS after ios/fetch-deps.sh):
#   ios/build.sh ios     -> $OUT/NarniaMetal.ipa (unsigned, for SideStore/AltStore)
#   ios/build.sh macos   -> $OUT/nm-host-macos (CLI host, for CI)
#
# NM_DRIVER picks the host's Vulkan driver: moltenvk (linked statically) or
# kosmickrisp (libvulkan_kosmickrisp.dylib from ios/build-kk.sh, embedded and
# dlopen()ed).
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
DEPS=${DEPS:-$REPO/ios/deps}
OUT=${OUT:-$REPO/ios/out}
TARGET=${1:-ios}
NM_DRIVER=${NM_DRIVER:-kosmickrisp}
VR="$DEPS/virglrenderer"
VKH="$DEPS/Vulkan-Headers/include"
OPUS="$DEPS/opus"
VERSION=${VERSION:-0.1.0}
BUILD=${BUILD:-1}
mkdir -p "$OUT"

case $TARGET in
  ios)
    SDK=iphoneos
    ARCH_FLAGS="-arch arm64 -miphoneos-version-min=16.0"
    MVK_LIB=$(find "$DEPS/moltenvk-ios" -path '*static/MoltenVK.xcframework/ios-arm64/*' -name libMoltenVK.a | head -1)
    FRAMEWORKS="-framework UIKit -framework Foundation -framework QuartzCore -framework Metal -framework IOSurface -framework CoreGraphics -framework GameController -framework AVFoundation -framework AudioToolbox"
    ;;
  macos)
    SDK=macosx
    ARCH_FLAGS="-arch arm64 -mmacosx-version-min=13.0"
    MVK_LIB=$(find "$DEPS/moltenvk-macos" -path '*static/MoltenVK.xcframework/macos-*' -name libMoltenVK.a | head -1)
    FRAMEWORKS="-framework AppKit -framework Foundation -framework QuartzCore -framework Metal -framework IOSurface -framework IOKit -framework CoreGraphics"
    ;;
  *) echo "usage: $0 ios|macos" >&2; exit 1 ;;
esac
if [ "$NM_DRIVER" = kosmickrisp ]; then
  KK_DYLIB="$OUT/kk-$TARGET/libvulkan_kosmickrisp.dylib"
  [ -f "$KK_DYLIB" ] || { echo "run ios/build-kk.sh $TARGET first" >&2; exit 1; }
  MVK_LIB=""
  FRAMEWORKS="$FRAMEWORKS -rpath @executable_path/Frameworks -rpath @executable_path"
else
  [ -n "$MVK_LIB" ] || { echo "MoltenVK static lib not found under $DEPS" >&2; exit 1; }
fi

SYSROOT=$(xcrun --sdk $SDK --show-sdk-path)
CC="xcrun --sdk $SDK clang"
CXX="xcrun --sdk $SDK clang++"
COMMON="$ARCH_FLAGS -isysroot $SYSROOT"

echo "== vkr ($TARGET)"
CC="$CC" CFLAGS="$COMMON" AR="xcrun --sdk $SDK ar" \
  "$REPO/vkr/build-vkr.sh" "$VR" "$OUT/vkr-$TARGET"

echo "== opus ($TARGET)"
CC="$CC" CFLAGS="$COMMON" AR="xcrun --sdk $SDK ar" \
  "$REPO/ios/build-opus.sh" "$OPUS" "$OUT/opus-$TARGET"

OBJ="$OUT/obj-$TARGET"
mkdir -p "$OBJ"
CFLAGS=(
  $COMMON -std=gnu11 -O2 -g -Wall -Wno-unused-parameter -pthread
  -I"$REPO/common" -I"$REPO/host" -I"$REPO/ios/app" -I"$REPO/vkr"
  -I"$VR/src" -I"$VR/src/venus" -I"$VR/src/mesa" -I"$VR/src/mesa/util"
  -I"$VKH" -I"$OPUS/include"
)
SRCS=("$REPO/host/nm_host.c" "$REPO/host/nm_present.c"
      "$REPO/host/nm_audio.c" "$REPO/host/nm_audio_player.c"
      "$REPO/host/nm_relay_client.c" "$REPO/host/nm_driver_$NM_DRIVER.c")
if [ "$TARGET" = ios ]; then
  SRCS+=("$REPO/ios/app/nm_presenter_metal.c"
        "$REPO/ios/app/nm_audio_ios.m" "$REPO/ios/app/main.m")
else
  SRCS+=("$REPO/host/nm_presenter_png.c" "$REPO/host/nm_audio_sink_wav.c"
        "$REPO/host/main_cli.c")
fi

echo "== host + app"
objs=()
for src in "${SRCS[@]}"; do
  o="$OBJ/$(basename "${src%.*}").o"
  case $src in
    *.m) $CC $COMMON -fobjc-arc -O2 -g -Wall -I"$REPO/common" -I"$REPO/host" \
           -I"$REPO/ios/app" -I"$VKH" -I"$OPUS/include" -c "$src" -o "$o" ;;
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
    $MVK_LIB "$OUT/opus-$TARGET/libopus.a" $FRAMEWORKS -lz
  if [ "$NM_DRIVER" = kosmickrisp ]; then
    mkdir -p "$APP/Frameworks"
    cp "$KK_DYLIB" "$APP/Frameworks/"
    codesign --force --sign - --timestamp=none "$APP/Frameworks/libvulkan_kosmickrisp.dylib" || true
  fi
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
    $MVK_LIB "$OUT/opus-$TARGET/libopus.a" $FRAMEWORKS -lz
  [ "$NM_DRIVER" = kosmickrisp ] && cp "$KK_DYLIB" "$OUT/"
  echo "built $OUT/nm-host-macos"
fi

#!/bin/bash
# Build MoltenVK from source into $DEPS/moltenvk-{ios,macos}/MoltenVK/static/...
# (the same layout as the release tarballs). We build from source because
# stock MoltenVK has no geometry shaders and no robustness2; see
# docs/notes.md. Pinned commits live in moltenvk/PINS; our own changes are in
# moltenvk/patches (MoltenVK) and moltenvk/spirv-cross-patches (SPIRV-Cross).
#
# Two stages so CI can cache them separately:
#   deps  fetchDependencies + the external libraries (SPIRV-Cross, SPIRV-Tools,
#         ...); depends on moltenvk/PINS and moltenvk/spirv-cross-patches only.
#         Lives in $DEPS/MoltenVK-src/External and $DEPS/SPIRV-Cross-src.
#   lib   MoltenVK itself, with moltenvk/patches applied (a few minutes).
# No argument runs both; a stage whose output exists is skipped.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
DEPS=${DEPS:-$REPO/ios/deps}
PLATS=${PLATS:-"macos ios"}
STAGE=${1:-all}
# shellcheck disable=SC1091
. "$REPO/moltenvk/PINS"
SRC="$DEPS/MoltenVK-src"
SPX="$DEPS/SPIRV-Cross-src"
mkdir -p "$DEPS"

# check out a pinned commit into a directory that may already hold cached
# build products (so no plain `git clone`)
checkout() { # dir url rev
  mkdir -p "$1"
  [ -d "$1/.git" ] || { git -C "$1" init -q; git -C "$1" remote add origin "$2"; }
  git -C "$1" cat-file -e "$3^{commit}" 2>/dev/null || git -C "$1" fetch -q --depth 1 origin "$3"
  git -C "$1" checkout -q -f "$3"
}

stage_deps() {
  [ -e "$SRC/External/.nm-deps-done" ] && return 0
  echo "== MoltenVK dependencies"
  checkout "$SRC" "$MVK_URL" "$MVK_REV"
  # SPIRV-Cross: the revision the MoltenVK branch names is only reachable
  # through a tag on TimDarcet's fork, which fetchDependencies can't fetch, so
  # fetch it ourselves and hand it over with --spirv-cross-root.
  checkout "$SPX" "$SPX_URL" "$SPX_REV"
  git -C "$SPX" clean -qfdx
  for p in "$REPO"/moltenvk/spirv-cross-patches/*.patch; do
    [ -e "$p" ] && git -C "$SPX" apply --whitespace=nowarn "$p"
  done
  (
    cd "$SRC"
    ./fetchDependencies --macos --ios --spirv-cross-root "$SPX" >"$DEPS/fetchdeps.log" 2>&1 ||
      { tail -30 "$DEPS/fetchdeps.log" >&2; exit 1; }
  )
  touch "$SRC/External/.nm-deps-done"
}

stage_lib() {
  stage_deps
  checkout "$SRC" "$MVK_URL" "$MVK_REV"
  git -C "$SRC" clean -qfdx -e External
  for p in "$REPO"/moltenvk/patches/*.patch; do
    [ -e "$p" ] && git -C "$SRC" apply --whitespace=nowarn "$p"
  done
  (
    cd "$SRC"
    for plat in $PLATS; do
      echo "== MoltenVK $plat"
      case $plat in
        macos) scheme="MoltenVK Package (macOS only)"; dest="generic/platform=macOS" ;;
        ios)   scheme="MoltenVK Package (iOS only)";   dest="generic/platform=iOS" ;;
      esac
      # arm64 only (the Makefile builds universal binaries, twice the time)
      xcodebuild build -project MoltenVKPackaging.xcodeproj -scheme "$scheme" \
        -destination "$dest" ARCHS=arm64 ONLY_ACTIVE_ARCH=NO \
        GCC_PREPROCESSOR_DEFINITIONS='$(inherited) MVK_CONFIG_LOG_LEVEL=2' -quiet 2>&1 | tail -40
      rm -rf "$DEPS/moltenvk-$plat"
      mkdir -p "$DEPS/moltenvk-$plat"
      cp -R Package/Release/MoltenVK "$DEPS/moltenvk-$plat/"
    done
  )
  echo "$MVK_REV" > "$DEPS/moltenvk-version"
}

case $STAGE in
  deps) stage_deps ;;
  lib|all) stage_lib ;;
  *) echo "usage: $0 [deps|lib]" >&2; exit 1 ;;
esac

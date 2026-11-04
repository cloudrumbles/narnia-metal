#!/bin/bash
# Build MoltenVK from source into $DEPS/moltenvk-{ios,macos}/MoltenVK/static/...
# (the same layout as the release tarballs). We build from source because
# stock MoltenVK has no geometry shaders and no robustness2; see
# docs/notes.md. Pinned commits live in moltenvk/PINS; our own changes are in
# moltenvk/patches (MoltenVK) and moltenvk/spirv-cross-patches (SPIRV-Cross).
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
DEPS=${DEPS:-$REPO/ios/deps}
PLATS=${PLATS:-"macos ios"}
# shellcheck disable=SC1091
. "$REPO/moltenvk/PINS"
SRC="$DEPS/MoltenVK-src"
mkdir -p "$DEPS"

if [ ! -d "$SRC/.git" ]; then
  git clone -q "$MVK_URL" "$SRC"
fi
git -C "$SRC" fetch -q origin "$MVK_REV" 2>/dev/null || true
git -C "$SRC" checkout -q -f "$MVK_REV"
git -C "$SRC" clean -qfdx -e External
for p in "$REPO"/moltenvk/patches/*.patch; do
  [ -e "$p" ] && git -C "$SRC" apply --whitespace=nowarn "$p"
done
# the pinned SPIRV-Cross is fetched by fetchDependencies from the pinned
# revision file (our SPIRV-Cross patches are applied afterwards)
(
  cd "$SRC"
  ./fetchDependencies --macos --ios >/dev/null 2>"$DEPS/fetchdeps.err" || { cat "$DEPS/fetchdeps.err" >&2; exit 1; }
  git -C External/SPIRV-Cross checkout -q -f "$(cat ExternalRevisions/SPIRV-Cross_repo_revision)"
  for p in "$REPO"/moltenvk/spirv-cross-patches/*.patch; do
    [ -e "$p" ] && git -C External/SPIRV-Cross apply --whitespace=nowarn "$p"
  done
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

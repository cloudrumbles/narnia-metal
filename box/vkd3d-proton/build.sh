#!/bin/bash
# Build vkd3d-proton (x86_64 d3d12.dll + d3d12core.dll) at PIN with our
# patches and install it into a Proton dir (and an existing prefix).
#
#   box/vkd3d-proton/build.sh [proton dir] [STEAM_COMPAT_DATA_PATH]
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
PIN=$(cat "$HERE/PIN")
WORK=${WORK:-$HOME/build-vkd3d-proton}
PROTON=${1:-$HOME/proton/GE-Proton11-12-x86_64}
PREFIX=${2:-$HOME/pfx}

sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -q mingw-w64 mingw-w64-tools \
  g++-mingw-w64-x86-64-posix gcc-mingw-w64-x86-64-posix glslang-tools meson ninja-build >/dev/null

if [ ! -d "$WORK/src/.git" ]; then
  git clone -q https://github.com/HansKristian-Work/vkd3d-proton.git "$WORK/src"
fi
cd "$WORK/src"
git checkout -q -f "$PIN"
git clean -qfdx
git submodule update -q --init --recursive
for p in "$HERE"/patches/*.patch; do
  patch -p1 --forward -s < "$p"
done

cat > "$WORK/cross-x86_64.txt" <<EOT
[binaries]
c = 'x86_64-w64-mingw32-gcc-posix'
cpp = 'x86_64-w64-mingw32-g++-posix'
ar = 'x86_64-w64-mingw32-ar'
strip = 'x86_64-w64-mingw32-strip'
widl = 'x86_64-w64-mingw32-widl'

[properties]
needs_exe_wrapper = true

[host_machine]
system = 'windows'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOT

rm -rf "$WORK/build"
meson setup "$WORK/build" --cross-file "$WORK/cross-x86_64.txt" \
  --buildtype=release --strip -Denable_tests=false -Denable_extras=false >/dev/null
ninja -C "$WORK/build"

dest="$PROTON/files/lib/wine/vkd3d-proton/x86_64-windows"
chmod -R u+w "$dest"  # Proton's tarball ships these read-only
for dll in d3d12 d3d12core; do
  built=$(find "$WORK/build" -name "$dll.dll" | head -1)
  [ -f "$dest/$dll.dll.orig" ] || cp "$dest/$dll.dll" "$dest/$dll.dll.orig"
  cp "$built" "$dest/$dll.dll"
  if [ -d "$PREFIX/pfx/drive_c/windows/system32" ]; then
    cp "$built" "$PREFIX/pfx/drive_c/windows/system32/$dll.dll"
  fi
  echo "installed $dll.dll"
done

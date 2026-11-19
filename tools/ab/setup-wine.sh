#!/bin/sh
# tools/ab/setup-wine.sh: (Linux only) everything tools/ab needs to run the test harness under Wine,
# on DXVK over Mesa's software Vulkan (lavapipe): a Wine prefix with DXVK in it, at
# $VU_WINEPREFIX (default ~/.cache/vrupscale/wine), and Microsoft's d3dcompiler_47.dll at
# tools/d3dcompiler_47.dll (the harness compiles its own shaders with it; tools/shaders.sh finds it
# there too). Installs the Ubuntu packages when run as root with apt. Safe to run again.
# On Windows none of this is needed: the harness runs natively, and Windows has d3dcompiler_47.dll.
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PREFIX=${VU_WINEPREFIX:-$HOME/.cache/vrupscale/wine}
DL=${VU_DOWNLOADS:-$HOME/.cache/vrupscale/downloads}
DXVK_VER=2.5.3
ELECTRON_VER=33.2.1   # its Windows zip carries Microsoft's d3dcompiler_47.dll (about 110 MB to download)
mkdir -p "$DL"

if command -v apt-get >/dev/null 2>&1 && [ "$(id -u)" = 0 ]; then
  if ! command -v wine >/dev/null 2>&1 || ! command -v x86_64-w64-mingw32-g++-win32 >/dev/null 2>&1 || \
     [ ! -f /usr/share/vulkan/icd.d/lvp_icd.json ] || ! command -v Xvfb >/dev/null 2>&1; then
    apt-get update -qq
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq mingw-w64 wine64 mesa-vulkan-drivers xvfb unzip curl
  fi
fi
for c in wine wineboot wineserver x86_64-w64-mingw32-g++-win32 Xvfb unzip curl; do
  command -v $c >/dev/null 2>&1 || { echo "missing $c (Ubuntu: apt-get install mingw-w64 wine64 mesa-vulkan-drivers xvfb unzip curl)" >&2; exit 1; }
done
[ -f /usr/share/vulkan/icd.d/lvp_icd.json ] || { echo "missing lavapipe (apt-get install mesa-vulkan-drivers)" >&2; exit 1; }

# Wine 9.0 crashes ("free(): invalid pointer") when /run/user/<uid> is missing, as in containers
U=/run/user/$(id -u)
[ -d "$U" ] || { mkdir -p "$U" && chmod 700 "$U"; }

pgrep -x Xvfb >/dev/null || { (setsid nohup Xvfb :99 -screen 0 1280x1024x24 >/dev/null 2>&1 < /dev/null &); sleep 1; }
export DISPLAY=${DISPLAY:-:99} WINEDEBUG=-all

if [ ! -f "$DL/dxvk-$DXVK_VER.tar.gz" ]; then
  curl -sSfL -o "$DL/dxvk-$DXVK_VER.tar.gz.part" "https://github.com/doitsujin/dxvk/releases/download/v$DXVK_VER/dxvk-$DXVK_VER.tar.gz"
  mv "$DL/dxvk-$DXVK_VER.tar.gz.part" "$DL/dxvk-$DXVK_VER.tar.gz"
fi
if [ ! -f "$ROOT/tools/d3dcompiler_47.dll" ]; then
  if [ ! -f "$DL/d3dcompiler_47.dll" ]; then
    curl -sSfL -o "$DL/electron.zip" "https://github.com/electron/electron/releases/download/v$ELECTRON_VER/electron-v$ELECTRON_VER-win32-x64.zip"
    unzip -q -j -o "$DL/electron.zip" d3dcompiler_47.dll -d "$DL"
    rm -f "$DL/electron.zip"
  fi
  cp "$DL/d3dcompiler_47.dll" "$ROOT/tools/d3dcompiler_47.dll"
fi

if [ ! -f "$PREFIX/drive_c/windows/system32/kernel32.dll" ]; then
  mkdir -p "$(dirname "$PREFIX")"
  WINEPREFIX="$PREFIX" wineboot -i >/dev/null 2>&1
  WINEPREFIX="$PREFIX" wineserver -w
fi
T=$(mktemp -d)
tar -xzf "$DL/dxvk-$DXVK_VER.tar.gz" -C "$T"
cp "$T/dxvk-$DXVK_VER/x64/"*.dll "$PREFIX/drive_c/windows/system32/"
rm -rf "$T"
echo "ready: Wine prefix $PREFIX (DXVK $DXVK_VER), tools/d3dcompiler_47.dll"

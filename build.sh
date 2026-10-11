#!/bin/sh
# Cross-compiles dxgi.dll (vrupscale) for Windows x64 with MinGW-w64. No runtime DLL dependencies.
set -eu
cd "$(dirname "$0")"
CC=${CC:-x86_64-w64-mingw32-gcc-win32}
CXX=${CXX:-x86_64-w64-mingw32-g++-win32}
WINDRES=${WINDRES:-x86_64-w64-mingw32-windres}
OUT=build
mkdir -p $OUT/obj $OUT/gen
[ -f src/gen/shader_blobs.h ] || { echo "src/gen/shader_blobs.h missing: run tools/shaders.sh"; exit 1; }

CFLAGS="-O2 -Wall -D__USE_MINGW_ANSI_STDIO=1 -Wno-unknown-pragmas -DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0A00 -DUNICODE -D_UNICODE"
INC="-Isrc -I$OUT/gen -Ithird_party/ffx -Ithird_party/minhook/include"

for f in third_party/minhook/src/buffer.c third_party/minhook/src/hook.c third_party/minhook/src/trampoline.c third_party/minhook/src/hde/hde64.c; do
  $CC $CFLAGS -w $INC -c $f -o $OUT/obj/mh_$(basename $f .c).o
done
$CC $CFLAGS $INC -c src/vtables.c -o $OUT/obj/vtables.o
$CC -c src/exports.S -o $OUT/obj/exports.o
for f in dllmain log config proxy hooks shaders upscaler temporal control gaze; do
  $CXX $CFLAGS -std=c++17 -fno-exceptions -fno-rtti -Wno-unused-function $INC -c src/$f.cpp -o $OUT/obj/$f.o
done

VER=$(sed -n 's/^#define VU_VERSION_STR "\(.*\)"/\1/p' src/common.h)
RCVER=$(echo "$VER" | tr '.' ',' | sed 's/$/,0/')
$WINDRES -DVU_RC_VERSION="$RCVER" -DVU_RC_VERSION_STR="\\\"$VER\\\"" src/version.rc -O coff -o $OUT/obj/version.o
$CXX -shared -o $OUT/dxgi.dll $OUT/obj/*.o src/exports.def \
  -static -static-libgcc -static-libstdc++ -Wl,--enable-stdcall-fixup -Wl,--subsystem,windows \
  -luuid -ldxguid -s -Wl,--no-insert-timestamp
echo "built $OUT/dxgi.dll"

# ---- the control app (vrupscale.exe)
VER=$(sed -n 's/^#define VU_VERSION_STR "\(.*\)"/\1/p' src/common.h)
RCVER=$(echo "$VER" | tr '.' ',' | sed 's/$/,0/')
mkdir -p $OUT/app
$WINDRES -DVU_RC_VERSION="$RCVER" -DVU_RC_VERSION_STR="\\\"$VER\\\"" -I app app/app.rc -O coff -o $OUT/app/app_res.o
for f in core ui main gameres update changelog; do
  $CXX -O2 -Wall -std=c++17 -municode -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DVU_APP_VERSION="\"$VER\"" \
    -c app/$f.cpp -o $OUT/app/$f.o
done
$CXX -municode -mwindows -o $OUT/vrupscale.exe $OUT/app/core.o $OUT/app/ui.o $OUT/app/main.o $OUT/app/gameres.o $OUT/app/update.o $OUT/app/changelog.o $OUT/app/app_res.o \
  -static -static-libgcc -static-libstdc++ -lgdiplus -lcomctl32 -lversion -lshell32 -lole32 -luuid -ladvapi32 -lwinhttp -lbcrypt -s -Wl,--no-insert-timestamp
echo "built $OUT/vrupscale.exe ($VER)"

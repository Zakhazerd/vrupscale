#!/bin/sh
# tools/ab/build-test.sh: builds build/selftest.exe (test/fake_compositor.cpp, which includes
# test/temporal_tests.inc) without the rest of tools/package.sh. Same CXX as build.sh.
set -eu
cd "$(dirname "$0")/../.."
CXX=${CXX:-x86_64-w64-mingw32-g++-win32}
mkdir -p build
$CXX -O2 -std=c++17 -D__USE_MINGW_ANSI_STDIO=1 -static -s -Wl,--no-insert-timestamp test/fake_compositor.cpp \
  -o build/selftest.exe -ld3d11 -luuid -ldxguid
echo "built build/selftest.exe"

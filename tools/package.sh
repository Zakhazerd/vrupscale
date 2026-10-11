#!/bin/sh
# Builds everything and produces the binary and source zips in build/.
set -eu
cd "$(dirname "$0")/.."
VER=$(sed -n 's/^#define VU_VERSION_STR "\(.*\)"/\1/p' src/common.h)
./build.sh
CXX=${CXX:-x86_64-w64-mingw32-g++-win32}
FLAGS="-O2 -std=c++17 -D__USE_MINGW_ANSI_STDIO=1 -static -s -Wl,--no-insert-timestamp"
$CXX $FLAGS test/fake_compositor.cpp -o build/selftest.exe -ld3d11 -luuid -ldxguid
# (a developer check of the shader sources; needs tools/shaders.sh's output, which a fresh clone lacks)
if [ -f build/gen/shader_sources.h ]; then $CXX $FLAGS -Ibuild/gen test/compile_check.cpp -o build/compile_check.exe; fi

P=build/pkg/vrupscale-$VER
rm -rf build/pkg && mkdir -p $P/LICENSES $P/selftest $P/payload
cp build/vrupscale.exe README.md third_party/openvr/openvr_api.dll $P/
cp build/dxgi.dll $P/payload/
cp LICENSE $P/LICENSES/vrupscale-MIT.txt
cp third_party/minhook/LICENSE.txt $P/LICENSES/MinHook-BSD2.txt
cp third_party/ffx/LICENSE.txt $P/LICENSES/FidelityFX-FSR1-MIT.txt
cp third_party/rubik/OFL.txt $P/LICENSES/Rubik-OFL.txt
cp third_party/openvr/LICENSE.txt $P/LICENSES/OpenVR-BSD3.txt
cp build/dxgi.dll build/selftest.exe $P/selftest/
printf '@echo off\r\ncd /d "%%~dp0"\r\nset VRUPSCALE_FORCE=1\r\nset VRUPSCALE_LOG=%%~dp0selftest.log\r\nif exist selftest.log del selftest.log\r\necho Running the vrupscale self-test (a few minutes).\r\necho Part 1 of 2: compositor draws on the immediate context\r\necho ===== part 1 of 2: compositor draws on the immediate context ===== > selftest_results.txt\r\nselftest.exe "%%~dp0." >> selftest_results.txt 2>&1\r\necho Part 2 of 2: compositor draws recorded into command lists, like SteamVR\r\necho.>> selftest_results.txt\r\necho ===== part 2 of 2: compositor draws recorded into command lists, like SteamVR ===== >> selftest_results.txt\r\nset VRUPSCALE_TEST_DEFERRED=1\r\nselftest.exe "%%~dp0." >> selftest_results.txt 2>&1\r\ntype selftest_results.txt\r\necho.\r\necho Results saved to selftest_results.txt (send that file), details in selftest.log\r\npause\r\n' > $P/selftest/selftest.cmd
# Stored, not compressed: the app's updater unpacks it without a decompressor (app/update.cpp).
(cd build/pkg && rm -f ../vrupscale-$VER.zip && python3 -I -c "
import os, zipfile
with zipfile.ZipFile('../vrupscale-$VER.zip', 'w', zipfile.ZIP_STORED) as z:
    for root, dirs, files in os.walk('vrupscale-$VER'):
        dirs.sort()
        z.write(root)
        for f in sorted(files):
            z.write(os.path.join(root, f))
")
(cd build && sha256sum vrupscale-$VER.zip > vrupscale-$VER.zip.sha256)

S=build/src/vrupscale-$VER-src
rm -rf build/src && mkdir -p $S
cp -r src shaders test tools app third_party build.sh README.md LICENSE .gitignore .github $S/
(cd build/src && python3 -I -c "
import shutil; shutil.make_archive('../vrupscale-$VER-src', 'zip', '.', 'vrupscale-$VER-src')")
ls -la build/vrupscale-$VER*

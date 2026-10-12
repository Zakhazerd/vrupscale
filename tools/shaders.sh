#!/bin/sh
# Regenerates src/gen/shaders.bin (the compiled shaders the DLL embeds) from shaders/*.hlsl with
# Microsoft's d3dcompiler_47.dll.
# On Linux this runs the compiler under Wine; put d3dcompiler_47.dll in tools/ or pass D3DCOMPILER=path.
set -eu
cd "$(dirname "$0")/.."
CXX=${CXX:-x86_64-w64-mingw32-g++-win32}
mkdir -p build/shadertool src/gen build/asm
$CXX -O1 -std=c++17 -D__USE_MINGW_ANSI_STDIO=1 -Isrc tools/compile_shaders.cpp -o build/shadertool/compile_shaders.exe -static -s
cp "${D3DCOMPILER:-tools/d3dcompiler_47.dll}" build/shadertool/
if command -v wine64 >/dev/null 2>&1 && [ "$(uname -s)" = "Linux" ]; then
  RUN="${WINE:-wine64}"
  to_win() { printf 'Z:%s' "$(readlink -f "$1" | sed 's#/#\\#g')"; }
  $RUN build/shadertool/compile_shaders.exe "$(to_win shaders)" "$(to_win third_party/ffx)" "$(to_win src/gen)\\shaders.bin" "$(to_win build/asm)"
else
  build/shadertool/compile_shaders.exe shaders third_party/ffx src/gen/shaders.bin build/asm
fi

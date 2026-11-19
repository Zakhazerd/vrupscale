#!/bin/sh
# tools/ab/translate.sh <dxgi.dll> <dir>: the app's translation end to end, without SteamVR and
# without the internet. Windows only (MSYS2 / Git Bash): it reads with Windows' English OCR.
#   - test/fake_compositor.cpp shows signs with text through that DLL (test/text_scene.inc): two
#     signs, the second one changed, none, the signs again, and Ctrl+F11 "pressed" (translation off);
#   - build/vrupscale.exe reads and "translates" them, with test/stub/openvr_stub.c standing in for
#     SteamVR and test/fake_deepl.py for DeepL (it answers a text in capitals between << >>);
#   - tools/ab/translate_check.py compares what the app sent, drew and placed with the scene.
# Needs build/vrupscale.exe, build/selftest.exe (build.sh, tools/ab/build-test.sh), a C compiler for
# the stub (CC, as build.sh) and Python 3 (PYTHON). FACTOR=1.3 upscales the scene (default 1.0),
# SECONDS_ON=n the length (default 36), VRUPSCALE_TEST_DEFERRED=1 draws through command lists,
# HEAD="x y z yaw" puts the head elsewhere (metres, degrees to the left; default "0 1.6 0 0").
set -u
. "$(dirname "$0")/common.sh"
[ $# -ge 2 ] || { sed -n '2,12p' "$0"; exit 2; }
[ "$VU_NATIVE" = 1 ] || { echo "translate.sh runs on Windows only (it needs Windows' text recognition)"; exit 2; }
CC=${CC:-x86_64-w64-mingw32-gcc-win32}
PYTHON=${PYTHON:-python3}
FACTOR=${FACTOR:-1.0}
LEN=${SECONDS_ON:-36}
HEAD=${HEAD:-0 1.6 0 0}
APP=$VU_ROOT/build/vrupscale.exe
[ -f "$APP" ] || { echo "no $APP: run build.sh first" >&2; exit 1; }
D=$2
ab_prepare "$1" "$D"
D=$(cd "$D" && pwd)
mkdir -p "$D/pics"
$CC -O1 -shared -o "$D/openvr_stub.dll" "$VU_ROOT/test/stub/openvr_stub.c" -static-libgcc || exit 1
: > "$D/steamvr.flag"

PORT=$((20000 + $$ % 20000))
KEY=00000000-test-test-test-000000000000:fx   # (not a real key: it only ever goes to the stand-in)
CH="Local\\vrupscale.control.test.translate.$$"
$PYTHON "$VU_ROOT/test/fake_deepl.py" $PORT "$(ab_winpath "$D")\\deepl.log" &
DEEPL=$!

export VRUPSCALE_CONTROL="$CH" VRUPSCALE_TEST_SERVE=$LEN VRUPSCALE_TEST_SERVE_TEXT=1
export VRUPSCALE_TEST_SERVE_SET="upscale.upscaleFactor=$FACTOR;translate.enabled=1;translate.source=en-US;translate.target=ja;translate.deeplKey=$KEY;translate.captureSize=1024;app.checkUpdates=0;app.manageResolution=0"
ab_exe "$D" 180 &
COMP=$!
i=0
while [ $i -lt 120 ] && ! grep -q "serving text" "$D/out.txt" 2>/dev/null; do sleep 0.25; i=$((i + 1)); done
grep -q "serving text" "$D/out.txt" 2>/dev/null || { echo "the compositor stand-in didn't start:"; tail -5 "$D/out.txt"; kill $DEEPL 2>/dev/null; exit 1; }

(
  export VRUPSCALE_TEST_OPENVR="$(ab_winpath "$D/openvr_stub.dll")" VRUPSCALE_TEST_STEAMVR_FLAG="$(ab_winpath "$D/steamvr.flag")"
  export STUB_LOG="$(ab_winpath "$D")\\stub.log" STUB_LOG_TIME=1 STUB_DUMP_DIR="$(ab_winpath "$D/pics")" STUB_QUIT_FILE="$(ab_winpath "$D")\\quit"
  export STUB_OVERLAY_HIDDEN=1 VRUPSCALE_DEEPL_URL="http://127.0.0.1:$PORT" VRUPSCALE_TRANSLATE_LOG="$(ab_winpath "$D")\\translate.log"
  export STUB_HEAD="$HEAD"
  "$APP" --settings "$(ab_winpath "$D")\\vrupscale.ini" --channel "$CH" --from-steamvr
  echo "app exit $?" >> "$D/out.txt"
) &
APPSH=$!
wait $COMP
: > "$D/quit"   # "SteamVR" asks the app to quit
i=0
while [ $i -lt 80 ] && ! grep -q "app exit" "$D/out.txt"; do sleep 0.25; i=$((i + 1)); done
grep -q "app exit" "$D/out.txt" || { echo "the app didn't close when asked"; taskkill //F //IM vrupscale.exe > /dev/null 2>&1; }
wait $APPSH 2>/dev/null
kill $DEEPL 2>/dev/null
$PYTHON "$VU_ROOT/tools/ab/translate_check.py" "$D" "$KEY" $HEAD

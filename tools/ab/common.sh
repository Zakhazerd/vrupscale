# Sourced by tools/ab/*.sh: runs test/fake_compositor.cpp's exe (build/selftest.exe) in a folder of
# its own with a given dxgi.dll, natively on Windows (MSYS2 / Git Bash) or under Wine on Linux
# (set up once with tools/ab/setup-wine.sh).
#   ab_prepare <dxgi.dll> <dir>   fresh <dir> with the exe and that DLL
#   ab_exe <dir> [timeout s]      runs it there (output appended to <dir>/out.txt); env VRUPSCALE_TEST_* etc.
#                                 set by the caller. VRUPSCALE_LOG goes to <dir>/vrupscale.log.
VU_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
VU_SELFTEST=${VU_SELFTEST:-$VU_ROOT/build/selftest.exe}
VU_WINEPREFIX=${VU_WINEPREFIX:-$HOME/.cache/vrupscale/wine}
VU_D3DCOMPILER=${VU_D3DCOMPILER:-$VU_ROOT/tools/d3dcompiler_47.dll}

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) VU_NATIVE=1 ;;
  *) VU_NATIVE=0 ;;
esac

ab_winpath() {   # a path as the exe sees it
  if [ "$VU_NATIVE" = 1 ]; then cygpath -w "$1"; else printf 'Z:%s' "$(readlink -f "$1" | sed 's#/#\\#g')"; fi
}

ab_prepare() {
  [ -f "$VU_SELFTEST" ] || { echo "no $VU_SELFTEST: run tools/package.sh (or tools/ab/build-test.sh) first" >&2; exit 1; }
  [ -f "$1" ] || { echo "no DLL $1" >&2; exit 1; }
  rm -rf "$2" && mkdir -p "$2"
  cp "$1" "$2/dxgi.dll"
  cp "$VU_SELFTEST" "$2/fake_compositor.exe"
  if [ "$VU_NATIVE" = 0 ]; then
    [ -f "$VU_D3DCOMPILER" ] || { echo "no $VU_D3DCOMPILER: run tools/ab/setup-wine.sh" >&2; exit 1; }
    cp "$VU_D3DCOMPILER" "$2/d3dcompiler_47.dll"
  fi
}

ab_exe() {
  _d=$(cd "$1" && pwd)
  _t=${2:-3600}
  export VRUPSCALE_FORCE=1 VRUPSCALE_LOG="$(ab_winpath "$_d/vrupscale.log")"
  _to="timeout $_t"
  timeout --version >/dev/null 2>&1 || _to=""   # (coreutils' timeout; Windows' own timeout.exe is something else)
  if [ "$VU_NATIVE" = 1 ]; then
    (cd "$_d" && $_to ./fake_compositor.exe "$(ab_winpath "$_d")" >> out.txt 2>&1)
    _rc=$?
  else
    [ -d "$VU_WINEPREFIX" ] || { echo "no Wine prefix $VU_WINEPREFIX: run tools/ab/setup-wine.sh" >&2; exit 1; }
    [ -d "/run/user/$(id -u)" ] || { mkdir -p "/run/user/$(id -u)" 2>/dev/null && chmod 700 "/run/user/$(id -u)"; }
    export DISPLAY=${DISPLAY:-:99}
    if command -v Xvfb >/dev/null 2>&1 && ! pgrep -x Xvfb >/dev/null; then
      (setsid nohup Xvfb "$DISPLAY" -screen 0 1280x1024x24 >/dev/null 2>&1 < /dev/null &)
      sleep 1
    fi
    # DXVK on lavapipe (Mesa's software Vulkan). dcSingleUseMode=False: command lists replayed more than once, like SteamVR.
    (cd "$_d" && WINEPREFIX="$VU_WINEPREFIX" WINEDEBUG=-all VK_ICD_FILENAMES=${VK_ICD_FILENAMES:-/usr/share/vulkan/icd.d/lvp_icd.json} \
      DXVK_CONFIG="d3d11.dcSingleUseMode = False" DXVK_LOG_LEVEL=none WINEDLLOVERRIDES="d3d11,dxgi,d3dcompiler_47=n" \
      $_to ${VU_WINE:-wine} fake_compositor.exe "$(ab_winpath "$_d")" >> out.txt 2>&1)
    _rc=$?   # (no wineserver -k here: other runs may share the prefix; the server exits by itself)
  fi
  return $_rc
}

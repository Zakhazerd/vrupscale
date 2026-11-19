#!/bin/sh
# tools/ab/selftest.sh <dxgi.dll> <outdir> [deferred]: the full self-test (what the release's
# selftest\selftest.cmd runs) with that DLL: about 195 checks, ending "RESULT: N passed, 0 failed".
# "deferred" records the compositor's draws into command lists, like SteamVR (the cmd's part 2).
# On Windows it runs on the real GPU, and its performance section gives real GPU times; under Wine
# the times are a software GPU's and mean little, and the performance section needs the machine to
# itself (two runs at once can crash it in winevulkan). VRUPSCALE_TEST_QUALITY_ONLY=1 or
# VRUPSCALE_TEST_PERF_ONLY=1 in the environment run one section. VRUPSCALE_TEST_EXTRA=opticalFlow=fsr3
# (more [temporal] settings, one per line) times every performance mode with those settings.
set -u
. "$(dirname "$0")/common.sh"
[ $# -ge 2 ] || { sed -n '2,9p' "$0"; exit 2; }
D=$2
ab_prepare "$1" "$D"
[ "${3:-}" = deferred ] && export VRUPSCALE_TEST_DEFERRED=1
ab_exe "$D" 5400
rc=$?
echo "exit $rc" >> "$D/out.txt"
grep "FAIL\|RESULT" "$D/out.txt"
exit $rc

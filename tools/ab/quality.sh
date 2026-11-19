#!/bin/sh
# tools/ab/quality.sh <dxgi.dll> <outdir> ["extra [temporal] settings, '|' between lines"]: the
# self-test's quality table (FSR 1 vs temporal against ground truth per region, plus the flow error
# against the true motion) with that DLL; the extra settings go into the temporal run's [temporal]
# section, e.g. "opticalFlow=fsr3". Prints the table (it is in <outdir>/out.txt).
#   MOTION=1 (head motion scale: 40 fast, 100 very fast and jerky); W=400 H=440 FACTOR=1.3 FRAMES=24;
#   SKIPFSR=1 (no FSR 1 column: faster); FULL=1 (all the quality checks too, not just the table:
#   only meaningful at the default W/H)
# The scene is small at the default size: its coarsest pyramid levels are a few pixels wide. For the
# optical flow, W=1000 H=1100 FACTOR=1.0 FRAMES=16 is closer to a headset (about 2 GB; one at a time).
set -u
. "$(dirname "$0")/common.sh"
[ $# -ge 2 ] || { sed -n '2,11p' "$0"; exit 2; }
D=$2
ab_prepare "$1" "$D"
export VRUPSCALE_TEST_QUALITY_ONLY=1 VRUPSCALE_TEST_TUNE_ONLY=$([ "${FULL:-0}" = 1 ] && echo 0 || echo 1) \
  VRUPSCALE_TEST_MOTION=${MOTION:-1} VRUPSCALE_TEST_SKIP_FSR=${SKIPFSR:-0} VRUPSCALE_TEST_W=${W:-400} VRUPSCALE_TEST_H=${H:-440} \
  VRUPSCALE_TEST_FACTOR=${FACTOR:-1.3} VRUPSCALE_TEST_FRAMES=${FRAMES:-24}
export VRUPSCALE_TEST_EXTRA="$(printf '%s' "${3:-}" | tr '|' '\n')"
ab_exe "$D" 3600
rc=$?
echo "exit $rc" >> "$D/out.txt"
grep -A8 "flow (temporal)" "$D/out.txt"
grep "RESULT\|FAIL" "$D/out.txt"
exit $rc

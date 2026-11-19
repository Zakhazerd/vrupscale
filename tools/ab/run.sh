#!/bin/sh
# tools/ab/run.sh <dxgi.dll> <outdir>: runs the A/B variants below with that DLL and writes every
# frame's output of both eyes to <outdir> (<tag>_e<eye>_<frame>.raw, raw RGBA8), for tools/ab/cmp.py.
# The usual check: the same variants with two builds (tools/ab/build-at.sh), then cmp.py; a change
# meant to leave the picture alone must come out identical.
#   VARIANTS="v1 v3 v8 u1"  which (default: all); FRAMES=16; W=400 H=440 (the game's eye size);
#   FACTOR=1.3 (the size the "compositor" draws each eye at, x W/H; each variant's own upscaleFactor
#   is in its settings);
#   MOTION=1 (head motion scale: 40 is fast, 100 very fast and jerky); NOTIME=1 (gpuTiming=0);
#   STATS=1 (per-pass GPU times in out.txt); TOGA=n TOGB=n (Ctrl+F10 before frame n)
# out.txt holds each variant's exit status ("exit 0 v1"). Two runs at a time at most on a 2-core,
# 8 GB machine; large W/H need more memory (W=1200 H=1320 runs out of 8 GB with two at once).
set -u
. "$(dirname "$0")/common.sh"
[ $# -ge 2 ] || { sed -n '2,13p' "$0"; exit 2; }
D=$2
ab_prepare "$1" "$D"
export VRUPSCALE_TEST_ONLY=abdump VRUPSCALE_TEST_FRAMES=${FRAMES:-16} VRUPSCALE_AB_NOTIME=${NOTIME:-0} \
  VRUPSCALE_AB_STATS=${STATS:-0} VRUPSCALE_AB_TOGGLE_A=${TOGA:-} VRUPSCALE_AB_TOGGLE_B=${TOGB:-} \
  VRUPSCALE_TEST_FACTOR=${FACTOR:-1.3} VRUPSCALE_TEST_W=${W:-400} VRUPSCALE_TEST_H=${H:-440} VRUPSCALE_TEST_MOTION=${MOTION:-1}
VARIANTS=${VARIANTS:-}
run() {   # tag, settings ('|' between lines), [1: each frame drawn twice]
  if [ -n "$VARIANTS" ]; then case " $VARIANTS " in *" $1 "*) ;; *) return 0 ;; esac; fi
  VRUPSCALE_AB_TAG=$1 VRUPSCALE_AB_INI="$2" VRUPSCALE_AB_REDRAW=${3:-0} ab_exe "$D" 1800
  rc=$?
  n=$(ls "$D" | grep -c "^$1_e[01]_.*\.raw$")
  [ "$rc" = 0 ] && [ "$n" != $((2 * ${FRAMES:-16})) ] && rc="99 ($n of $((2 * ${FRAMES:-16})) frames written)"
  echo "exit $rc $1" >> "$D/out.txt"
}
U='[upscale]|mode=temporal|upscaleFactor=1.30'
S='[upscale]|mode=fsr|upscaleFactor=1.30'
run v1 "$U|sharpness=1.0|[temporal]|temporalArea=0.6"
run v2 "$U|sharpness=0|[temporal]|temporalArea=0.6"
run v3 "$U|sharpness=0.5|[temporal]|temporalArea=1"
run v4 "$U|sharpness=0.8|sharpnessArea=0.85|[temporal]|temporalArea=0.6|sharpHistory=1|bilinearEdges=1"
run v5 "$U|sharpness=1.0|fxaa=after|[temporal]|temporalArea=0.6"
run v6 "$U|sharpness=1.0|debugTint=1|fxaa=before|[temporal]|temporalArea=0.6|localTrust=0"
run v7 "$S|sharpness=1.0|[temporal]|temporalArea=0.6"
run v8 "$U|sharpness=1.0|[temporal]|temporalArea=0.6" 1
run v9 "$U|sharpness=1.0|[temporal]|temporalArea=0.6|commandLists=0"
run v10 "$S|sharpness=0.5|[temporal]|temporalArea=1"
run v11 "$S|sharpness=0.8|sharpnessArea=0.85|[temporal]|temporalArea=0.6|bilinearEdges=1"
run v12 "$S|sharpness=1.0|fxaa=after|debugTint=1|[temporal]|temporalArea=0.6"
run v13 "$S|sharpness=0|[temporal]|temporalArea=0.6"
run v14 "$S|sharpness=0.6|fxaa=before|[temporal]|temporalArea=0.6" 1
# at 1x (Native), with the settings in use on the RTX 3090 / Beyond 2e
N='[upscale]|mode=temporal|upscaleFactor=1.00'
run u1 "$N|sharpness=1.0|sharpnessArea=0.8|[temporal]|temporalArea=0.6|bilinearEdges=1|localTrust=0.8|residualTol=0.9"
run u2 "$N|sharpness=1.0|sharpnessArea=0.8|[temporal]|temporalArea=0.6|bilinearEdges=1|localTrust=0.8|residualTol=0.9" 1
run u3 "[upscale]|mode=fsr|upscaleFactor=1.00|sharpness=1.0|sharpnessArea=0.8|[temporal]|temporalArea=0.6|bilinearEdges=1"
# [temporal] opticalFlow=fsr3 (the FSR 3 optical flow experiment): compare w1 with v1 etc.
run w1 "$U|sharpness=1.0|[temporal]|temporalArea=0.6|opticalFlow=fsr3"
run w3 "$U|sharpness=0.5|[temporal]|temporalArea=1|opticalFlow=fsr3"
run w8 "$U|sharpness=1.0|[temporal]|temporalArea=0.6|opticalFlow=fsr3" 1
run x1 "$N|sharpness=1.0|sharpnessArea=0.8|[temporal]|temporalArea=0.6|bilinearEdges=1|localTrust=0.8|residualTol=0.9|opticalFlow=fsr3"
echo done >> "$D/out.txt"
grep -c "^exit 0" "$D/out.txt" | sed 's/$/ variants ran/'
grep "^exit [1-9]" "$D/out.txt" && exit 1
exit 0

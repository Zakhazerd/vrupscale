#!/bin/sh
# tools/ab/build-at.sh <commit> <out.dll>: builds dxgi.dll as it was at <commit> (a branch, tag or
# hash) in a temporary git worktree, for an A/B against the working tree's build. The compiled
# shaders are committed (src/gen/shaders.bin), so no shader compiler is needed. Takes the same
# CC / CXX / WINDRES as build.sh (on MSYS2: CC=gcc CXX=g++ WINDRES=windres).
set -eu
[ $# -eq 2 ] || { sed -n '2,5p' "$0"; exit 2; }
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
WT=$(mktemp -d "${TMPDIR:-/tmp}/vrupscale-at.XXXXXX")
trap 'git -C "$ROOT" worktree remove --force "$WT" >/dev/null 2>&1; rm -rf "$WT"' EXIT
git -C "$ROOT" worktree add --detach -q "$WT" "$1"
(cd "$WT" && sh build.sh > build.log 2>&1) || { tail -20 "$WT/build.log"; exit 1; }
cp "$WT/build/dxgi.dll" "$OUT"
echo "built $OUT at $(git -C "$ROOT" rev-parse --short "$1")"

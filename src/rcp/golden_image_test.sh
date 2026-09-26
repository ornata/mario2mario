#!/bin/sh
# Golden-image test: render a replayed frame with the real OpenGL backend
# and compare it with a committed golden PNG within a tolerance.
#
# Needs a GL context (WindowServer), so the target is tagged "manual" and
# "local" (no sandbox). Run it explicitly:
#   bazelisk test //src/rcp:golden_image_test
# Regenerate a golden with:
#   bazelisk run //oracle:play -- --headless --scale 1 --replay REC \
#     --frames N --png tests/goldens/NAME.png
set -eu
PLAY="$1"; PNGDIFF="$2"; shift 2
while [ $# -ge 3 ]; do
  rec="$1"; frames="$2"; golden="$3"; shift 3
  out="$TEST_TMPDIR/$(basename "$golden")"
  "$PLAY" --headless --scale 1 --replay "$rec" --frames "$frames" --png "$out"
  echo "$golden vs frame $frames:"
  "$PNGDIFF" "$out" "$golden"
done

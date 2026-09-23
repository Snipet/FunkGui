#!/bin/sh
# Capture one frame of the panel as a primitive dump, with everything that
# could vary between runs pinned: the Graphite theme (not persisted), Init
# and default parameters whatever session the Standalone restores, a fresh
# preset database, frame 8 (the first-run hint is still fully
# drawn; only its alpha fades, which the fingerprint ignores).
#
#   Scripts/capture-frame.sh <build-dir> <out.dump>
#
# Exits non-zero if no frame was written within the wait — no window server,
# or the GPU path did not come up.
set -e
BUILD=${1:?build dir}; OUT=${2:?output dump path}
APP=$(find "$BUILD/HardwareReverb_artefacts" -name HardwareReverb -type f -path "*Standalone*/MacOS/*" | head -1)
[ -x "$APP" ] || { echo "no Standalone under $BUILD" >&2; exit 2; }
rm -f "$OUT"
# A fresh preset database per capture, so the browser shows exactly the
# factory bank in a known order — and so a capture can never read or write
# the real ~/Library/Application Support/HardwareReverb/Presets.db. Callers
# that set HRVB_PRESETS_DB themselves keep theirs. HRVB_UI_FRESH makes the
# processor ignore state restores: the Standalone reopens its last session
# from the user's own settings file, which a golden must not depend on.
SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/hrvb-capture.XXXXXX")
trap 'rm -rf "$SCRATCH"' EXIT
: "${HRVB_PRESETS_DB:=$SCRATCH/Presets.db}"
export HRVB_PRESETS_DB
# Backing scale 2 from the first frame, whatever display the window opens on:
# the geometry fingerprint is in physical pixels, so a 1x display would
# fingerprint differently. Callers that drive the scale-change path set
# their own.
: "${HRVB_UI_SCALE:=2}"
export HRVB_UI_SCALE
HRVB_CANVAS_DUMP="$OUT" HRVB_CANVAS_DUMP_AFTER=${HRVB_CANVAS_DUMP_AFTER:-8} HRVB_UI_THEME=0 HRVB_UI_FRESH=1 "$APP" >/dev/null 2>&1 &
PID=$!
i=0; while [ $i -lt 40 ] && [ ! -f "$OUT" ]; do sleep 0.5; i=$((i+1)); done
kill $PID 2>/dev/null; wait $PID 2>/dev/null || true
[ -f "$OUT" ] || { echo "no frame captured" >&2; exit 1; }

#!/bin/sh
# tools/capture-frame.sh: capture one frame of a FunkGui app as a dump v2 (FCompressor docs/design/02-funkgui-and-ui.md
# §2.2, §3.10, §5.1; A §6.4.8; G7). HR's Scripts/capture-frame.sh, generalised: the app binary and the product's
# environment prefix are arguments, and every product hook is <ENV_PREFIX><NAME> (EditorHost's CaptureConfig).
#
#   tools/capture-frame.sh <app-binary> <out.dump> <ENV_PREFIX>
#
#   <app-binary>  the executable inside the bundle, e.g. .../FunkGuiGalleryApp.app/Contents/MacOS/FunkGuiGalleryApp or
#                 FCompressor_artefacts/<Config>/Standalone/FCompressor.app/Contents/MacOS/FCompressor
#   <ENV_PREFIX>  the product's funkgui_configure_product ENV_PREFIX: FUNKGUI_ (gallery), FCMP_ (FCompressor)
#
# Everything that could vary between runs is pinned unless the caller already set it (P = <ENV_PREFIX>):
#   P CANVAS_DUMP=<out.dump> (always)   P CANVAS_DUMP_AFTER=8   P UI_THEME=0 (not persisted)   P UI_SCALE=2 (the
#   fingerprint is in physical px, so a 1x display must not change it)   P UI_FIXED_DT=0.0166666675 (set it to 0 for the
#   real clock)   P PREFS_DIR and P PRESETS_DB inside a scratch directory, so a capture never reads or writes the real
#   ~/Library/Application Support/<product>/ (C §6.6).
# Other hooks (P UI_KEYS, P A11Y_DUMP, P UI_VIEW, P GPU_LOG, ...) pass through from the caller's environment.
#
# The app is started, the script polls for the dump (EditorHost writes it beside the path and renames it in, so its
# existence means the frame is complete), then stops the app. Exits 0 with the dump in place; 1 if no frame was written
# within CAPTURE_TIMEOUT_SEC (default 20): no window server, the screen is locked, or the GPU path never came up; the
# tail of the app's output is printed then. 2 on a usage error.
set -eu

usage() {
  echo "usage: tools/capture-frame.sh <app-binary> <out.dump> <ENV_PREFIX>" >&2
  exit 2
}
[ $# -eq 3 ] || usage
APP=$1
OUT=$2
P=$3
case "$P" in
  ''|*[!A-Za-z0-9_]*) echo "capture-frame.sh: ENV_PREFIX '$P' must be letters, digits and underscores" >&2; exit 2 ;;
esac
[ -f "$APP" ] && [ -x "$APP" ] || { echo "capture-frame.sh: not an executable: $APP" >&2; exit 2; }
[ -n "$OUT" ] || usage
TIMEOUT=${CAPTURE_TIMEOUT_SEC:-20}
case "$TIMEOUT" in ''|*[!0-9]*) echo "capture-frame.sh: CAPTURE_TIMEOUT_SEC must be whole seconds" >&2; exit 2 ;; esac

rm -f "$OUT" "$OUT.partial"
SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/funkgui-capture.XXXXXX")
PID=
cleanup() {
  if [ -n "$PID" ]; then
    kill "$PID" 2>/dev/null || true
    wait "$PID" 2>/dev/null || true
  fi
  rm -rf "$SCRATCH"
}
trap cleanup EXIT INT TERM

# <P><NAME>=<value> unless the caller set <P><NAME>. The prefix was validated above, so the eval sees only an identifier.
pin() {
  eval "_have=\${${P}$1+set}"
  if [ -z "$_have" ]; then
    eval "${P}$1=\$2"
    eval "export ${P}$1"
  fi
}
pin CANVAS_DUMP_AFTER 8
pin UI_THEME 0
pin UI_SCALE 2
pin UI_FIXED_DT 0.0166666675
pin PREFS_DIR "$SCRATCH/prefs"
pin PRESETS_DB "$SCRATCH/Presets.db"
eval "${P}CANVAS_DUMP=\$OUT"
eval "export ${P}CANVAS_DUMP"

"$APP" >"$SCRATCH/app.log" 2>&1 &
PID=$!

ticks=$((TIMEOUT * 4))
i=0
while [ $i -lt $ticks ] && [ ! -f "$OUT" ]; do
  if ! kill -0 "$PID" 2>/dev/null; then
    break                                              # the app exited on its own (bad section, crash)
  fi
  sleep 0.25
  i=$((i + 1))
done

if [ -f "$OUT" ]; then
  exit 0
fi
echo "capture-frame.sh: no frame captured within ${TIMEOUT} s ($APP)" >&2
if [ -s "$SCRATCH/app.log" ]; then
  echo "---- app output (last 20 lines) ----" >&2
  tail -n 20 "$SCRATCH/app.log" >&2
fi
exit 1

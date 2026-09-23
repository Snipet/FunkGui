#!/usr/bin/env bash
# tools/subset-font.sh: regenerates fonts/JetBrainsMono-Regular-subset.ttf from fonts/upstream/JetBrainsMono-Regular.ttf
# with printable ASCII plus every codepoint of include/funkgui/text/Glyphs.def (FCompressor
# docs/design/02-funkgui-and-ui.md §4.6; A §4's command, generalised).
#
#   tools/subset-font.sh [--venv <dir>] [--out <file>]
#
# fontTools is not a build or configure dependency and is not installed on the build machines: unless --venv names an
# existing virtual environment that has it, the script makes a throwaway one under $TMPDIR, installs fonttools into it
# (the one network access, 02 §4.6) and deletes it on exit. The subset is generated once and committed.
#
# After writing the subset, the script reads its cmap back and fails if any Glyphs.def codepoint (or any printable
# ASCII character) is missing: a codepoint the face lacks would otherwise bake to nothing and draw as nothing. It does
# not prove the atlas: build and run fg.font.probe (FontProbe bakes the subset and the upstream face and requires
# bit-identical distance fields and advances), then hand the atlas candidates to the lead (golden impact `atlas`).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"
venv=""
# The fontTools release the committed subset was cut with (S3 G4: sha256 2cda2110...c311e, 11004 bytes). Pinned so a
# rerun reproduces the file byte for byte; the same release reproduces v0.3.0's nine-extra subset (610aa83f...5661)
# from HR's list, so this is HR's command, generalised (A §4). Only the atlas proof (fg.font.probe) is binding.
fonttools_version="4.65.0"
out="$root/fonts/JetBrainsMono-Regular-subset.ttf"
upstream="$root/fonts/upstream/JetBrainsMono-Regular.ttf"
glyphs="$root/include/funkgui/text/Glyphs.def"

usage() {
  echo "usage: tools/subset-font.sh [--venv <dir>] [--out <file>]" >&2
  exit 2
}

while [ $# -gt 0 ]; do
  case "$1" in
    --venv) [ $# -ge 2 ] || usage; venv="$2"; shift 2 ;;
    --out)  [ $# -ge 2 ] || usage; out="$2"; shift 2 ;;
    *) usage ;;
  esac
done

[ -f "$upstream" ] || { echo "subset-font.sh: no upstream face at $upstream" >&2; exit 1; }
[ -f "$glyphs" ] || { echo "subset-font.sh: no $glyphs" >&2; exit 1; }

# The Glyphs.def codepoints as U+XXXX, in file order (02 §4.6's grep).
extras="$(grep -o 'FUNKGUI_GLYPH(0x[0-9A-F]*' "$glyphs" | sed 's/.*0x/U+/' | paste -sd, -)"
[ -n "$extras" ] || { echo "subset-font.sh: no FUNKGUI_GLYPH entries in $glyphs" >&2; exit 1; }
count="$(printf '%s\n' "$extras" | tr ',' '\n' | wc -l | tr -d ' ')"
dups="$(printf '%s\n' "$extras" | tr ',' '\n' | sort | uniq -d | paste -sd, -)"
[ -z "$dups" ] || { echo "subset-font.sh: duplicate Glyphs.def codepoints: $dups" >&2; exit 1; }

cleanup=""
if [ -z "$venv" ]; then
  venv="$(mktemp -d "${TMPDIR:-/tmp}/funkgui-fonttools.XXXXXX")"
  cleanup="$venv"
  trap 'rm -rf "$cleanup"' EXIT
fi
if [ ! -x "$venv/bin/pyftsubset" ]; then
  echo "== subset-font.sh: fonttools into the throwaway venv $venv"
  python3 -m venv "$venv"
  "$venv/bin/pip" install --quiet --disable-pip-version-check "fonttools==$fonttools_version"
fi
echo "== fonttools $("$venv/bin/python" -c 'import fontTools; print(fontTools.version)')"
echo "== $count extra codepoints: $extras"

"$venv/bin/pyftsubset" "$upstream" --output-file="$out" \
  --unicodes="U+0020-007E,$extras" --layout-features='' --no-hinting --name-IDs='0,13,14' \
  --drop-tables+=DSIG --notdef-outline

# Every requested codepoint must be in the subset's cmap.
"$venv/bin/python" - "$out" "$extras" <<'PY'
import sys
from fontTools.ttLib import TTFont

path, extras = sys.argv[1], sys.argv[2]
cmap = TTFont(path)["cmap"].getBestCmap()
want = list(range(0x20, 0x7F)) + [int(u[2:], 16) for u in extras.split(",")]
missing = [cp for cp in want if cp not in cmap]
if missing:
    sys.exit("subset-font.sh: not in the subset (absent from the upstream face?): "
             + ", ".join("U+%04X" % cp for cp in missing))
print("== %s: %d cmap entries, all %d requested codepoints present" % (path, len(cmap), len(want)))
PY
ls -l "$out"
shasum -a 256 "$out"

#!/usr/bin/env bash
# tools/check-headers.sh: every public header under include/funkgui compiles standalone (CTest fg.headers; FCompressor
# docs/design/03-build-verify-process.md §4.7 item 2, lead revision 1 of docs/sprints/s0.md).
#
#   check-headers.sh --cxx <compiler> --flags <file> --root <FunkGui source dir> [--bgfx] [--sdk <path>]
#                    [--min-macos <version>] [--extra-flag <flag>]...
#
# --extra-flag adds a compiler flag to every header's compile: cmake/FunkGuiPlatform.cmake's JUCE 8.0.4 workaround where
# the compiler needs one (upstream Clang; v0.11.0).
#
# <file> holds a consumer's include directories and definitions, one per line as "I <dir>" and "D <definition>"
# (test/CMakeLists.txt generates it from a target that links FunkGui::core, ::harness and, with bgfx, ::gpu, and calls
# funkgui_configure_product). Directories inside the FunkGui source tree or the build directory are -I; everything
# else (JUCE, bgfx) is -isystem: third-party headers are not what this checks.
#
# Each header is its own translation unit: -std=c++20 -fsyntax-only -Wall -Wextra -Wshadow -Wpedantic -Werror
# -ffp-contract=off, compiled as c++-header (objective-c++-header when it contains Objective-C), so a missing include,
# a non-inline definition in a header or a warning fails. include/funkgui/gpu/ is checked only with --bgfx. Negative
# check: core/Config.h without the product seams must fail with its "call funkgui_configure_product()" #error.
set -uo pipefail

cxx="" flags="" root="" bgfx=0 sdk="" minos="" extra=()
while [ $# -gt 0 ]; do
  case "$1" in
    --cxx) cxx="${2:-}"; shift 2 ;;
    --flags) flags="${2:-}"; shift 2 ;;
    --root) root="${2:-}"; shift 2 ;;
    --sdk) sdk="${2:-}"; shift 2 ;;
    --min-macos) minos="${2:-}"; shift 2 ;;
    --bgfx) bgfx=1; shift ;;
    --extra-flag) extra+=("${2:-}"); shift 2 ;;
    *) echo "check-headers.sh: unknown argument $1" >&2; exit 2 ;;
  esac
done
if [ -z "$cxx" ] || [ ! -f "$flags" ] || [ ! -d "$root/include/funkgui" ]; then
  echo "usage: check-headers.sh --cxx <compiler> --flags <file> --root <FunkGui source dir> [--bgfx] [--sdk <path>]" \
       "[--min-macos <version>] [--extra-flag <flag>]..." >&2
  exit 2
fi
root="$(cd "$root" && pwd)"
build="$(cd "$(dirname "$flags")" && pwd)"

includes=() defines=() plain_defines=()
while IFS= read -r line || [ -n "$line" ]; do
  case "$line" in
    "I "?*)
      dir="${line#I }"
      case "$dir" in
        "$root"/*|"$build"/*) includes+=(-I "$dir") ;;
        # The compiler's own directories (Linux: a pkg-config or Find module reports /usr/include): passing them again
        # reorders the search, and libstdc++'s #include_next <stdlib.h> then fails (v0.11.0).
        /usr/include|/usr/local/include) ;;
        *) includes+=(-isystem "$dir") ;;
      esac ;;
    "D "?*)
      def="${line#D }"
      defines+=("-D$def")
      case "$def" in FUNKGUI_PRODUCT_NAME=*|FUNKGUI_OBJC_PREFIX=*|FUNKGUI_ENV_PREFIX=*|FUNKGUI_PREFS_FOLDER=*) ;;
                     *) plain_defines+=("-D$def") ;;
      esac ;;
  esac
done < "$flags"

common=(-std=c++20 -fsyntax-only -Wall -Wextra -Wshadow -Wpedantic -Werror -ffp-contract=off)
[ -n "$sdk" ] && common+=(-isysroot "$sdk")
[ -n "$minos" ] && common+=("-mmacosx-version-min=$minos")
common+=(${extra[@]+"${extra[@]}"})

# Snapshot headers that cannot be standalone yet and whose fix belongs to a later card. Each entry is
# "<header>|<include it still has>|<reason>": skipped (and reported) only while the header still has that include.
exceptions=(
  "include/funkgui/core/TypeScale.h|#include \"SdfCanvas.h\"|snapshot file includes the bgfx-coupled gpu/SdfCanvas.h\
 by stem; G2 includes text/TextStyle.h instead (02 §2.2)"
)

log="$build/fg-headers.last.log"                          # the last compile's output, kept for inspection
checked=0 passed=0 failed=0 skipped=0

echo "fg.headers: $root/include/funkgui (bgfx: $([ $bgfx -eq 1 ] && echo yes || echo no); $cxx)"
while IFS= read -r header; do
  rel="${header#"$root"/}"
  if [ $bgfx -eq 0 ] && [[ "$rel" == include/funkgui/gpu/* ]]; then
    echo "SKIP  $rel  (gpu/ needs FUNKGUI_WITH_BGFX)"
    skipped=$((skipped + 1))
    continue
  fi
  skip_reason=""
  for entry in "${exceptions[@]}"; do
    IFS='|' read -r ex_header ex_include ex_reason <<< "$entry"
    if [ "$rel" = "$ex_header" ] && grep -qF "$ex_include" "$header"; then
      skip_reason="$ex_reason"
    fi
  done
  if [ -n "$skip_reason" ]; then
    echo "SKIP  $rel  ($skip_reason)"
    skipped=$((skipped + 1))
    continue
  fi
  lang=c++-header
  grep -qE '^[[:space:]]*(@interface|@class|@protocol|#import)' "$header" && lang=objective-c++-header
  checked=$((checked + 1))
  if "$cxx" "${common[@]}" ${includes[@]+"${includes[@]}"} ${defines[@]+"${defines[@]}"} -x "$lang" "$header" \
       > "$log" 2>&1; then
    echo "PASS  $rel"
    passed=$((passed + 1))
  else
    echo "FAIL  $rel"
    sed 's/^/      /' "$log"
    failed=$((failed + 1))
  fi
done < <(find "$root/include/funkgui" -name '*.h' | LC_ALL=C sort)

# Negative check: an unconfigured consumer must not compile (02 §1.8).
checked=$((checked + 1))
if "$cxx" "${common[@]}" ${includes[@]+"${includes[@]}"} ${plain_defines[@]+"${plain_defines[@]}"} -x c++-header \
     "$root/include/funkgui/core/Config.h" > "$log" 2>&1; then
  echo "FAIL  negative: include/funkgui/core/Config.h compiled without funkgui_configure_product()"
  failed=$((failed + 1))
elif grep -q 'call funkgui_configure_product()' "$log"; then
  echo "PASS  negative: include/funkgui/core/Config.h without the seams fails with its #error"
  passed=$((passed + 1))
else
  echo "FAIL  negative: include/funkgui/core/Config.h failed without the expected #error:"
  sed 's/^/      /' "$log"
  failed=$((failed + 1))
fi

echo "fg.headers: $checked checked, $passed passed, $failed failed, $skipped skipped"
[ $failed -eq 0 ]

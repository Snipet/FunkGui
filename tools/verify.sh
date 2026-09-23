#!/usr/bin/env bash
# tools/verify.sh <build>: FunkGui's definition-of-done gate (FCompressor docs/design/03-build-verify-process.md §4.7;
# 02 §1.10). It runs the `verify` label of an already-built tree and classifies every result:
#
#   1. wipes <build>/golden-candidates and <build>/probe-results, so nothing stale is reported;
#   2. ctest -L verify -j4 (never stops at the first failure; JUnit written to <build>/verify-junit.xml);
#   3. tools/golden.py report: BLOCKING (spec_fail, harness_error, crash, timeout, a disabled or not-run test),
#      DRIFT and MISSING (golden candidates: allowed only with a one-line reason per key group in the handoff),
#      IMPROVED, and the 10 slowest tests.
#
# Exit 0 when nothing is blocking. --quick is refused (a partial row set is not evidence).
#
#   cd "$FWT" && cmake --workflow --preset agent-gui-verify && tools/verify.sh "$FWT/build-agent-gui"
set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"

usage() {
  echo "usage: tools/verify.sh <build-dir>" >&2
  exit 2
}

[ $# -eq 1 ] || usage
case "$1" in
  --quick) echo "tools/verify.sh: --quick is refused: the gate needs every row (03 §4.7)" >&2; exit 2 ;;
  -*) usage ;;
esac
[ -d "$1" ] || { echo "tools/verify.sh: no build directory $1" >&2; exit 2; }
build="$(cd "$1" && pwd)"
[ -f "$build/CTestTestfile.cmake" ] || { echo "tools/verify.sh: $build is not a configured FunkGui build" >&2; exit 2; }

rm -rf "$build/golden-candidates" "$build/probe-results"
mkdir -p "$build/golden-candidates" "$build/probe-results"
junit="$build/verify-junit.xml"
rm -f "$junit"

echo "== tools/verify.sh: ctest -L verify -j 4 in $build"
ctest --test-dir "$build" -L verify -j 4 --output-on-failure --output-junit "$junit"
echo "== ctest exit $?"

python3 "$here/golden.py" report "$build" --junit "$junit" --golden-root "$root/test/golden"
rc=$?
if [ $rc -eq 0 ]; then
  echo "== tools/verify.sh: PASS (0 blocking)"
else
  echo "== tools/verify.sh: FAIL (blocking results above)"
fi
exit $rc

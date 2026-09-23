# FunkGui

A headless-testable SDF/bgfx GUI library for JUCE audio plug-ins, seeded from HardwareReverb's GUI
(`/Users/seanfunk/audio/plugins/HardwareReverb`, read-only). First consumer: FCompressor. The design is FCompressor's
`docs/design/02-funkgui-and-ui.md` (Part 1) and `docs/design/03-build-verify-process.md`.

## Status: v0.0.1 (bootstrap)

- `FunkGui::harness` (`include/funkgui/test/Harness.h`, namespace `funkgui::test`) is Harness v2: header-only,
  C++20, JUCE-free (03 §3.2).
- `FunkGui::core`, `FunkGui::gpu` and `FunkGui::presets` are **empty placeholder** INTERFACE targets, and
  `funkgui_configure_product()`, `funkgui_compile_shaders()` and `funkgui_add_font()` only print. They let a consumer
  write its final link lines now; v0.1.0 (task G1) gives them content.
- The snapshot sources under `include/funkgui/{core,text,gpu,prefs}`, `src/`, `shaders/`, `fonts/` and `tools/` are
  HR's files after mechanical renames. Nothing compiles them yet.
- Options (02 §1.7): `FUNKGUI_WITH_BGFX` (ON), `FUNKGUI_WITH_PRESETS` (ON), `FUNKGUI_HARNESS_ONLY` (OFF),
  `FUNKGUI_BUILD_TOOLS` (top-level). `FUNKGUI_VERSION` is a `CACHE INTERNAL` variable the consumer can read.

Consume it with FetchContent at a tag (FCompressor pins tag, SHA and version):

```cmake
FetchContent_Declare(FunkGui GIT_REPOSITORY /Users/seanfunk/audio/libraries/FunkGui GIT_TAG v0.0.1)
FetchContent_MakeAvailable(FunkGui)
target_link_libraries(my_probe PRIVATE FunkGui::harness)
```

Agents: read `CLAUDE.md` first.

## Unreleased: S0 review fixes

These are the consumer-visible rules added since v0.0.1. `CHANGELOG.md` → Unreleased has the full list.

- **Blessing stays with the lead.** A probe's `--bless-to` may not be `--golden-root`, lie inside it or contain it
  (harness error). `golden.py adopt` accepts only an `--allow-env` name of the form `*_ALLOW_BLESS`, only the statuses
  `pass`/`golden_drift`/`golden_missing`, and only candidates whose files match their results. It refuses Mode-scoped
  candidates unless the build's dsp.registry noted `"provisional"` (as `[]` when empty).
- **Goldens hold finite values only.** `num()` refuses NaN and ±Inf, and compares exactly the printed `%.9g` value.
- **Candidates are never stale.** Each probe run replaces its own candidate files and removes the rest.
- **The shaderc is stamped.** A given `FUNKGUI_SHADERC` needs `shaderc.stamp` = `bgfx.cmake <pinned SHA>` beside it.
  `fg.shader.hash` checks the stamp with a spec row.
- **The font data has hidden symbols.** `FunkGuiFonts` is built with hidden visibility, so no plug-in exports it.
- `report` and `diff` accept `--allow-env`, so FCompressor's `Scripts/golden.py` can pass it to every subcommand.

## Provenance

FunkGui starts as a byte-for-byte copy of HardwareReverb's GUI files (commit 1, "Snapshot of HardwareReverb GUI";
`SEED.tsv` lists every file with its HR path, FunkGui path, sha256 and HR mtime) followed by mechanical renames only
(commit 2, "Mechanical renames"). File stems stay HR's (`SdfCanvas.h` stays `SdfCanvas.h`), so a sed-normalised diff
against HR is empty at commit 2, and the new names (`Canvas.h`, ...) stay free for later work (02 §2.1–§2.3).

### The rename script (commit 2, exact)

Run from the FunkGui root on the commit-1 tree. It edits `*.h`, `*.cpp` and `*.mm` under `include/`, `src/` and
`tools/`; shaders, fonts and `tools/capture-frame.sh` are verbatim at v0.0.1 (02 §2.2). The `std::getenv` rule
applies to library files (`include/`, `src/`) only: library code reads the environment through `funkgui::env("<NAME>")`,
tools keep `std::getenv(FUNKGUI_ENV_PREFIX "<NAME>")` (02 §2.2, "env prefix").

```sh
SED="${TMPDIR:-/tmp}/funkgui-seed.sed"
cat > "$SED" <<'SEDSCRIPT'
# 1. Namespace: hrvbgui -> funkgui (hrvbgui::type -> funkgui::type)
s/hrvbgui/funkgui/g
# 2. Font binary data: HardwareReverbFonts.h / hrvbfonts -> FunkGuiFonts.h / funkguifonts
s/HardwareReverbFonts\.h/FunkGuiFonts.h/g
s/hrvbfonts/funkguifonts/g
# 3. Harness: hrvb:: -> funkgui::test::, namespace hrvb -> namespace funkgui::test
s/hrvb::/funkgui::test::/g
s/namespace hrvb([^A-Za-z0-9_]|$)/namespace funkgui::test\1/g
# 4. ObjC classes: static classes named through the product's ObjC prefix macro (G1 defines FUNKGUI_OBJC_NAME)
s/Hrvb(RenderView|DisplayLinkTarget)/FUNKGUI_OBJC_NAME(\1)/g
# 5. Prefs folder: "HardwareReverb" -> FUNKGUI_PREFS_FOLDER
s/"HardwareReverb"/FUNKGUI_PREFS_FOLDER/g
s|"Application Support/HardwareReverb/|"Application Support/" FUNKGUI_PREFS_FOLDER "/|g
s|Application Support/HardwareReverb/|Application Support/<PREFS_FOLDER>/|g
# 6. Env vars: HRVB_<NAME> -> FUNKGUI_ENV_PREFIX + <NAME> (library getenv -> funkgui::env is the -e rule, library files only)
s/"HRVB_([A-Z0-9_]+)"/FUNKGUI_ENV_PREFIX "\1"/g
s/HRVB_([A-Z0-9_]+)/<ENV_PREFIX>\1/g
# 7. Shader include path (02 §2.2, BgfxContext.cpp): <shaders/X.mtl.h> -> <funkgui/shaders/X.mtl.h>
s|<shaders/([A-Za-z0-9_]+)\.mtl\.h>|<funkgui/shaders/\1.mtl.h>|g
SEDSCRIPT
GETENV='s/std::getenv\("HRVB_([A-Z0-9_]+)"\)/funkgui::env("\1")/g'
LIB=$(find include src -type f \( -name '*.h' -o -name '*.cpp' -o -name '*.mm' \) | sort)
TOOLS=$(find tools -type f \( -name '*.h' -o -name '*.cpp' -o -name '*.mm' \) | sort)
for f in $LIB;   do sed -E -e "$GETENV" -f "$SED" "$f" > "$f.new"; cat "$f.new" > "$f"; rm "$f.new"; done
for f in $TOOLS; do sed -E -f "$SED" "$f" > "$f.new"; cat "$f.new" > "$f"; rm "$f.new"; done
```

Seams the renamed sources now name, all defined by `funkgui_configure_product()` / `include/funkgui/core/Config.h`
from v0.1.0 (02 §1.8): `FUNKGUI_ENV_PREFIX` (a string literal, e.g. `"FCMP_"`), `FUNKGUI_PREFS_FOLDER` (a string
literal), `FUNKGUI_OBJC_NAME(<Root>)` (the static ObjC class `<OBJC_PREFIX><Root>`, a token paste over
`FUNKGUI_OBJC_PREFIX`), and `funkgui::env()` (`include/funkgui/core/Env.h`). `<ENV_PREFIX>` and `<PREFS_FOLDER>`
appear in comments only.

Deliberately not renamed at v0.0.1: the `Theme` token `ice` (→ `signal` is generalisation work, 02 §2.2), HR tool
names and scratch paths in `tools/*.cpp` comments and strings (the tool ports rewrite them), and
`tools/capture-frame.sh` (generalised later with an `<ENV_PREFIX>` argument, 02 §2.2).

### Checking against HR (sed-normalised diff)

Empty for the commit-2 tree; HR's later migration diffs against the same baseline. `$SED` is the file written above.

```sh
HR=/Users/seanfunk/audio/plugins/HardwareReverb; FG=/Users/seanfunk/audio/libraries/FunkGui; REV=HEAD
GETENV='s/std::getenv\("HRVB_([A-Z0-9_]+)"\)/funkgui::env("\1")/g'
grep -v '^#' "$FG/SEED.tsv" | while IFS=$'\t' read -r hr fg sha mtime; do
  case "$fg" in
    include/*.h|include/*.cpp|include/*.mm|src/*.h|src/*.cpp|src/*.mm) norm() { sed -E -e "$GETENV" -f "$SED" "$1"; } ;;
    tools/*.h|tools/*.cpp|tools/*.mm)                                  norm() { sed -E -f "$SED" "$1"; } ;;
    *)                                                                 norm() { cat "$1"; } ;;
  esac
  diff -u --label "HR/$hr (sed)" --label "FunkGui/$fg@$REV" <(norm "$HR/$hr") <(git -C "$FG" show "$REV:$fg") || true
done
```

`include/funkgui/test/Harness.h` is replaced by Harness v2 in the "Bootstrap" commit, so from v0.0.1 on that one file
differs by design (`REV=3d1a2dd11bca`, the "Mechanical renames" commit, is the seed baseline).

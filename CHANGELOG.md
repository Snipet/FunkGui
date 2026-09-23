# Changelog

Every entry states its **golden impact** (`none`, `atlas`, or `geometry: <widgets>`) so consumers can plan
re-blessing (FCompressor docs/design/02-funkgui-and-ui.md §1.10). Tags are annotated `v0.MINOR.PATCH` on `main`.

## v0.2.0 — 2026-09-23 · S1: public UI API (FZ1) and core utilities

Golden impact: **none** (`fg.font.probe`, `fg.shader.hash` unchanged). Card G2.

- Public headers frozen at FZ1 (public declarations only; private sections marked provisional may grow in G3–G7):
  `core/{Geometry,Ease,Format}`, `text/{TextStyle,FontService,TextFit}`, `canvas/{Prim (84 B),PrimList,Canvas,Tags,
  Axis,Fingerprint,SoftRaster,Expand (Vtx 64 B)}`, `panel/{Input,Panel,HostServices,CaptureConfig,HeadlessHost}`,
  `params/{ParamPort,GestureController,JuceParamPort}`, `widgets/*` (9), `a11y/A11yItem`.
- Implemented: `GestureController` (begin/set/end, drag, wheel, `tapMany` bracketed by `HostServices::beginBatch/
  endBatch`), `JuceParamPort`, `ease`, `fmt` (U+2212), `text::width/fits/fitEllipsis`, `a11yDumpLine`,
  `FontAtlasSdf::baked()`.
- `Theme::ice` renamed `signal` (values bit-identical); `Col::fade/premix`; `TypeScale` via `text/TextStyle.h` (now
  standalone and covered by `fg.headers`).
- Tests: `fg.gesture`, `fg.paramport`, `fg.textfit`, `fg.format`, `fg.ease`, `fg.a11yline` (spec rows only).

## v0.1.0 — 2026-09-23 · S0: build system, tools on Harness v2, GPU build chain, review fixes

First tag FCompressor consumes. Goldens blessed: `fg.font.probe`, `fg.prefs.check`, `fg.shader.hash` (golden impact
for consumers: none). Includes card G1 (FunkGui CMake, presets, tools ported, `golden.py`, self-tests) and the S0
review fixes below.

### S0 review fixes (FCompressor docs/sprints/s0-review.md, R-G1 #1–#12)

Golden impact: **none** (FunkGui has no goldens yet; for consumers, no stored row changes meaning — rows that used to
drift against their own blessed value now pass, and rows that could never be valid are now refused).

- **Harness v2 (`include/funkgui/test/Harness.h`)**
  - `--bless-to` equal to, inside or containing `--golden-root` is a harness error (exit 4). Paths are compared after
    resolving symlinks and `..`, and by file identity on case-insensitive volumes (#3).
  - A probe's candidate files always come from its latest run. After writing, every other `<probe>.txt`, `<probe>.diff`
    and `<probe>.<key>.lines` in its candidate directory is removed, including after a partial (`--only`/`--quick`) or
    failed run. A `.diff` is now written on drift even when the run has no golden rows left (#4).
  - `num()` stores and compares the `"%.9g"` value the golden file holds, so a freshly blessed row no longer drifts
    under `le:0`/`ge:0`/`abs:0`/`rel:0` (#6).
  - `num()` refuses NaN and ±Inf. `text()` values and golden-file values spelled as a non-finite number
    (`[+-]inf|infinity|nan|nan(…)`, any case) are refused. A golden row with a numeric tolerance must hold a finite
    number (#7).
- **`tools/golden.py`**
  - `report` and `diff` accept (and ignore) `--allow-env`, so FCompressor's wrapper works for every subcommand (#1).
  - `adopt` refuses an `--allow-env` name that does not match `^[A-Z][A-Z0-9_]*_ALLOW_BLESS$` (#8).
  - `adopt` accepts only the statuses `pass`, `golden_drift` and `golden_missing` (an unreadable results file is
    refused). It refuses Mode-scoped candidates unless every build given has a results JSON with a `"provisional"`
    list: dsp.registry must `note("provisional", "[]")` even when no Mode is provisional (#5).
  - `adopt` refuses a candidate whose files do not hold the `golden_rows` its results report (stale files) (#4).
  - `selftest` checks the reason for each refusal, covers the refusals above and the wrapper's command line, and
    isolates itself from the user's git hooks and signing.
- **CMake**
  - `FunkGuiFonts` (juce_add_binary_data) is built with hidden visibility, so plug-ins no longer export its
    `funkguifonts::` symbols (#2).
  - A given `FUNKGUI_SHADERC` needs a `shaderc.stamp` beside it whose first line is `bgfx.cmake <pinned SHA>`: FATAL
    at top level, a WARNING in a consumer. `<funkgui/shaders/shaderc_stamp.h>` (generated) carries the stamp in use
    (#9).
- **Tests**
  - New `fg.golden.self` (`test/smoke/golden_py.cpp`: runs `tools/golden.py selftest`) (#10).
  - `fg.shader.hash`: new spec row `shaderc.stamp_is_pin`, so a shaderc from another bgfx fails the test (#9).
  - `fg.smoke.gpu` checks the class of the view `createRenderView` returns: rows `objc.render_view_is_nsview`,
    `objc.render_view_name_root` (the name starts with `<OBJC_PREFIX>RenderView`) and `objc.render_view_not_hr`. The
    `objc_getClass` rows are gone (`objc.render_view_class`, `objc.display_link_target_class`,
    `objc.hr_names_absent`, `surface.class`), so G7's randomised runtime class names pass unchanged (#11).
  - `fg.harness.self` pins #3, #4, #6 and #7 (140 spec rows).
- `CMakePresets.json`: `jobs: 6` with no `-l` for the agent build presets and 6 test jobs (#12; already on `main`
  as 0e083be).

## v0.0.1 — 2026-09-22 — Seed snapshot + bootstrap + Harness v2

Golden impact: **none** (no goldens exist yet).

- Snapshot of HardwareReverb's GUI (byte for byte, `SEED.tsv`), then mechanical renames only (script in
  `README.md` → Provenance). HR file stems are kept; the snapshot sources are not compiled yet.
- Bootstrap `CMakeLists.txt`: `FunkGui::harness` (INTERFACE, header-only); **empty placeholder** INTERFACE targets
  `FunkGui::core`, `FunkGui::gpu`, `FunkGui::presets`; `FUNKGUI_VERSION` (CACHE INTERNAL); options
  `FUNKGUI_WITH_BGFX`, `FUNKGUI_WITH_PRESETS`, `FUNKGUI_HARNESS_ONLY`, `FUNKGUI_BUILD_TOOLS`; placeholder functions
  `funkgui_configure_product`, `funkgui_compile_shaders`, `funkgui_add_font` that only print.
- `include/funkgui/test/Harness.h`: Harness v2 (03 §3.2.1–§3.2.4): `Tol` grammar, spec vs golden rows, `lines`
  sidecars, base + arch-overlay goldens, duplicate-key errors, `--bless-to` candidates written atomically, the
  `RESULT` JSON line and exit codes 0–4. Replaces HR's v1 harness (`--check`/`--bless`).

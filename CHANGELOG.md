# Changelog

Every entry states its **golden impact** (`none`, `atlas`, or `geometry: <widgets>`) so consumers can plan
re-blessing (FCompressor docs/design/02-funkgui-and-ui.md §1.10). Tags are annotated `v0.MINOR.PATCH` on `main`.

## v0.8.0 — S11: UI zoom (G7c; the lead merges G8's FunkPresets text here)

Golden impact (G7c): **new** `fg.gallery.zoom`; no existing golden moved (every `fg` golden, `fg.gallery.live` and the
legacy-hr parity are unchanged: with no zoom steps, and under `CANVAS_DUMP` without `UI_ZOOM`, the editor is exactly
v0.7.1's). Card G7c (FCompressor ADR-68, which revises ADR-06's "one `setSize`"). All additive.

- **UI zoom in `EditorHost`**: one machine-wide preference scales the whole panel uniformly while the Panel keeps its
  logical size W × H and draws, hit-tests and lists accessibility in its own px. At an effective zoom z the editor is
  round(W·z) × round(H·z); the drawable is sized at z × backing scale and the frame's `dpi` is that product, so pixel
  snapping (hairlines, text) lands on device px at the effective scale. Canvas, PrimList, dump v2 and fingerprints stay
  logical. Pointer, wheel, drag and file-drag positions reach the Panel divided by z; `showParamMenu` positions are
  multiplied back; `A11yBridge` children are placed at z × their items.
- `EditorConfig::{zoomSteps, defaultZoomPercent, zoomPrefKey}` (defaults: no steps = no zoom, 100, not persisted).
  FCompressor passes `{100, 125, 150, 175}`, `125`, `"uiZoom"` (UF1b); HardwareReverb passes nothing.
- `HostServices::zoomPercent()` (default 100), `setZoomPercent(int)` (default no-op), `zoomSteps()` (default empty
  `std::span<const int>`). `EditorHost` validates the step, writes the preference (`UiPreferences::setInt`), answers at
  once and resizes at the start of the next frame (`setSize`; the host resizes its window; the render view and
  drawable follow before that frame is recorded and submitted); every open editor follows the preferences revision,
  like the theme. A missing, damaged or unlisted preference reads as the default.
- Fit: a window larger than the user area of the editor's display is drawn at the largest step that fits; the
  preference is kept. `EditorHost::Diagnostics::zoomPercent` reports the effective zoom.
- Capture: `CaptureConfig::uiZoom` (`<PREFIX>UI_ZOOM=<percent>`, 25–400) pins the zoom (not persisted, not fitted);
  under `CANVAS_DUMP` the zoom is 100 % unless `UI_ZOOM` is set, so captures and `gui-live` are unchanged.
- `A11yBridge::setScale/scale`; `HeadlessHost::setZoom(steps, percent)` and `Log::zooms` (HeadlessHost stays logical:
  it only answers the three calls, for a Panel's ZOOM control).
- Gallery: section `zoom` (ZOOM cells over the host's steps, a readout, density samples); `FunkGuiGalleryApp` now has
  steps 100/125/150/175 (default 100, preference `uiZoom`), so the section resizes its window live.
- Tests `fg.editorhost.zoom` (gpu), `fg.editorhost.zoom.live` (gpu, live), `fg.gallery.zoom`; new
  `fg.editorhost.headless` rows.

## v0.7.1 — 2026-09-24 · S11: EditorHost follows ancestor moves; theme index, file drags, owner component

Golden impact: **none** (no drawing changed; every existing `fg` golden and `fg.gallery.live` unchanged). Card G7b.
All additive: every new virtual has a default, so a Panel or host written against v0.7.0 compiles and behaves as before.

- **Fix:** `EditorHost` keeps its render view on the editor when an *ancestor* moves inside the same window (a
  `juce::ComponentMovementWatcher` on the editor). JUCE's Standalone lays its content out under the title bar after the
  view attaches, which left the Metal view 27 px too high. Products can delete their own watchers (FCompressor U7's
  `Editor::AncestorWatcher`). Test `fg.editorhost.follow` (label `live`).
- `HostServices::themeIndex()` (default 0): the index of the Theme the next `draw()` receives — `EditorHost`: the
  `UI_THEME` override, else `UiPreferences::theme()`, so a theme cell's click reads the new index at once and the next
  frame ticks and draws with it; `HeadlessHost`: its constructor's index. Replaces matching the Theme's colours.
- `HostServices::ownerComponent()` (default `nullptr`): the `juce::Component` to anchor a `PopupMenu` or parent a
  `FileChooser` — `EditorHost` returns itself, `HeadlessHost` `nullptr`. Replaces product-side `setOwner()`.
- `Panel::filesDragEnter(paths, x, y)`, `filesDragMove(x, y)`, `filesDragExit()` (defaults ignore them): `EditorHost`
  now overrides JUCE's `fileDragEnter/Move/Exit` and forwards them like the drop (no exit follows a drop).
- Tests `fg.editorhost.headless` (core), `fg.editorhost.api` (gpu), `fg.editorhost.follow` (gpu, live).

## v0.7.0 — 2026-09-24 · S8: EditorHost, GPU sink, runtime ObjC names, live parity

Golden impact: **none** (35/35 `fg`; `fg.gallery.live` proves a live Metal capture's geometry, text and a11y hashes equal
the headless ones for `primitives`, `area`, `ruleslider`, a key-replay case and an overflow case). Card G7.

- `EditorHost` (surface lifecycle, fallback screen, retry, backing scale, `FramePump` client, diagnostics incl. the
  transient-buffer overflow counter; `CaptureConfig` read once from `<PREFIX>` env; fixed-dt capture; teardown order),
  `EditorConfig` (+ `beginBatch`/`endBatch`; size 0 = the Panel's).
- `A11yBridge` (VoiceOver tree from the Panel's a11y model) — the planned v0.7.1 split is not needed.
- `BgfxSink::submit` over `expand()`; `BgfxContext::configure()` (atlas from `FontService`, 32 MiB transient buffer).
- Objective-C classes registered at runtime as `juce::ObjCClass` under `<PREFIX>RenderView_…` / `<PREFIX>DisplayLinkTarget_…`
  with randomised suffixes (`fg.objc.names`), so two consumers in one host never collide.
- **Removed:** the snapshot `SdfCanvas` (the recorder `Canvas` + `BgfxSink` replace it).
- `tools/GalleryApp` (plain JUCE app hosting `GalleryPanel`), `tools/capture-frame.sh <app> <out.dump> <ENV_PREFIX>`.

## v0.6.0 — 2026-09-23 · S5: cell widgets and UI utilities

Golden impact: **new** `fg.gallery.{segmented,latch,theme,hint,dwell,lineedit}`; no existing golden moved
(`fg.prefs.check` unchanged: new int keys are more `<VALUE>` elements in the same file). Card G6.

- `SegmentedSelector` with `CellText`, `ParamCells` (choice parameter), `PrefCells` (int preference); `CellModel::help`;
  `SegmentedSelector::{settled,setSpokenTitle,bounds}`.
- `LatchToggle` (+ `settled`, `reason`) and `ParamToggle`; `ThemeCells`; `HintLine` (+ `remaining`, `alpha`, `kCut`).
- `DwellSelector` (+ `pinned`, `held`, `dwellLeft`) and `ScreenFader` (fixed dt, `kScreenFadeTau`).
- `LiveFeed<Frame>` + `LiveState` (staleness rule).
- `UiPreferences::{getInt,setInt,file,defaultFile}` (per-product folder, Q7); `reload()` bumps the revision on any key.
- `MenuLook` (+ `setTheme`), `text::LineEdit` (+ `set`), `text::printable` overloads.
- Tests `fg.livefeed`, `fg.lineedit`, `fg.dwell`; new `fg.prefs.check` rows.

## v0.5.0 — 2026-09-23 · S4: RuleSlider, AttachedWord, FocusRing

Golden impact: **new** `fg.gallery.{ruleslider,word,focusring}`; no existing golden moved. Card G5.

- `RuleSlider`: live / stepped / locked / derived / n/a states, hybrid end cells, rename / `+` / `~` / clamped markers,
  detent ticks with the adjacent-pair label fit rule (`detentLabelsFit`, shared with products' text-fit lints),
  index-space accessibility (0…n−1, step 1), arrows and PageUp/Down move one detent, wheel accumulation
  (`kWheelNotch` 0.10), drags land on detents, locked/derived/n/a refuse writes. Additive API: `pointerMove`,
  `pointerExit`, `settled`, `detentLabelsDrawn`, `kWheelNotch`, `kEndCell`, `kEndGap`.
- `AttachedWord` with `WordModel : ToggleModel` (`visible()`): hidden when n/a, disabled with its reason when locked.
- `FocusRing`. Test `fg.ruleslider.input` (201 spec rows).

## v0.4.0 — 2026-09-23 · S3: shapes, AREA shader, SoftRaster, FrameRender CLI, glyphs

Golden impact: **atlas** (`fg.font.probe` re-blessed: 10 glyphs appended via `Glyphs.def`; ASCII and HR's nine keep
their UVs, so `fg.gallery.primitives` is unchanged) and **shader** (`fg.shader.hash`: range-classified branch chain +
`KIND_AREA`; kinds 0–2 maths unchanged). New goldens `fg.gallery.area`, `fg.gallery.glyphs`. Card G4.

- `Canvas`: `area`, `areaStrip`, `polyline`, `disc`, `dotted`, `axis` (`src/canvas/CanvasShapes.cpp`).
- `shaders/fs_ui.sc`: `KIND_AREA = 3` with edge-stroke flags; `SoftRaster` CPU mirror in the same commit (flag decode
  proven equal for flags 0–7; kinds 0–2 differ from HR's FrameRender maths by 0 per channel).
- `HeadlessHost::writePng()` wired to SoftRaster. `funkgui_framerender`: `<dump> <png> [ss]`, `--fingerprint`, `--check`,
  `--legacy-hr`.
- Glyphs: `include/funkgui/text/Glyphs.def` (append-only) feeds `FontAtlasSdf::kExtraChars` (+10: µ − … ≤ ≥ ≈ Δ — • ←);
  subset regenerated reproducibly by `tools/subset-font.sh` (fonttools 4.65.0), proven equal to upstream by FontProbe.
- Test fixtures for "missing glyph" now use U+2206/U+21BA (absent upstream).

## v0.3.0 — 2026-09-23 · S2: recorder, dump v2, fingerprint, HeadlessHost, gallery

Golden impact: **new** `fg.gallery.primitives` (149 rows). Card G3. Spike passed: the recorder's vertex expansion is
bit-identical to HardwareReverb's `SdfCanvas` for every rrect/hairline/segment/text case at dpi 1, 1.5, 2.

- `Canvas` recorder (HR primitive maths byte-identical), tag/live scopes, dump v2 write/parse (reads HR v1 dumps; strict
  parser, C locale), `Fingerprint` (skips live prims, colours, tags and text gamma), `FontService` (CPU bake;
  `atlasHash()` equals FontProbe's), bgfx-free `expand()` (HR's a,b,c,a,c,d order).
- `HeadlessHost`: fixed dt, `settle()` (returns maxFrames+1 when it never settles), key/drag/wheel replay, host-call log,
  `writeDump`. `writePng()` returns false until SoftRaster (G4).
- Gallery: `GalleryPanel` with self-registering `gallery::Section`s and `FunkGuiGalleryProbe` (`--section`, scripted
  states, dpi 1/2, theme invariance, a11y lines); each widget card adds `fg.gallery.<section>`.
- Tests: `fg.canvas.parity`, `fg.canvas.expansion`, `fg.headless.settle`, `fg.gallery.primitives`.

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

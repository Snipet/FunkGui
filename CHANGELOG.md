# Changelog

Every entry states its **golden impact** (`none`, `atlas`, or `geometry: <widgets>`) so consumers can plan
re-blessing (FCompressor docs/design/02-funkgui-and-ui.md §1.10). Tags are annotated `v0.MINOR.PATCH` on `main`.

## Unreleased — for v0.12.0 · MINOR: FunkGui::core without JUCE

Golden impact: **none** (no row moves on macOS or Linux with JUCE; the JUCE-free configurations run the same rows
against the same goldens). New tests: `fg.font.blob`, `fg.font.baked` (macOS), `fg.prefs.backend`. Not tagged yet: the
tag follows the WebGL2 sink and the host services (FCompressor ADR-93, web Sprint B).

- **`FUNKGUI_WITH_JUCE`** (CMake option, default ON: nothing changes). OFF gives a `FunkGui::core` with no JUCE under
  it, for a host that is not a JUCE plug-in (the browser). `FunkGui::gpu` and `FunkGui::presets` need JUCE and are not
  defined there. Sources that need JUCE live in `src/juce/`, their counterparts in `src/nojuce/`. Consumers read
  `FUNKGUI_HAS_JUCE` (`funkgui/core/HasJuce.h`), an interface definition of `FunkGui::core` and `FunkGui::harness`.
- **The font atlas, baked ahead.** `FontAtlasSdf::serialise()` and `load()`; `fonts/FunkGuiAtlas-macos.bin` is the
  committed bake of the bundled face on macOS (`tools/AtlasBlob.cpp` writes it; `fg.font.baked` holds it to a fresh
  bake bit for bit). A JUCE-free build loads it, since it has no JUCE to rasterise glyphs.
- **Preferences behind a storage backend:** `UiPreferences::Backend`, `setBackend()`, `memoryBackend()`. With JUCE
  the default backend is the same properties file as before, read the same way (the theme still reads as `atoi`
  did). `file()` and `defaultFile()` exist only with JUCE.
- **`HeadlessGuiScope`** (`funkgui/panel/HeadlessGuiScope.h`): what a headless tool needs from the GUI toolkit, a
  JUCE message-manager scope with JUCE and nothing without.
- **Emscripten:** `CLocale` has a branch for it, the Harness knows `--arch wasm32`, and FunkGui's own `web` preset
  builds the JUCE-free tests as wasm32 and runs them under node. The `nojuce` preset is the same core natively, so the
  option stays honest on every gate.

## v0.11.1 — 2026-10-01 · PATCH: the SQLite target name under CMake before 4.3

Golden impact: **none**.

- `FunkPresets` linked `SQLite3::SQLite3`, which CMake's FindSQLite3 defines only from 4.3; up to 4.2 the target is
  `SQLite::SQLite3` (kept as a deprecated alias since). FunkGui asks for CMake 3.30, so a Linux configure with CMake
  3.30 to 4.2 failed to generate (FCompressor's first Linux CI run: Ubuntu 24.04, CMake 3.31.6). It now links whichever
  the running CMake defines. macOS and CMake 4.3 or later are unchanged.

## v0.11.0 — 2026-10-01 · MINOR: Linux

Golden impact: **none on macOS** (no macOS row moves; `fg.shader.hash` gains four SPIR-V rows, the same on every host).
New: `fg.font.probe-linux` and `fg.smoke.gpu` (Linux), each on Linux only. Linux x86-64 runs every other `fg` test
against the same goldens as macOS. (FCompressor ADR-92.)

- **The GPU layer on Linux:** bgfx on **Vulkan** into an X11 child window of JUCE's peer (`src/gpu/linux/`). The view
  is created on JUCE's display connection through JUCE's own dynamically loaded Xlib, selects no events (every event
  reaches JUCE's peer, as with JUCE's OpenGL child window), and is placed and sized in device pixels at the drawable's
  scale. JUCE's peers are X11 windows, so a Wayland desktop runs the editor through XWayland, like every JUCE plug-in.
  Vulkan only: bgfx's OpenGL path (EGL) aborts the process on any initialisation failure, its Vulkan path fails softly
  into `EditorHost`'s fallback screen. `BgfxContext` asks bgfx for Vulkan without its renderer fallback (with it, a
  failed Vulkan went on to that OpenGL path and then to Noop, so init never failed), and on every platform an init
  that came up on another renderer than the shaders' counts as failed. There is no display link on Linux: `FramePump` runs on its timer.
- **API (additive):** `nativeDisplay()` (bgfx's `PlatformData::ndt`: JUCE's X11 `Display*` on Linux, nullptr on macOS)
  and `setRenderViewScale(view, scale)` (the scale `EditorHost` sizes the drawable at, so on Linux the X window, the
  swapchain and the drawable agree even under a `UI_SCALE` override; nothing on macOS) in `gpu/NativeSurface.h`;
  `funkgui/core/CLocale.h` (`strtofC`, `strtolC`, `snprintfC`, `fprintfC`: C-locale number text on macOS and Linux,
  replacing direct calls to xlocale's `*_l` functions with a null `locale_t`, which is Apple's alone).
- **Shaders:** `FunkGuiShaders` compiles every profile on every host (`<name>.mtl.h` and `<name>.spv.h`). shaderc's
  output depends on the pinned bgfx only: the Metal pair compiled on Linux is the macOS golden byte for byte.
- **Storage on Linux:** `UiPreferences` in `~/.config/<PREFS_FOLDER>/preferences.settings` and `PresetStore` in
  `~/.config/<product>/Presets.db`, not JUCE's defaults (`~/<folder>/` and `~/.config/Application Support/`). The
  `XDG_CONFIG_HOME` variable is not read yet: JUCE 8.0.4 resolves the directory from `user-dirs.dirs`, never from the
  environment. The preset keys' fold uses GLib (full Unicode case fold, diacritics, width, NFC),
  the counterpart of CoreFoundation's, instead of the ASCII-only fallback.
- **Build:** configure accepts `FUNKGUI_WITH_BGFX` on Linux; bgfx is fetched without its Wayland backend (no
  `libwayland-egl` dependency) and compiled with `-w` there. Top-level builds default to Clang on Linux, turn off C++20
  module scanning, and apply `cmake/FunkGuiPlatform.cmake`: upstream Clang rejects the layout constructor template in
  JUCE 8.0.4's `juce_AudioPluginInstance.h`, so where it does, `-fdelayed-template-parsing` (and the silencing of its
  C++20 deprecation warning). `funkgui_add_font` copies the licences into the VST3 bundle's `Contents/Resources` and
  beside other executables at PRE_LINK on Linux. Tests: `*.mm` and `*_apple.cpp` register on Apple only, `*_linux.cpp`
  on Linux only; `fg.headers` skips the compiler's own include directories and takes `--extra-flag`.
- **Keys:** `EditorHost` delivers a chord whose text is a control character as its key code (`Key::character`, `ch`
  the key), as it already does for a chord with no text: X11 reports Ctrl-Z, the command chord on Linux, as 0x1A, so a
  Panel that tests `mods.cmd` and `ch == 'z'` sees it. `mods.cmd` and `mods.ctrl` are both set there (JUCE's command
  modifier is Ctrl off macOS).
- **Tools:** the gallery app puts its Section menu in the window on Linux; `PrefsCheck`, `fg.presets.store` and
  `fg.font.probe` know each platform's paths and atlas.

## v0.10.0 — 2026-09-30 · MINOR: the animation speed

Golden impact: **none** (the default scale is 1, which is bit for bit the eases as they were; every `fg` golden is
unchanged). `fg.ease` gains 12 spec rows.

- `ease::setTimeScale(float)` / `ease::timeScale()`: every tau that `ease::toward`, `hover` and `shown` take (and so
  every widget and `DwellSelector` / `ScreenFader`, which ease through them) is multiplied by it. 0 is no animation:
  each ease lands on its target in one call, as tau <= 0 always did. 2 is twice as slow. One value for the process,
  set on the message thread by the product from a machine-wide preference; values outside 0 … 8 (a NaN, a negative)
  are taken as 1. Only these eases read it, so meters, clocks and dwells keep their own time. (FCompressor ADR-90: an
  ANIMATION slider in the settings screen, whose fastest setting is none.)

## v0.9.0 — 2026-09-28 · MINOR: Canvas clipping

Golden impact: **none** (additive API; nothing records a clip unless it asks for one, and every `fg` golden is
unchanged). New spec-only test `fg.canvas.clip`.

- `Canvas::pushClip(const Rect&)` / `popClip()`, `Canvas::ClipScope` and `clipDepth()`: every primitive recorded
  between a push and its pop is cropped to the rectangle (intersected with the enclosing clips). The quad is cut at
  the rectangle and its local coordinates (a glyph's atlas uvs) are interpolated to the new corners, so each sample
  inside draws as the uncropped primitive's did; a primitive wholly outside is dropped, one wholly inside is kept bit
  for bit. It is done on the CPU at `popClip()`: `PrimList`, the dump, fingerprints, `SoftRaster` and `BgfxSink` see
  ordinary primitives, and no shader, vertex layout or dump format changes. Nested up to `Canvas::kMaxClips` (8);
  deeper pushes are ignored with their pops; `begin()` drops clips left open and `end()` closes them. Axis records
  are never clipped. The cut is a hard edge, so a caller puts it on a device px. (FCompressor ADR-84: smooth,
  pixel-exact scrolling of the preset browser.)
- `fg.canvas.clip`: a scene of every kind rasterised with and without a clip through all of it at dpi 1 and 2,
  supersample 1 and 2, is identical inside the clip and the clear colour outside; kept, dropped and cut primitives;
  nesting; the depth rules; `ClipScope`.

## v0.8.1 — 2026-09-25 · PATCH: golden.py lines-only probes; FunkPresets on Windows

Golden impact: **none** (tooling and Windows link only; no source a product compiles on macOS changed).

- **Fix:** `tools/golden.py` attributed the `.lines` sidecars of a probe that has no `.txt` by the file name's last
  dot, so a key holding dots (FCompressor `ui.a11y.chars.colour`) created a phantom probe and `adopt` refused the
  real one as stale. The probe's name now comes from the build's results. Self-test rows `discover.lines_only_dotted`
  and `adopt.lines_only_dotted` catch the old behaviour.
- FunkPresets links Windows' own SQLite (`winsqlite3`, `Normaliz`, `FUNKGUI_WINSQLITE=1`) instead of
  `find_package(SQLite3)` on Windows, as HR's preset layer did (from HR's migration phase 2). Untested on Windows here.

## v0.8.0 — 2026-09-24 · S11: FunkPresets (G8), UI zoom (G7c)

Golden impact: **new** `fg.gallery.zoom`; no existing golden moved (every `fg` golden, `fg.gallery.live` and the
legacy-hr parity are unchanged). New spec-only tests `fg.presets.{store,file,hooks}`. All additive (MINOR).

### FunkPresets (card G8)

- `FunkGui::presets` now has sources: HR's preset layer (S11.L1 snapshot, HR 34cb23b; SEED.tsv) generalised per
  FCompressor 01 §9.2; SQLite3 from the SDK; no GUI dependency.
- `ProductConfig{productName, fileExtension, xmlRoot, dbEnvVar}` + `isValid()`; `Attribute`, `Preset::{attributes, attr,
  setAttr}`.
- `PresetHooks{isPresetParameter, beginApply, applyBefore, onApplied, captureExtra, findFactory, initialPreset,
  mixParameter}`; `PresetManager(apvts, hooks)`: begin → before → values → identity → onApplied; `<PRESET>` state
  carries `ATTR`.
- `PresetStore(const ProductConfig&)`: Application Support/<productName>/Presets.db or `$<dbEnvVar>`; schema v2
  (attributes; min_reader 1); WAL, in-memory fallback, corrupt files set aside; read-only older files migrated in memory.
- `PresetFile::{toXmlString, fromXmlString, write, read}(config, …)`: `<xmlRoot plugin=productName>` with `ATTR` + `PARAM`;
  tags and timestamps never exported.
- Removed: `FactoryPresets.*` (product data). `Platform.h`/`Sqlite.h` private; `FUNKGUI_WINSQLITE` replaces
  `HRVB_WINSQLITE`.
- Tests: `fg.presets.store`, `fg.presets.file`, `fg.presets.hooks` (new `FUNKGUI_TEST` value `links=presets`).

### UI zoom (card G7c; FCompressor ADR-68, which revises ADR-06's "one `setSize`")

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
- `HostServices::zoomFits(percent)` (lead; default true): whether choosing that step would draw at it, so a ZOOM
  control can mark the steps the current display cannot show (EditorHost: a listed step that fits, or any step under
  a pin; `HeadlessHost::setZoomFitLimit` simulates a display).
- Capture: `CaptureConfig::uiZoom` (`<PREFIX>UI_ZOOM=<percent>`, 25–400) pins the zoom (not persisted, not fitted);
  under `CANVAS_DUMP` the zoom is 100 % unless `UI_ZOOM` is set, so captures and `gui-live` are unchanged.
- `A11yBridge::setScale/scale`; `HeadlessHost::setZoom(steps, percent)` and `Log::zooms` (HeadlessHost stays logical:
  it only answers the three calls, for a Panel's ZOOM control).
- Gallery: section `zoom` (ZOOM cells over the host's steps, a readout, density samples); `FunkGuiGalleryApp` now has
  steps 100/125/150/175 (default 100, preference `uiZoom`), so the section resizes its window live.
- Tests `fg.editorhost.zoom` (gpu), `fg.editorhost.zoom.live` (gpu, live), `fg.gallery.zoom`; new
  `fg.editorhost.headless` rows.

### Fix (lead, from the HardwareReverb migration)

- `funkgui_framerender --legacy-hr`: the six count rows (`layout.static_count`, `text_count`, `rank_strokes`,
  `segments`, `view_w`, `view_h`) declare `abs:0`, as HR's goldens hold them, instead of `exact`; `--check` against
  HR's unedited goldens now passes. No FunkGui golden holds these rows.

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

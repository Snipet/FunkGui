# FunkGui

![macOS 14+ on Apple Silicon](https://img.shields.io/badge/macOS-14%2B%20·%20Apple%20Silicon-555)
![Linux x86-64](https://img.shields.io/badge/Linux-x86--64%20·%20X11%20and%20Wayland-555)
![Browser: wasm32 · WebGL2](https://img.shields.io/badge/browser-wasm32%20·%20WebGL2-555)
![JUCE 8.0.4](https://img.shields.io/badge/JUCE-8.0.4-555)
[![Licence: GPL-3.0](https://img.shields.io/badge/licence-GPL--3.0-555)](LICENSE)

A GPU user-interface library for JUCE audio plugins on macOS and Linux. A plugin's interface is a `Panel` that draws
into a `Canvas`. The canvas records primitives on the CPU (rounded rectangles, lines, filled areas,
signed-distance-field text), and bgfx draws the recorded frame in one draw call, on Metal on macOS and on Vulkan on
Linux. The same Panel also runs headless, in a console process with a fixed clock and synthetic input, so a test can
check its layout, text and accessibility against golden files and render any frame to a PNG without a GPU. FunkGui also
has a small widget set, a preset store and the test harness itself.

From v0.12.0 the core also builds without JUCE, so the same Panel runs in a browser: compiled to WebAssembly by
Emscripten, on a `<canvas>`, where a WebGL2 sink draws the recorded frame in one draw call.

FunkGui is the GUI of [FCompressor](https://github.com/Snipet/FCompressor). HardwareReverb, a private plugin whose GUI
code FunkGui grew from, has moved onto it too. FunkGui is at 0.x, and its API may change between minor versions.
Every release is an annotated tag on `main` (latest: `v0.14.0`), and [`CHANGELOG.md`](CHANGELOG.md) says what each
release does to a consumer's golden files.

![Widgets from FunkGui's gallery: slider states, filled areas and a transfer curve, segmented selectors, latches and a focus ring](docs/images/gallery.png)

*Sections of the widget gallery, rendered headless by `FunkGuiGalleryProbe` and `funkgui_framerender` (no GPU, no
window), cropped and tiled.*

## How it works

```
Panel         your UI: one fixed logical size, tick(dt), draw(Canvas&, Theme&), input, accessibility items
  |
Canvas        records primitives on the CPU into a PrimList; makes no GPU calls
  |
  +-- EditorHost + BgfxSink   live: a juce::AudioProcessorEditor; bgfx on Metal or Vulkan, one draw call per frame
  +-- WebHost + WebGlSink     browser: a <canvas>, no JUCE; WebGL2, one draw call per frame
  +-- HeadlessHost            tests: fixed dt, synthetic input -> frame dump, fingerprint, PNG, accessibility list
```

- A Panel reads no wall clock: time comes from `tick(dt)`. Every ease snaps onto its target, so with a fixed dt a
  headless run settles in an exact number of frames and draws the same frame every time.
- A fingerprint hashes a frame's geometry and text, leaving out colours and live data, so a test can pin a layout in a
  few golden rows. A GPU test captures a real Metal frame of the gallery app and checks that its hashes equal the
  headless frame's. In a browser, a test page reads back what WebGL2 drew and compares it with `SoftRaster`'s image of
  the same frame.
- The host scales the Panel uniformly for the display's backing scale and for optional zoom steps; the Panel always
  draws and takes input in its own logical pixels.

## Features by layer

Public headers live in `include/funkgui/<layer>/`, in namespace `funkgui` (`funkgui::presets` for the presets,
`funkgui::test` for the harness).

- **core**: geometry, colours, two themes (graphite and paper), the type scale, deterministic eases with one
  process-wide animation speed, locale-free number formatting, environment variables read under a product prefix,
  and `FUNKGUI_HAS_JUCE` (`core/HasJuce.h`): 1 when JUCE is under the build, 0 without.
- **canvas**: the recorder. Rounded rectangles (per-corner radii, borders, soft edges), hairlines snapped to device
  pixels, segments, polylines, filled areas and strips, discs, dotted rules and text. Each primitive carries a semantic
  tag and a live flag for tests. Nested clipping, a text dump format, fingerprints, and `SoftRaster`, a CPU rasteriser
  that mirrors the fragment shader.
- **text**: an SDF font atlas baked on the CPU, through JUCE, from the bundled JetBrains Mono subset (printable ASCII
  and 19 symbols), text measurement and fitting, and `LineEdit`, the state of a one-line text field. A build without
  JUCE loads `fonts/FunkGuiAtlas-macos.bin` instead, the committed bake from macOS (`fg.font.baked` holds it to a fresh
  bake, bit for bit).
- **panel**: the `Panel` interface; `HostServices`, what a Panel may ask of its host, including (from v0.12.0) a popup
  menu, a file chooser and the clipboard as plain calls that need no JUCE in the Panel, with `services()` saying which
  the host serves; `HeadlessHost`, which serves all three to a test (every call logged, a menu or a chooser pending
  until the test answers it); `HeadlessGuiScope`, what a headless program holds before it draws (JUCE's GUI
  initialiser with JUCE, nothing without); and the capture settings read from the environment.
- **params**: `ParamPort` (a host parameter as 0..1), `JuceParamPort` over a `juce::RangedAudioParameter`, and
  `GestureController`, which wraps every write in a begin/end gesture: drags, clicks, wheel bursts and batches of
  several parameters.
- **widgets**: `RuleSlider` (a label, a value and a 1 px rule with a caret; detents; live, stepped, locked, derived
  and n/a states), `SegmentedSelector`, `LatchToggle`, `AttachedWord`, `ThemeCells`, `HintLine`, `FocusRing` and
  `DwellSelector`. Widgets render models the product implements and write only through `GestureController`.
- **a11y**: the accessibility items a Panel lists, and their one-line text form for tests.
- **prefs**, **live**, **juce**: UI preferences (theme, zoom, integer keys) behind a storage backend: with JUCE a
  machine-wide file in `~/Library/Application Support/<product>/` (Linux: `~/.config/<product>/`), without JUCE memory
  for the life of the process unless the host installs another backend; `LiveFeed`, which tells a live telemetry
  stream from a stale one; `MenuLook`, the theme applied to JUCE popup menus.
- **gpu**: `EditorHost`, a `juce::AudioProcessorEditor` that runs one Panel on bgfx: Metal on a click-through NSView
  on macOS, Vulkan on an X11 child window of JUCE's peer on Linux (which is XWayland on a Wayland desktop). It handles
  the surface lifecycle with a no-GPU fallback screen, converts JUCE input, mirrors the accessibility items for the
  screen reader, offers UI zoom steps, serves the menu, the file chooser and the clipboard of `HostServices` through
  JUCE, and can capture frames. `FramePump` gives every editor in the process one clock (a display link on macOS, a
  timer on Linux) and one `bgfx::frame()` per tick. On macOS the Objective-C classes are registered at run time under
  randomised names, so two products using FunkGui in one host never collide.
- **web** (`FunkGui::web`; Emscripten only, no JUCE): `WebHost`, `EditorHost`'s counterpart on a `<canvas>`. It runs
  the frame clock on `requestAnimationFrame` (about 60 Hz while anything moves, 12 Hz idle, nothing while the document
  is hidden), sizes the canvas for the UI zoom and the device pixel ratio, converts the browser's pointer, wheel and key
  events by JUCE's rules, and serves menus (DOM elements in the request's theme) and the clipboard; it has no file
  chooser, no IME, no file drops and no accessibility mirror. `WebHostConfig::beforeTick` (v0.14.0) is a product's
  function called just before each tick of the Panel. `WebGlSink` draws a recorded frame with WebGL2 in one draw call,
  from shader text generated out of the same `shaders/*.sc`, and rebuilds its resources after a context loss.
  `installLocalStoragePrefs()` (`WebPrefs.h`) keeps the UI preferences in `localStorage`. `WebInput.h` and
  `WebClock.h` are the input and clock rules as header-only C++, tested natively and under node. Every header is plain
  C++ and compiles on every host; `WebHost`, `WebGlSink` and `installLocalStoragePrefs()` are defined only under
  Emscripten. The menu and the clipboard live behind `src/web/WebServices.h`, private to the host.
- **presets** (FunkPresets): a SQLite preset store shared by every plugin instance, a preset manager with product
  hooks, and an XML preset file format. It does not depend on the GUI layers.
- **test** (`test/Harness.h`): a header-only, JUCE-free probe harness. Spec rows are judged at once and never stored;
  golden rows are compared, with tolerances, against files under a golden root. Candidates are written outside the
  golden tree, each probe writes a JSON result, and exit codes run from 0 (pass) to 4 (harness error).

## Requirements

- **Platforms:** macOS 14 or later on Apple Silicon, and (from v0.11.0) x86-64 Linux. The GPU layer needs Metal on
  macOS and Vulkan on Linux; configure stops if `FUNKGUI_WITH_BGFX` is on anywhere else. The preset layer has Windows
  code paths (Windows' own SQLite) carried over from HardwareReverb; they are untested. The harness and golden tools
  also handle x86_64 and wasm32 (per-architecture golden overlays; `golden.py adopt` writes no wasm32 overlay).
- **Browser** (from v0.12.0): the JUCE-free core and `FunkGui::web`, compiled to wasm32 by Emscripten, need WebGL2.
  FunkGui's own browser pages have run in headless Chrome on macOS only (the sink's page on ANGLE's Metal and on
  SwiftShader).
- **Tools:** CMake 3.30 or later, Ninja, Python 3 and git; Xcode or its command-line tools (Apple clang, C++20) on
  macOS, Clang on Linux (the default there unless `CC`/`CXX` say otherwise). Upstream Clang rejects a constructor
  template in JUCE 8.0.4's `juce_AudioPluginInstance.h`; where it does, `cmake/FunkGuiPlatform.cmake` adds
  `-fdelayed-template-parsing`, and a consumer does the same in its own scope. The `web` preset also needs Emscripten
  (`em-config` on `PATH`, or a `CMAKE_TOOLCHAIN_FILE`) and node, which runs its tests; FunkGui pins no Emscripten
  version (FCompressor pins 6.0.3). The browser pages need Google Chrome or Chromium, and node 22 or later for
  `tools/web/check-page.mjs`.
- **Dependencies:** JUCE 8.0.4 (another version is a configure error unless `FUNKGUI_ALLOW_OTHER_JUCE=ON`);
  bgfx.cmake `v1.153.9385-561` (bgfx API 153 is checked); SQLite 3 for the presets (the macOS SDK's). On Linux also
  GLib (the preset keys' Unicode fold) and the development packages JUCE needs (ALSA, FreeType, Fontconfig, X11). A
  build with `FUNKGUI_WITH_JUCE=OFF` fetches neither JUCE nor bgfx and needs none of these.

## Using FunkGui from CMake

Consume it with FetchContent at a tag. Make JUCE available first: FunkGui never fetches JUCE for a consumer, it
compiles against yours and checks the version.

```cmake
include(FetchContent)

FetchContent_Declare(JUCE GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 8.0.4 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

# Options are normal variables set before FetchContent_MakeAvailable(FunkGui); these two are the defaults.
set(FUNKGUI_WITH_BGFX ON)
set(FUNKGUI_WITH_PRESETS ON)
FetchContent_Declare(FunkGui GIT_REPOSITORY https://github.com/Snipet/FunkGui.git GIT_TAG v0.14.0)
FetchContent_MakeAvailable(FunkGui)

juce_add_plugin(MyPlugin FORMATS AU VST3 Standalone PRODUCT_NAME "My Plugin")   # plus your usual arguments
target_link_libraries(MyPlugin PRIVATE FunkGui::core FunkGui::gpu FunkGui::presets)
funkgui_configure_product(MyPlugin PRODUCT MyPlugin OBJC_PREFIX MyPl ENV_PREFIX MYPL_ PREFS_FOLDER MyPlugin)
funkgui_compile_shaders(MyPlugin)   # builds the embedded Metal and SPIR-V shaders before MyPlugin
funkgui_add_font(MyPlugin)          # the font's and bgfx's licences into each bundle's Resources
```

For a browser, configure with Emscripten's toolchain (`emcmake cmake …`) and no JUCE:

```cmake
set(FUNKGUI_WITH_JUCE OFF)          # the JUCE-free core; FunkGui::gpu and FunkGui::presets are not defined
FetchContent_Declare(FunkGui GIT_REPOSITORY https://github.com/Snipet/FunkGui.git GIT_TAG v0.14.0)
FetchContent_MakeAvailable(FunkGui)

add_executable(my_ui ui.cpp)        # one module for the page: my_ui.js and my_ui.wasm
target_link_libraries(my_ui PRIVATE FunkGui::core FunkGui::web)
target_link_options(my_ui PRIVATE -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH=1)
funkgui_configure_product(my_ui PRODUCT MyPlugin OBJC_PREFIX MyPl ENV_PREFIX MYPL_ PREFS_FOLDER MyPlugin)
```

- **Targets.** `FunkGui::harness` (always), `FunkGui::core`, `FunkGui::gpu` (with `FUNKGUI_WITH_BGFX`),
  `FunkGui::presets` (with `FUNKGUI_WITH_PRESETS`), `FunkGui::web` (under Emscripten; linked by name), and `FunkGui`
  (core plus gpu). They are INTERFACE libraries that carry FunkGui's sources, so FunkGui compiles inside your target
  with your JUCE configuration, or with none. There is no install step and no package config.
- **Without JUCE.** `FUNKGUI_WITH_JUCE=OFF` is required under Emscripten (configure stops otherwise, unless
  `FUNKGUI_HARNESS_ONLY` is on) and also builds natively. `funkgui_add_font` does nothing there: a page carries the
  font's licence, `fonts/JetBrainsMono-LICENSE.txt`, itself. [`tools/GalleryWeb`](tools/GalleryWeb) is a complete page.
- **Link PRIVATE.** A PUBLIC link on a `juce_add_plugin` target would compile FunkGui again into every format wrapper.
- **Product identity.** Every target that links `FunkGui::core` calls `funkgui_configure_product()`, which sets the
  product name, the Objective-C class prefix, the environment-variable prefix and the preferences folder. Without it,
  FunkGui's sources fail to compile.
- **bgfx.** In a GPU configuration FunkGui uses your `bgfx` target if one exists and otherwise fetches bgfx.cmake
  itself. The shaders are compiled with bgfx's `shaderc`, which is built from source (about 2,000 CPU-seconds) unless
  `FUNKGUI_SHADERC` names a prebuilt one. A prebuilt one should have a `shaderc.stamp` beside it naming the pinned
  bgfx.cmake commit; FunkGui warns otherwise. If you declare bgfx yourself, also build its shader tool
  (`BGFX_BUILD_TOOLS` and `BGFX_BUILD_TOOLS_SHADER` on) or set `FUNKGUI_SHADERC`.
- `FUNKGUI_VERSION` holds the version, for your own checks.

| Option | Default | Effect |
| --- | --- | --- |
| `FUNKGUI_WITH_JUCE` | ON | OFF: `FunkGui::core` without JUCE; no bgfx, `FunkGui::gpu` or `FunkGui::presets` |
| `FUNKGUI_WITH_BGFX` | ON | `FunkGui::gpu`, the shaders and bgfx. OFF: a headless build with no GPU code |
| `FUNKGUI_WITH_PRESETS` | ON | `FunkGui::presets` (links SQLite 3) |
| `FUNKGUI_HARNESS_ONLY` | OFF | Only `FunkGui::harness`; JUCE is not needed (DSP-only builds) |
| `FUNKGUI_BUILD_TOOLS` | ON at top level | The tool targets, such as `funkgui_framerender` (excluded from `all`) |
| `FUNKGUI_BUILD_TESTS` | ON at top level | FunkGui's own `fg.*` tests (needs the tools) |
| `FUNKGUI_ALLOW_OTHER_JUCE` | OFF | A JUCE version other than 8.0.4 is a warning instead of an error |
| `FUNKGUI_SHADERC` | empty | Path to a prebuilt, stamped `shaderc` |
| `FUNKGUI_TRANSIENT_VB_MIB` | 32 | bgfx's transient vertex buffer, in MiB, shared by every editor in the process |
| `FUNKGUI_WERROR` | ON | `-Werror` on FunkGui's own sources in its tools and tests |
| `FUNKGUI_WEB` | OFF | Top level only (the `web` preset): Emscripten's toolchain, from `em-config` if none is given |

FCompressor's [`cmake/FcmpDeps.cmake`](https://github.com/Snipet/FCompressor/blob/main/cmake/FcmpDeps.cmake) is a
complete consumer, with a version and commit check on every dependency.

### A Panel, live, headless and in a browser

A sketch of the smallest Panel and its three hosts:

```cpp
#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Theme.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/panel/Panel.h>

class HelloPanel final : public funkgui::Panel
{
public:
    void attach(funkgui::HostServices&) override {}
    int  width() const override  { return 320; }               // logical px, fixed
    int  height() const override { return 120; }
    void tick(float /*dt*/) override {}                         // advance eases here
    bool wantsFullRate() const override { return false; }       // true while anything moves
    void draw(funkgui::Canvas& c, const funkgui::Theme& th) override
    {
        c.rrect(16, 16, 288, 88, 4, th.ink16);
        c.text("HELLO", 32, 40, funkgui::type::kLabel, th.ink100);
    }
    void accessibility(std::vector<funkgui::A11yItem>&) const override {}
    uint32_t a11yRevision() const override { return 0; }
    void a11yAction(uint32_t, funkgui::A11yAction, double) override {}
    void closeGestures() override {}
};

// Live: the AudioProcessor's editor (FunkGui::gpu, <funkgui/gpu/EditorHost.h>).
juce::AudioProcessorEditor* createEditor() override
{
    return new funkgui::EditorHost(*this, funkgui::EditorConfig{}, std::make_unique<HelloPanel>());
}

// Headless: a console test (FunkGui::core, <funkgui/panel/HeadlessHost.h>, <funkgui/panel/HeadlessGuiScope.h>).
const funkgui::HeadlessGuiScope gui;               // JUCE's GUI initialiser with JUCE (the atlas bakes through it)
HelloPanel panel;
funkgui::HeadlessHost host(panel);                 // theme 0, dpi 2
host.settle();                                     // tick at 1/60 s until nothing moves
host.draw();
host.writePng("hello.png");                        // the CPU rasteriser, no GPU; false without JUCE (no PNG encoder)

// In a browser: the page's module (FunkGui::web, <funkgui/web/WebHost.h>, <funkgui/web/WebPrefs.h>), drawing on
// the page's <canvas id="canvas">. The runtime outlives main(), and so must the Panel and its host.
std::unique_ptr<HelloPanel> panel;
std::unique_ptr<funkgui::WebHost> host;

int main()
{
    funkgui::installLocalStoragePrefs(nullptr);    // before the Panel: the preferences in localStorage
    panel = std::make_unique<HelloPanel>();
    host = std::make_unique<funkgui::WebHost>(*panel, funkgui::WebHostConfig{});   // ok() is false without WebGL2
    host->start();                                 // requestAnimationFrame from here on
}
```

## Building and testing FunkGui

```sh
git clone https://github.com/Snipet/FunkGui.git
cd FunkGui
cmake --workflow --preset agent-verify        # headless (no bgfx): core, presets, tools and tests
tools/verify.sh build-agent                    # the gate: exits 0 when nothing is blocking
cmake --workflow --preset agent-gui-verify    # adds bgfx, the shaders and the GPU tests
tools/verify.sh build-agent-gui
cmake --workflow --preset nojuce-verify       # the core without JUCE, on the host compiler
tools/verify.sh build-nojuce
cmake --workflow --preset web-verify          # the core without JUCE as wasm32 (Emscripten); node runs the tests
tools/verify.sh build-web
```

Configure fetches JUCE 8.0.4 (and bgfx.cmake for the GPU presets) and checks their commits against the pins. When
`~/audio/.deps` (`FUNKGUI_DEPS_DIR`) holds them, as FCompressor's `Scripts/deps.sh` leaves it, that cache is used
instead, including a prebuilt `shaderc`. The `nojuce` and `web` presets fetch neither: they build `FunkGuiCoreCheck`
(every core source and, under Emscripten, `FunkGui::web`'s, under `-Werror` with no JUCE on the include path) and the
tools and tests that need no JUCE, and run those tests against the same goldens. The `lead` preset is the Release GPU
build used for tagging.

Every test is a CTest test labelled `verify`, and `tools/verify.sh` classifies the results: blocking (spec failure,
harness error, crash, timeout, disabled test), golden drift, missing golden, and the slowest tests. A test run never
writes `test/golden/`: new or changed rows become candidates in `<build>/golden-candidates`, and
`tools/golden.py adopt` moves reviewed candidates in with a stated reason (it requires `FUNKGUI_ALLOW_BLESS=1`).
Tests labelled `live` need a window on the GPU or a browser and are not part of `verify`; run the GPU ones with
`ctest --test-dir build-agent-gui -L live` in an unlocked desktop session.

node has no WebGL and no document, so `FunkGui::web` is checked in a browser. The `web` build also makes three pages
in `build-web/test/web`: the sink's (`index.html`: WebGL2 against `SoftRaster`, a forced context loss and restore),
the host's (`host.html`: `WebHost` against `HeadlessHost` over the gallery's sections, then its sizing, input,
services, teardown, clock and `beforeTick`) and the services' (`services.html`: the DOM menu, the clipboard and the
`localStorage` preferences). `tools/web/check-page.mjs` serves a page on 127.0.0.1, opens it in headless Chrome with a
throwaway profile, and exits 0 on the page's PASS, 1 on its FAIL and 2 when it could not run. CTest runs the three as
`fg.web.page`, `fg.web.host` and `fg.web.services` (label `live`):

```sh
tools/web/check-page.mjs build-web/test/web                 # the sink's page; --page host, --page services
ctest --test-dir build-web -L live --output-on-failure      # all three
```

None of FunkGui's pages makes a sound. Run a page that can (a consumer's, with an `AudioContext`) with
`--chrome-flag --mute-audio`; since any `--chrome-flag` replaces the runner's default GPU flag, give that too
(`--chrome-flag --use-angle=metal` on macOS, `--use-angle=swiftshader --enable-unsafe-swiftshader` elsewhere).

To look at the widgets, render a gallery section to PNGs:

```sh
B=build-agent
$B/FunkGuiGalleryProbe_artefacts/RelWithDebInfo/FunkGuiGalleryProbe --list
$B/FunkGuiGalleryProbe_artefacts/RelWithDebInfo/FunkGuiGalleryProbe fg.gallery.ruleslider \
    --dump-dir /tmp/fg --golden-root test/golden --arch arm64
$B/funkgui_framerender_artefacts/RelWithDebInfo/funkgui_framerender /tmp/fg/ruleslider.rest.dpi2.dump ruleslider.png
```

or run them live on the GPU. The gallery app takes `--section <name>`, and its Section menu switches sections:

```sh
build-agent-gui/tools/GalleryApp/FunkGuiGalleryApp_artefacts/RelWithDebInfo/FunkGuiGalleryApp.app/Contents/MacOS/FunkGuiGalleryApp --section ruleslider
```

or in a browser, from the `web` build (`?section=`, and the capture pins `theme`, `zoom`, `dt` and `scale`):

```sh
python3 -m http.server 8138 --bind 127.0.0.1 --directory build-web/tools/GalleryWeb
# then open http://127.0.0.1:8138/index.html?section=ruleslider
```

## Repository map

```
include/funkgui/     the public API, one directory per layer
src/                 the implementations, same layout; only src/gpu/ touches bgfx or Objective-C++
src/juce/, nojuce/   the core's sources that need JUCE, and their JUCE-free counterparts (FUNKGUI_WITH_JUCE=OFF)
src/web/             FunkGui::web, the only library sources that include Emscripten's headers
shaders/             the bgfx vertex and fragment shaders: one program, four primitive kinds; WebGL2 gets the same
                     program as generated GLSL ES 3.00 text
fonts/               the JetBrains Mono subset, the upstream face it was cut from, their licence, and
                     FunkGuiAtlas-macos.bin, the atlas baked on macOS for builds without JUCE
cmake/               FunkGuiDeps.cmake (pins and dependency checks), FunkGuiTargets.cmake (targets and functions),
                     FunkGuiPlatform.cmake, FunkGuiShaderText.cmake, FunkGuiEmbed.cmake
test/unit/, smoke/   the fg.* tests, each registered by its own first line
test/web/            the browser pages for FunkGui::web (fg.web.page, fg.web.host, fg.web.services)
test/gallery/        the widget gallery: one section per widget, run headless by the probe and live by the app
test/golden/         the blessed golden files
tools/               verify.sh, golden.py, funkgui_framerender, the gallery probe and app, FontProbe, AtlasDump,
                     AtlasBlob, PrefsCheck, capture-frame.sh, subset-font.sh, check-headers.sh
tools/GalleryWeb/    the gallery as a web page, over WebHost
tools/web/           check-page.mjs, which runs a page in headless Chrome
SEED.tsv             every file first copied from HardwareReverb, with its sha256
```

## Design and history

The design lives in FCompressor:
[`docs/design/02-funkgui-and-ui.md`](https://github.com/Snipet/FCompressor/blob/main/docs/design/02-funkgui-and-ui.md)
(Part 1 is FunkGui) and
[`docs/design/03-build-verify-process.md`](https://github.com/Snipet/FCompressor/blob/main/docs/design/03-build-verify-process.md)
(the harness, the golden files and the process). Code comments cite them as `02 §n` and `03 §n`. The core without
JUCE and `FunkGui::web` were made for FCompressor's browser demo, and its ADR-93
([`docs/DECISIONS.md`](https://github.com/Snipet/FCompressor/blob/main/docs/DECISIONS.md)) records why.
[`CLAUDE.md`](CLAUDE.md) holds the rules for the coding agents that work on the library.

FunkGui began as a byte-for-byte copy of HardwareReverb's GUI code followed by mechanical renames. `SEED.tsv` lists
each seeded file, and [`docs/PROVENANCE.md`](docs/PROVENANCE.md) has the rename script and the check against the
original.

## Used by

- [FCompressor](https://github.com/Snipet/FCompressor), an all-in-one compressor plugin (AU, VST3, Standalone). It is
  the reference consumer: each new FunkGui tag is pinned there and must pass FCompressor's full test gate, golden
  files included. Its browser demo runs the plugin's editor on `WebHost` and `WebGlSink`, without JUCE, and is
  published from FCompressor's `main` at <https://snipet.github.io/FCompressor/>.
- HardwareReverb, a private plugin.

![FCompressor, built with FunkGui](https://raw.githubusercontent.com/Snipet/FCompressor/main/docs/images/fcompressor.png)

## Licence

FunkGui is GPL-3.0 ([`LICENSE`](LICENSE)).

- The JetBrains Mono files in `fonts/` (the embedded subset and the upstream face) are under the SIL Open Font
  License 1.1 ([`fonts/JetBrainsMono-LICENSE.txt`](fonts/JetBrainsMono-LICENSE.txt)).
- JUCE is not part of this repository. A consumer provides it; JUCE 8's open-source licence is the AGPLv3. FunkGui's
  own build fetches it for its tools and tests (not in the `nojuce` and `web` presets).
- bgfx, bx and bimg (BSD-2-Clause) and bgfx.cmake (CC0-1.0) are fetched at configure time. `funkgui_add_font()`
  copies their licences and the font's into each plugin bundle.

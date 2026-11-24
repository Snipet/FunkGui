# FunkGui: rules for agents

FunkGui is a GUI library consumed by FCompressor and HardwareReverb through FetchContent at a pinned tag. The design
lives in FCompressor: `docs/design/02-funkgui-and-ui.md` Part 1 (layout, targets, seams, widgets),
`docs/design/03-build-verify-process.md` §3.2 (Harness v2) and §4 (process), and ADR-93 in `docs/DECISIONS.md` (the
core without JUCE, `FunkGui::web`). The card in your manifest,
`/Users/seanfunk/audio/plugins/FCompressor/docs/sprints/s<N>.md` or `web-*.md`, is your task: its `OWNS` line is the
only set of paths you may change.

## Hard rules

- **HardwareReverb is read-only.** `/Users/seanfunk/audio/plugins/HardwareReverb/**` is never edited, never built
  in, and never referenced by CMake. Reading it (or copying a file out of it when your card says so, with a
  `SEED.tsv` row and a sha256 checked that day) is fine.
- **Never bless.** Probes cannot write goldens; candidates go to `<build>/golden-candidates`. Never run
  `tools/golden.py adopt`, never set `FUNKGUI_ALLOW_BLESS`, never edit `test/golden/**`. Explain every candidate
  group in your handoff instead.
- **Git:** no `push`, `rebase`, `tag`, `worktree add/remove/prune` or `gc`, and no `commit` but ONE handoff commit on
  your own branch when the lead's prompt asks for it. Change no ref except your own branch. The lead merges, tags and
  bumps `project(FunkGui VERSION …)` and `CHANGELOG.md`.
- **Never edit `SEED.tsv`** or `docs/PROVENANCE.md` (moved verbatim from the README's Provenance section at v0.10.0;
  they are HR's migration baseline).

## Worktrees (03 §4.4, §4.5)

- `/Users/seanfunk/audio/libraries/FunkGui` (this checkout, branch `main`) is the **lead's**. Agents never edit or
  build here.
- Your worktree and branch are the ones your card names, under `/Users/seanfunk/audio/libraries/FunkGui.wt/`
  (`s<N>-<task>` on branch `s<N>/<task>` in the numbered sprints), made by the lead from the base SHA in your card.
  Build only inside it (`<worktree>/build-<preset>`).
- `FunkGui.wt/pin-<sha7>` worktrees are lead-made, detached and read-only: they exist so FCompressor can configure
  against a pinned SHA (`-DFETCHCONTENT_SOURCE_DIR_FUNKGUI=…`). Never edit them, and never point an override at
  another agent's live worktree.
- When your card changes rendering or API, verify FCompressor in the lead-made read-only worktree
  `/Users/seanfunk/audio/plugins/FCompressor/.claude/worktrees/ro-<your worktree's name>`, configured with
  `-DFETCHCONTENT_SOURCE_DIR_FUNKGUI=<your worktree>`; build there, never edit it.
- Shells reset between calls: learn your worktree path once, write it literally into every command, and start each
  command with `cd "<worktree>" &&`.

## Code rules

- C++ namespace `funkgui` (`funkgui::type` for the type scale, `funkgui::test` for the harness, `funkgui::presets`
  for FunkPresets). No product names in library code: product identity comes only from the seams set by
  `funkgui_configure_product()` (`FUNKGUI_PRODUCT_NAME`, `FUNKGUI_OBJC_PREFIX`, `FUNKGUI_ENV_PREFIX`,
  `FUNKGUI_PREFS_FOLDER`; 02 §1.8).
- Environment variables are read only through `funkgui::env("<NAME>")` (prefix added, cached on first use); no
  `getenv` in drawing code.
- Anything that includes bgfx or Objective-C lives under `include/funkgui/gpu/` and `src/gpu/`; everything else must
  compile with `FUNKGUI_WITH_BGFX=OFF`. `include/funkgui/test/Harness.h` stays header-only and JUCE-free. The one
  exception is `src/desktop/` (v0.28.0), compiled only with `FUNKGUI_WITH_DESKTOP`, which needs bgfx: it may include
  bgfx's headers and `src/gpu/`'s private ones, and no Objective-C (macOS's Metal layer comes from SDL's C calls).
- An SDL header appears only in `src/desktop/`, `tools/GalleryDesktop/` and a test that links `FunkGui::desktop`
  (`links=desktop`); `include/funkgui/desktop/` is plain C++ with no SDL and no bgfx header, and compiles in every
  configuration (`fg.headers`). What needs no window (input rules, the menu overlay, the preferences' file, file
  patterns) stays free of SDL so that a test drives it without one.
- In the library, anything that needs JUCE lives under `gpu/`, `presets/`, `include/funkgui/juce/` or `src/juce/`
  (its JUCE-free counterpart in `src/nojuce/`), or behind `#if FUNKGUI_HAS_JUCE`; everything else must compile with
  `FUNKGUI_WITH_JUCE=OFF` (the `nojuce`, `desktop` and `web` presets, which leave out a tool or test that includes
  JUCE). In `src/gpu/` the JUCE-free files are the ones `cmake/FunkGuiTargets.cmake` lists (`_fg_gpu_without_juce`:
  the bgfx context, the sink, the shader library, the image program, `gpuInfo()`); a JUCE-free build compiles those and
  `src/gpu/nojuce/**`, and a new file there that names no JUCE symbol is added to that list. An
  Emscripten header appears only in `src/web/`, `test/web/` and `tools/GalleryWeb/`; `include/funkgui/web/` is plain
  C++ that compiles on every host (`fg.headers`).
- Public headers (`include/funkgui/**`) are API: a change needs the lead's approval and bumps MINOR at tagging;
  frozen headers (see the sprint plan's frozen sets) are proposed in the handoff, never edited.
- FunkGui's own tests are named `fg.*`, self-register from their first line, and build with `-ffp-contract=off`
  and `-Wall -Wextra -Wshadow -Wpedantic -Werror`.
- The UI program has two vertex shaders that make the same vertices (v0.26.0): `shaders/vs_ui.sc` over
  `canvas/Expand.h`'s six vertices per primitive, and `shaders/vs_ui_inst.sc` over one 80-byte instance each, which
  `BgfxSink` uses. A change to `Prim`, the vertex layout or `expand()` changes `instance()`, `instanceCorner()` and
  `vs_ui_inst.sc` with it, and `fg.canvas.instance` (CPU) and `fg.gpu.instancing` (Metal, pixel for pixel) keep them
  equal; `fs_ui.sc` serves both.

## The desktop target and the cloud (v0.28.0)

- `FunkGui::desktop` (`FUNKGUI_WITH_DESKTOP`, the `desktop` preset) is `DesktopHost` over SDL3 and the JUCE-free part
  of `FunkGui::gpu`. It and JUCE exclude each other: both define the three functions of `gpu/NativeSurface.h` that
  `BgfxContext` calls. SDL is pinned in `cmake/FunkGuiDeps.cmake` (tag and commit) and fetched by git; a pin never
  becomes an archive URL.
- A change to a JUCE build's targets is proved, not assumed: configure `agent` and `agent-gui` before and after with
  `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` and compare the compile commands (v0.28.0 added three test targets and
  changed none).
- **In a cloud container** (Linux, no GPU, no display, no `~/audio`): the environment's `FUNKGUI_DEPS_DIR` names the
  dependency cache (`bgfx.cmake-<tag>/`, `SDL-<tag>/`, `JUCE-<tag>/` and `tools/shaderc-<tag>/shaderc` with its
  `shaderc.stamp`); release archives by URL are refused there, git clones work. A window is opened on Xvfb with a
  window manager (openbox) and drawn by Mesa's software Vulkan (lavapipe): the real GPU path on a software driver.
  Say which machine ran what; macOS and Windows are proved by the `Desktop` workflow's builds and by a person running
  its artifacts, never by a claim.
- `.github/workflows/desktop.yml` builds and tests the `desktop` preset on macOS, Windows and Linux and keeps
  `funkgui-gallery-desktop-<platform>`. Every action is pinned to a commit SHA.

## Done means (03 §4.7)

The `agent`, `agent-gui` and `nojuce` presets build with no warnings, and `web` too when the card touches anything a
JUCE-free build compiles; `tools/verify.sh <build>` exits 0 for each (no spec failures, harness errors, crashes or
disabled tests); a card that touches `FunkGui::web` also passes its pages (`tools/web/check-page.mjs
<build-web>/test/web [--page host|services]`); a one-line reason for every candidate group; the cross-repo check above
when it applies; a handoff of at most 40 lines (task, worktree/branch, `git status --short`, verify summary,
candidates with reasons, interface-change requests, override SHA, PNG paths, known gaps).

- **The `web` preset:** Emscripten's system-library cache is shared by every worktree: on a lock or a half-written
  library wait and retry, never clear it.
- **A browser** is headless Chrome through `tools/web/check-page.mjs` only (its own throwaway profile, never a visible
  window). A page that can make sound (a consumer's) runs with `--chrome-flag --mute-audio`, and with the GPU flag
  that any flag replaces (`--chrome-flag --use-angle=metal` on macOS). Stop every server and browser you start.

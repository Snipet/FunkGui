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
  compile with `FUNKGUI_WITH_BGFX=OFF`. `include/funkgui/test/Harness.h` stays header-only and JUCE-free.
- In the library, anything that needs JUCE lives under `gpu/`, `presets/`, `include/funkgui/juce/` or `src/juce/`
  (its JUCE-free counterpart in `src/nojuce/`), or behind `#if FUNKGUI_HAS_JUCE`; everything else must compile with
  `FUNKGUI_WITH_JUCE=OFF` (the `nojuce` and `web` presets, which leave out a tool or test that includes JUCE). An
  Emscripten header appears only in `src/web/`, `test/web/` and `tools/GalleryWeb/`; `include/funkgui/web/` is plain
  C++ that compiles on every host (`fg.headers`).
- Public headers (`include/funkgui/**`) are API: a change needs the lead's approval and bumps MINOR at tagging;
  frozen headers (see the sprint plan's frozen sets) are proposed in the handoff, never edited.
- FunkGui's own tests are named `fg.*`, self-register from their first line, and build with `-ffp-contract=off`
  and `-Wall -Wextra -Wshadow -Wpedantic -Werror`.

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

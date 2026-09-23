# Changelog

Every entry states its **golden impact** (`none`, `atlas`, or `geometry: <widgets>`) so consumers can plan
re-blessing (FCompressor docs/design/02-funkgui-and-ui.md §1.10). Tags are annotated `v0.MINOR.PATCH` on `main`.

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

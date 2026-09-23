// FUNKGUI_TEST name=fg.golden.self timeout=300 gpu=0 exe=tools/golden.py args="selftest"
//
// Registration stub (test/CMakeLists.txt: a FUNKGUI_TEST line with exe= is not compiled). fg.golden.self runs
// `tools/golden.py selftest` as a plain test (its exit code is its result), so the gate covers golden.py's own checks:
// every adopt refusal (allow variable and its name, linked worktree, dirty golden tree, statuses, stale candidates,
// provisional Modes, ui.geometry before FZ5), the --x86 merge rules, report classification, and the command line a
// wrapper builds (S0 review R-G1 #10; FCompressor docs/design/03-build-verify-process.md §3.2.5). The selftest works in
// a scratch git repository under $TMPDIR and never touches test/golden or reads the real allow variable.

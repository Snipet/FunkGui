// FUNKGUI_TEST name=fg.prefs.check timeout=600 gpu=0 exe=FunkGuiPrefsCheck
//
// Registration stub (test/CMakeLists.txt: a FUNKGUI_TEST line with exe= is not compiled). fg.prefs.check runs
// tools/PrefsCheck.cpp (FCompressor docs/design/02-funkgui-and-ui.md §3.11) against a scratch store inside the test's
// sandbox: a write reaches the disk, reload() sees another process's write, an out-of-range theme is clamped, the real
// ~/Library/Application Support store is never touched, and the on-disk format is a golden row.

// FUNKGUI_TEST name=fg.font.probe timeout=900 gpu=0 exe=FunkGuiFontProbe args="@SOURCE_DIR@/fonts/upstream/JetBrainsMono-Regular.ttf"
//
// Registration stub (test/CMakeLists.txt: a FUNKGUI_TEST line with exe= is not compiled). fg.font.probe runs
// tools/FontProbe.cpp (FCompressor docs/design/02-funkgui-and-ui.md §3.11): the bundled face's atlas hash and metrics as
// golden rows; as spec rows, no glyph of the baked set missing, and the committed subset interchangeable with the
// upstream face it was cut from (same advances, bit-identical distance field).

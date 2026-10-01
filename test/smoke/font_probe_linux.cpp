// FUNKGUI_TEST name=fg.font.probe-linux timeout=900 gpu=0 exe=FunkGuiFontProbe args="@SOURCE_DIR@/fonts/upstream/JetBrainsMono-Regular.ttf"
//
// Registration stub (test/CMakeLists.txt: a FUNKGUI_TEST line with exe= is not compiled). fg.font.probe-linux is
// fg.font.probe (font_probe_apple.cpp) on Linux (v0.11.0): the same tool, rows and spec checks, under its own name so
// that its golden holds Linux's atlas. FontAtlasSdf rasterises each glyph with juce::Graphics into a native Image:
// CoreGraphics on macOS, JUCE's software renderer on Linux, so the distance field (font.atlas) differs by platform
// while the metric rows (font.cap, font.x, font.ascent, font.digit) hold the same values in both goldens.

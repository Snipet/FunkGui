// FUNKGUI_TEST name=fg.font.baked timeout=900 gpu=0 exe=FunkGuiAtlasBlob args="@SOURCE_DIR@/fonts/FunkGuiAtlas-macos.bin"
//
// Registration stub (test/CMakeLists.txt: a FUNKGUI_TEST line with exe= is not compiled). fg.font.baked runs
// tools/AtlasBlob.cpp (v0.12.0): the committed bake of the atlas, fonts/FunkGuiAtlas-macos.bin, which a build without
// JUCE embeds and draws with, must be the bake this machine makes now: no texel, no glyph record and no metric
// differs, and the file is byte for byte what FontAtlasSdf::serialise() writes. Spec rows only.
//
// macOS only, as fg.font.probe is: the bake is the platform's (CoreGraphics here, JUCE's software renderer on Linux,
// whose pixels differ), and the committed blob is macOS's. A JUCE test: the live bake needs JUCE, so it does not exist
// with FUNKGUI_WITH_JUCE=OFF, where fg.font.blob checks the embedded copy instead.

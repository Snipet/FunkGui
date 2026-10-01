#include <funkgui/text/FontAtlasSdf.h>

// FontAtlasSdf::bake without JUCE (v0.12.0): there is no typeface, shaper or path rasteriser in this build, so no face
// can be baked, and bake() says so the way it always has when it could not bake the face that was asked for: false,
// and an unbaked atlas. The atlas of such a build comes from FontAtlasSdf::load() (FontService loads the bake committed
// under fonts/). Compiled only with FUNKGUI_WITH_JUCE=OFF (src/nojuce/**); src/juce/FontAtlasBake.cpp is the bake.

namespace funkgui
{
    bool FontAtlasSdf::bake(const void*, size_t, const char*)
    {
        glyphs_.assign(static_cast<size_t>(kLastChar - kFirstChar + 1), {});
        extras_.assign(static_cast<size_t>(kNumExtra), {});
        pixels_.assign(static_cast<size_t>(kAtlasW) * kAtlasH, 0);
        usedEmbedded_ = false;
        baked_ = false;
        return false;
    }
}

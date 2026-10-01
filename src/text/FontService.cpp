#include <funkgui/text/FontService.h>

#include <funkgui/core/HasJuce.h>
#include <funkgui/text/BundledFont.h>

#include <cstddef>
#include <cstdint>

// The process-wide CPU bake of the bundled face (02 §3.4). JUCE rasterises the glyph coverage (FontAtlasSdf::bake), so
// the process must have JUCE's GUI side initialised before the first atlas() call: a plug-in host always has, a console
// probe holds a funkgui::HeadlessGuiScope (a juce::ScopedJuceInitialiser_GUI). One bake is attempted per process; a
// failure is not retried (it would fail the same way) and leaves an unbaked atlas, which text draws nothing with.
//
// Without JUCE (FUNKGUI_HAS_JUCE == 0, v0.12.0) nothing here can rasterise, so the atlas is the bake committed as
// fonts/FunkGuiAtlas-macos.bin, embedded by FunkGuiFonts and adopted with FontAtlasSdf::load(): macOS's bake of the
// same face, bit for bit (fg.font.baked holds the file to the live bake), so every glyph metric, UV and texel is the
// one a macOS plug-in draws with. A blob this build cannot adopt (another glyph set: the file was not regenerated
// after a Glyphs.def append) leaves the atlas unbaked, exactly as a failed bake does.

namespace funkgui
{
    namespace
    {
        // FNV-1a 64 over the bytes (HR's hash; FontProbe's font.atlas hashes the same R8 pixels the same way).
        uint64_t fnv1a(const uint8_t* p, size_t n) noexcept
        {
            uint64_t h = 1469598103934665603ull;
            for (size_t i = 0; i < n; ++i)
            {
                h ^= p[i];
                h *= 1099511628211ull;
            }
            return h;
        }
    }

    FontService& FontService::get()
    {
        // Never destroyed (HR idiom): a plug-in binary is unloaded while statics are torn down in an order nothing here
        // controls, and a Canvas may still hold a reference to the atlas.
        static FontService* const s = new FontService();
        return *s;
    }

    const FontAtlasSdf& FontService::atlas()
    {
        if (!attempted_)
        {
            attempted_ = true;
#if FUNKGUI_HAS_JUCE
            const bool made = atlas_.bake(BundledFont::data(), BundledFont::size());
#else
            const bool made = atlas_.load(reinterpret_cast<const uint8_t*>(funkguifonts::FunkGuiAtlasmacos_bin),
                                          static_cast<size_t>(funkguifonts::FunkGuiAtlasmacos_binSize));
#endif
            if (made)
            {
                const auto& px = atlas_.pixels();
                hash_ = fnv1a(px.data(), px.size() * sizeof(px[0]));
            }
        }
        return atlas_;
    }

    bool FontService::ok() const
    {
        return atlas_.baked() && atlas_.usedEmbeddedFace();
    }

    uint64_t FontService::atlasHash() const
    {
        return hash_;
    }
}

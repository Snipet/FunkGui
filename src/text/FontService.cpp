#include <funkgui/text/FontService.h>

#include <funkgui/text/BundledFont.h>

#include <cstddef>
#include <cstdint>

// The process-wide CPU bake of the bundled face (02 §3.4). JUCE rasterises the glyph coverage (FontAtlasSdf::bake), so
// the process must have JUCE's GUI side initialised before the first atlas() call: a plug-in host always has, a console
// probe holds a juce::ScopedJuceInitialiser_GUI. One bake is attempted per process; a failure is not retried (it would
// fail the same way) and leaves an unbaked atlas, which text draws nothing with.

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
            if (atlas_.bake(BundledFont::data(), BundledFont::size()))
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

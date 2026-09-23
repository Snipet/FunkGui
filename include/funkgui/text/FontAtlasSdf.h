#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace funkgui
{
    // Signed-distance-field font atlas, baked once at runtime. JUCE is used
    // only to rasterise glyph coverage offscreen; the distance transform and
    // everything on screen is ours. Distance is mapped to [0,1] with the
    // glyph edge at 0.5 and a spread of kSpread texels.
    class FontAtlasSdf
    {
    public:
        static constexpr int   kAtlasW   = 1024;
        static constexpr int   kAtlasH   = 512;
        static constexpr float kBasePx   = 48.0f;   // rasterisation size
        static constexpr int   kSpread   = 6;       // SDF radius, texels
        static constexpr int   kFirstChar = 33;
        static constexpr int   kLastChar  = 126;

        // Codepoints past ASCII that the UI actually needs. Kept as an
        // explicit list rather than a range so the atlas only pays for
        // glyphs that get drawn.
        static constexpr uint32_t kExtraChars[] = {
#define FUNKGUI_GLYPH(cp, name) cp,
#include <funkgui/text/Glyphs.def>
#undef FUNKGUI_GLYPH
        };
        static constexpr int kNumExtra =
            static_cast<int>(sizeof(kExtraChars) / sizeof(kExtraChars[0]));

        struct Glyph
        {
            float u0 = 0, v0 = 0, u1 = 0, v1 = 0;   // atlas uv
            float w = 0, h = 0;                     // quad size at base px
            float bx = 0, by = 0;                   // bearing from the pen
            float advance = 0;
        };

        // Bakes from an embedded TrueType/OpenType file when ttfData is given.
        // That is the shipping path: the system default's metrics, shapes and
        // glyph coverage vary by macOS version and user settings, so without a
        // bundled face the product literally renders differently machine to
        // machine. faceName names an installed face instead, for comparing
        // candidates; with neither, the system default is used.
        // Returns false if it could not bake the face that was ASKED for —
        // in particular, supplying ttfData that the system cannot turn into a
        // typeface is a failure, not a licence to quietly substitute the
        // system font. Substituting silently would reproduce exactly the
        // machine-to-machine divergence that bundling a face exists to
        // prevent, and would do it invisibly.
        bool bake(const void* ttfData = nullptr, size_t ttfBytes = 0,
                  const char* faceName = nullptr);

        // Whether the last successful bake used the caller's embedded bytes.
        bool usedEmbeddedFace() const { return usedEmbedded_; }

        // Whether the last bake() succeeded, so glyphs, metrics and pixels()
        // are real (02 §3.3: text needs only this, no GPU texture). False
        // before the first bake and after a failed one.
        bool baked() const { return baked_; }

        // Codepoint lookup. ASCII hits a flat array; the handful of extras
        // are linear-searched, which at kNumExtra entries beats any map.
        // nullptr for a codepoint outside the baked set, and for every
        // codepoint before the first bake() (the tables are still empty).
        const Glyph* glyph(uint32_t cp) const
        {
            if (cp >= kFirstChar && cp <= kLastChar)
                return glyphs_.empty() ? nullptr : &glyphs_[cp - kFirstChar];
            for (size_t i = 0; i < static_cast<size_t>(kNumExtra) && i < extras_.size(); ++i)
                if (kExtraChars[i] == cp) return &extras_[i];
            return nullptr;
        }

        float spaceAdvance() const { return spaceAdvance_; }
        float ascent()       const { return ascent_; }
        float descent()      const { return descent_; }
        float lineHeight()   const { return lineHeight_; }

        // Cap height and x-height, measured from the baked outlines. Optical
        // centring needs the cap height, not the font's full line box —
        // centring on lineHeight leaves text visibly high in a button.
        float capHeight()    const { return capHeight_; }
        float xHeight()      const { return xHeight_; }

        // Widest of '0'..'9'. Drawing every digit on this advance keeps a
        // column of live numbers from shimmering sideways as its digits
        // change — the system UI font is proportional, so "1" is narrower
        // than "8" and an un-tabulated readout jitters on every update.
        float maxDigitAdvance() const { return maxDigitAdvance_; }

        const std::vector<uint8_t>& pixels() const { return pixels_; }

    private:
        std::vector<Glyph>   glyphs_;   // ASCII kFirstChar..kLastChar
        std::vector<Glyph>   extras_;   // parallel to kExtraChars
        std::vector<uint8_t> pixels_;
        float spaceAdvance_ = 12.0f;
        float ascent_       = 34.0f;
        float descent_      = 10.0f;
        float lineHeight_   = 56.0f;
        float capHeight_    = 34.0f;
        float xHeight_      = 24.0f;
        float maxDigitAdvance_ = 26.0f;
        bool  usedEmbedded_ = false;
        bool  baked_ = false;
    };
}

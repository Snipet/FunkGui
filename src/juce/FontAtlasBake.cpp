#include <funkgui/text/FontAtlasSdf.h>

#include "../text/SdfField.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <cmath>
#include <vector>

// FontAtlasSdf::bake (v0.12.0: moved here from src/text/FontAtlasSdf.cpp, unchanged). JUCE makes the typeface, shapes
// each glyph and fills its outline into a single-channel image; the distance transform (src/text/SdfField.h) and the
// field written from it are FunkGui's own. This file is compiled only with FUNKGUI_WITH_JUCE (src/juce/**): a build
// without JUCE has no rasteriser, so its bake() (src/nojuce/FontAtlasBake.cpp) refuses and the atlas comes from
// FontAtlasSdf::load().

namespace funkgui
{
    bool FontAtlasSdf::bake(const void* ttfData, size_t ttfBytes, const char* faceName)
    {
        using namespace juce;

        glyphs_.assign(static_cast<size_t>(kLastChar - kFirstChar + 1), {});
        extras_.assign(static_cast<size_t>(kNumExtra), {});
        pixels_.assign(static_cast<size_t>(kAtlasW) * kAtlasH, 0);

        usedEmbedded_ = false;
        baked_ = false;               // until this bake has proved itself below

        FontOptions opts = FontOptions { kBasePx };
        if (ttfData != nullptr && ttfBytes > 0)
        {
            // Registered for the lifetime of the returned Ptr, which
            // FontOptions holds by value until the bake below finishes; the
            // typeface takes its own copy of these bytes, so nothing here
            // outlives the call.
            auto tf = Typeface::createSystemTypefaceFor(ttfData, ttfBytes);
            if (tf == nullptr)
                return false;      // asked for a specific face, could not get it
            opts = FontOptions{}.withTypeface(tf).withHeight(kBasePx);
            usedEmbedded_ = true;
        }
        else if (faceName != nullptr && *faceName != 0)
        {
            opts = FontOptions{}.withName(faceName).withHeight(kBasePx);
        }
        Font font { opts };
        ascent_     = font.getAscent();
        descent_    = font.getDescent();
        lineHeight_ = font.getHeight();

        int penX = 1, penY = 1, rowH = 0;
        bool overflow = false;

        // Rasterise one glyph and write its SDF into the atlas. Returns false
        // only when the atlas is genuinely out of room.
        auto bakeOne = [&](juce_wchar cp, Glyph& g) -> bool
        {
            GlyphArrangement one;
            one.addLineOfText(font, String::charToString(cp), 0.0f, 0.0f);
            if (one.getNumGlyphs() < 1) return true;   // nothing to draw

            // PositionedGlyph::w is the glyph's own shaped advance. The old
            // code took the difference between consecutive glyph *lefts* in
            // one long run of the whole charset, which folded the kerning of
            // whatever arbitrary pair happened to be adjacent into every
            // advance.
            g.advance = one.getGlyph(0).getRight() - one.getGlyph(0).getLeft();

            Path path;
            one.createPath(path);
            const auto pb = path.getBounds();
            if (pb.isEmpty()) return true;             // whitespace

            const int pad = kSpread + 1;
            const int gw  = static_cast<int>(std::ceil(pb.getWidth()))  + pad * 2;
            const int gh  = static_cast<int>(std::ceil(pb.getHeight())) + pad * 2;

            Image img(Image::SingleChannel, gw, gh, true);
            {
                Graphics gr(img);
                gr.setColour(Colours::white);
                gr.fillPath(path, AffineTransform::translation(
                    static_cast<float>(pad) - pb.getX(),
                    static_cast<float>(pad) - pb.getY()));
            }

            if (penX + gw + 1 >= kAtlasW) { penX = 1; penY += rowH + 1; rowH = 0; }
            if (penY + gh + 1 >= kAtlasH) return false;
            rowH = std::max(rowH, gh);

            const size_t GW = static_cast<size_t>(gw), GH = static_cast<size_t>(gh);
            const size_t area = GW * GH;
            std::vector<float> cov(area);
            {
                Image::BitmapData bd(img, Image::BitmapData::readOnly);
                for (int y = 0; y < gh; ++y)
                {
                    const uint8* src = bd.getLinePointer(y);
                    for (int x = 0; x < gw; ++x)
                        cov[static_cast<size_t>(y) * GW + static_cast<size_t>(x)] =
                            static_cast<float>(src[x * bd.pixelStride]) * (1.0f / 255.0f);
                }
            }

            // Two EDTs: distance to the nearest outside texel and to the
            // nearest inside texel. Sites are the zeros of each grid.
            std::vector<float> dIn(area), dOut(area);
            for (size_t i = 0; i < area; ++i)
            {
                const bool inside = cov[i] > 0.5f;
                dIn [i] = inside ? detail::kSdfInf : 0.0f;   // sites = outside texels
                dOut[i] = inside ? 0.0f : detail::kSdfInf;   // sites = inside texels
            }
            detail::sdfEdt2d(dIn,  gw, gh);
            detail::sdfEdt2d(dOut, gw, gh);

            for (int y = 0; y < gh; ++y)
            {
                for (int x = 0; x < gw; ++x)
                {
                    const size_t i = static_cast<size_t>(y) * GW + static_cast<size_t>(x);
                    const float  a = cov[i];
                    float sd;

                    if (a > 0.004f && a < 0.996f)
                    {
                        // The edge crosses this texel: its area coverage
                        // localises the edge far better than any distance
                        // measured between texel centres. This is what kills
                        // the stair-stepping the old threshold-only field
                        // produced on diagonals and curves.
                        sd = a - 0.5f;
                    }
                    else
                    {
                        // Distances run centre-to-centre, and the edge sits
                        // about half a texel inside the nearer of the two, so
                        // subtract the half texel to land on the real outline.
                        const bool  inside = a > 0.5f;
                        const float d = std::sqrt(inside ? dIn[i] : dOut[i]);
                        sd = (inside ? 1.0f : -1.0f) * (d - 0.5f);
                    }

                    const float n = 0.5f + sd / (2.0f * static_cast<float>(kSpread));
                    pixels_[static_cast<size_t>(penY + y) * kAtlasW
                          + static_cast<size_t>(penX + x)] =
                        static_cast<uint8_t>(jlimit(0.0f, 1.0f, n) * 255.0f);
                }
            }

            g.u0 = static_cast<float>(penX)      / kAtlasW;
            g.v0 = static_cast<float>(penY)      / kAtlasH;
            g.u1 = static_cast<float>(penX + gw) / kAtlasW;
            g.v1 = static_cast<float>(penY + gh) / kAtlasH;
            g.w  = static_cast<float>(gw);
            g.h  = static_cast<float>(gh);
            g.bx = pb.getX() - static_cast<float>(pad);
            g.by = pb.getY() - static_cast<float>(pad);   // relative to baseline

            penX += gw + 1;
            return true;
        };

        for (int c = kFirstChar; c <= kLastChar && !overflow; ++c)
            if (!bakeOne(static_cast<juce_wchar>(c), glyphs_[static_cast<size_t>(c - kFirstChar)]))
                overflow = true;

        for (int i = 0; i < kNumExtra && !overflow; ++i)
            if (!bakeOne(static_cast<juce_wchar>(kExtraChars[i]), extras_[static_cast<size_t>(i)]))
                overflow = true;

        // Space advance, and the cap/x heights the layout code centres on.
        {
            GlyphArrangement sp;
            sp.addLineOfText(font, " ", 0.0f, 0.0f);
            if (sp.getNumGlyphs() >= 1)
                spaceAdvance_ = sp.getGlyph(0).getRight() - sp.getGlyph(0).getLeft();

            auto outlineHeight = [&](juce_wchar cp, float fallback)
            {
                GlyphArrangement ga;
                ga.addLineOfText(font, String::charToString(cp), 0.0f, 0.0f);
                Path p;
                ga.createPath(p);
                const auto b = p.getBounds();
                return b.isEmpty() ? fallback : b.getHeight();
            };
            capHeight_ = outlineHeight('H', ascent_ * 0.72f);
            xHeight_   = outlineHeight('x', ascent_ * 0.52f);

            maxDigitAdvance_ = 0.0f;
            for (int c = '0'; c <= '9'; ++c)
                if (const auto* g = glyph(static_cast<uint32_t>(c)))
                    maxDigitAdvance_ = std::max(maxDigitAdvance_, g->advance);
            if (!(maxDigitAdvance_ > 0.0f)) maxDigitAdvance_ = kBasePx * 0.55f;
        }

        // Overflow is a real failure, not a degraded render. bakeOne writes
        // the advance before the packing check, so the glyph that overflows
        // keeps a correct advance with a zero-area quad, and every glyph after
        // it stays default-constructed with advance 0 — which stalls the pen
        // in SdfCanvas::text and makes textWidth under-report, silently
        // shifting every centred and right-aligned run. Rendering no text is
        // more honest than rendering truncated text.
        if (overflow) return false;

        // Sample across the packing order rather than trusting one glyph:
        // '0' is baked sixteenth, so a failure after it used to report success
        // with the whole alphabet missing.
        for (const char ch : { '0', '9', 'A', 'Z', 'a', 'z', '%' })
            if (const auto* g = glyph(static_cast<uint32_t>(static_cast<unsigned char>(ch)));
                g == nullptr || !(g->w > 0.0f))
                return false;
        baked_ = true;
        return true;
    }
}

#include "FontAtlasSdf.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace hrvbgui
{
    namespace
    {
        constexpr float kInf = 1.0e20f;

        // Felzenszwalb & Huttenlocher's exact 1-D squared distance transform:
        // the lower envelope of the parabolas (q - v)^2 + f[v]. Linear time,
        // and separable, so running it over columns then rows gives the exact
        // 2-D squared EDT.
        //
        // This replaces a brute-force search over a (2*kSpread+1)^2 window
        // that ran per texel through juce::Image::BitmapData::getPixelColour.
        // At ~100 glyphs that was ~61M iterations of format-switching and
        // juce::Colour construction — a few hundred milliseconds to a full
        // second of message-thread stall the first time any editor opened,
        // which reads to a user as the plugin hanging.
        void edt1d(const float* f, float* d, int n, int* v, float* z)
        {
            int k = 0;
            v[0] = 0;
            z[0] = -kInf;
            z[1] =  kInf;

            for (int q = 1; q < n; ++q)
            {
                float s;
                for (;;)
                {
                    const float vk = static_cast<float>(v[k]);
                    const float fq = static_cast<float>(q);
                    s = ((f[q] + fq * fq) - (f[v[k]] + vk * vk))
                      / (2.0f * fq - 2.0f * vk);
                    if (s > z[k] || k == 0) break;
                    --k;
                }
                ++k;
                v[k]     = q;
                z[k]     = s;
                z[k + 1] = kInf;
            }

            k = 0;
            for (int q = 0; q < n; ++q)
            {
                while (z[k + 1] < static_cast<float>(q)) ++k;
                const float dq = static_cast<float>(q - v[k]);
                d[q] = dq * dq + f[v[k]];
            }
        }

        // Exact squared EDT of the set { grid[i] == 0 }.
        void edt2d(std::vector<float>& grid, int w, int h)
        {
            const size_t W = static_cast<size_t>(w), H = static_cast<size_t>(h);
            const size_t n = std::max(W, H);
            std::vector<float> f(n), d(n), z(n + 1);
            std::vector<int>   v(n);

            for (size_t x = 0; x < W; ++x)
            {
                for (size_t y = 0; y < H; ++y) f[y] = grid[y * W + x];
                edt1d(f.data(), d.data(), h, v.data(), z.data());
                for (size_t y = 0; y < H; ++y) grid[y * W + x] = d[y];
            }
            for (size_t y = 0; y < H; ++y)
            {
                float* row = &grid[y * W];
                for (size_t x = 0; x < W; ++x) f[x] = row[x];
                edt1d(f.data(), d.data(), w, v.data(), z.data());
                for (size_t x = 0; x < W; ++x) row[x] = d[x];
            }
        }
    }

    bool FontAtlasSdf::bake(const void* ttfData, size_t ttfBytes, const char* faceName)
    {
        using namespace juce;

        glyphs_.assign(static_cast<size_t>(kLastChar - kFirstChar + 1), {});
        extras_.assign(static_cast<size_t>(kNumExtra), {});
        pixels_.assign(static_cast<size_t>(kAtlasW) * kAtlasH, 0);

        usedEmbedded_ = false;

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
                dIn [i] = inside ? kInf : 0.0f;   // sites = outside texels
                dOut[i] = inside ? 0.0f : kInf;   // sites = inside texels
            }
            edt2d(dIn,  gw, gh);
            edt2d(dOut, gw, gh);

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
        return true;
    }
}

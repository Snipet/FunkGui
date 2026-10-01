#include "FontAtlasSdf.h"

#include "SdfField.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

// The atlas's JUCE-free parts (v0.12.0): the distance transform the bake runs (the bake itself, which rasterises with
// JUCE, is src/juce/FontAtlasBake.cpp), and the baked atlas as bytes: serialise() and load(), so that a build without
// JUCE (FUNKGUI_WITH_JUCE=OFF: the browser) starts from a bake made where JUCE is.
//
// The blob, little-endian whatever the host, floats as their IEEE-754 bit patterns:
//
//   offset  bytes  field
//        0      8  magic "FGSDFATL"
//        8      4  version (kBlobVersion)
//       12      4  size of the whole blob in bytes
//       16      4  kAtlasW            20  4  kAtlasH
//       24      4  kBasePx (float)    28  4  kSpread
//       32      4  kFirstChar         36  4  kLastChar
//       40      4  kNumExtra          44  4  glyph count (ASCII + extras)
//       48      4  flags: bit 0 = baked from the caller's embedded face (usedEmbeddedFace)
//       52      4  0 (reserved)
//       56      8  FNV-1a 64 of the pixels (what FontService::atlasHash() and FontProbe's font.atlas report)
//       64      8  FNV-1a 64 of the table: every byte from offset 72 up to the pixels
//       72     28  metrics: space advance, ascent, descent, line height, cap height, x height, widest digit advance
//      100  40 * n glyph records in atlas order (ASCII kFirstChar..kLastChar, then kExtraChars): codepoint, then
//                  u0 v0 u1 v1 w h bx by advance
//        .  W * H  the R8 distance field, row-major from the top
//
// load() accepts a blob only when every constant in it is this build's (so a blob baked before a Glyphs.def append, or
// with another atlas size, is refused rather than drawn with shifted UVs), both hashes hold and every float is finite.

namespace funkgui
{
    namespace
    {
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
            z[0] = -detail::kSdfInf;
            z[1] =  detail::kSdfInf;

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
                z[k + 1] = detail::kSdfInf;
            }

            k = 0;
            for (int q = 0; q < n; ++q)
            {
                while (z[k + 1] < static_cast<float>(q)) ++k;
                const float dq = static_cast<float>(q - v[k]);
                d[q] = dq * dq + f[v[k]];
            }
        }

        constexpr char   kMagic[8] = { 'F', 'G', 'S', 'D', 'F', 'A', 'T', 'L' };
        constexpr size_t kHeaderBytes = 72;
        constexpr size_t kMetricCount = 7;
        constexpr size_t kGlyphFloats = 9;
        constexpr size_t kGlyphBytes = 4u * (1u + kGlyphFloats);
        constexpr size_t kAsciiCount = static_cast<size_t>(FontAtlasSdf::kLastChar - FontAtlasSdf::kFirstChar + 1);
        constexpr size_t kGlyphCount = kAsciiCount + static_cast<size_t>(FontAtlasSdf::kNumExtra);
        constexpr size_t kTableBytes = 4u * kMetricCount + kGlyphBytes * kGlyphCount;
        constexpr size_t kPixelBytes = static_cast<size_t>(FontAtlasSdf::kAtlasW) * FontAtlasSdf::kAtlasH;
        constexpr size_t kBlobBytes = kHeaderBytes + kTableBytes + kPixelBytes;
        constexpr uint32_t kFlagEmbeddedFace = 1u;

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

        void put32(uint8_t* at, uint32_t v) noexcept
        {
            for (int i = 0; i < 4; ++i)
                at[i] = static_cast<uint8_t>(v >> (8 * i));
        }

        void put64(uint8_t* at, uint64_t v) noexcept
        {
            for (int i = 0; i < 8; ++i)
                at[i] = static_cast<uint8_t>(v >> (8 * i));
        }

        void putFloat(uint8_t* at, float v) noexcept { put32(at, std::bit_cast<uint32_t>(v)); }

        uint32_t get32(const uint8_t* at) noexcept
        {
            uint32_t v = 0;
            for (int i = 0; i < 4; ++i)
                v |= static_cast<uint32_t>(at[i]) << (8 * i);
            return v;
        }

        uint64_t get64(const uint8_t* at) noexcept
        {
            uint64_t v = 0;
            for (int i = 0; i < 8; ++i)
                v |= static_cast<uint64_t>(at[i]) << (8 * i);
            return v;
        }

        // A finite float, or false: a blob never holds NaN or infinity (a bake never produces one).
        bool getFloat(const uint8_t* at, float& out) noexcept
        {
            out = std::bit_cast<float>(get32(at));
            return std::isfinite(out);
        }

        // The codepoint of the i-th glyph record: the atlas's packing order.
        uint32_t codepointAt(size_t i) noexcept
        {
            return i < kAsciiCount ? static_cast<uint32_t>(FontAtlasSdf::kFirstChar) + static_cast<uint32_t>(i)
                                   : FontAtlasSdf::kExtraChars[i - kAsciiCount];
        }
    }

    void detail::sdfEdt2d(std::vector<float>& grid, int w, int h)
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

    std::vector<uint8_t> FontAtlasSdf::serialise() const
    {
        if (!baked_ || glyphs_.size() != kAsciiCount || extras_.size() != static_cast<size_t>(kNumExtra)
            || pixels_.size() != kPixelBytes)
            return {};

        std::vector<uint8_t> blob(kBlobBytes, 0);
        uint8_t* const b = blob.data();
        std::memcpy(b, kMagic, sizeof kMagic);
        put32(b + 8, kBlobVersion);
        put32(b + 12, static_cast<uint32_t>(kBlobBytes));
        put32(b + 16, static_cast<uint32_t>(kAtlasW));
        put32(b + 20, static_cast<uint32_t>(kAtlasH));
        putFloat(b + 24, kBasePx);
        put32(b + 28, static_cast<uint32_t>(kSpread));
        put32(b + 32, static_cast<uint32_t>(kFirstChar));
        put32(b + 36, static_cast<uint32_t>(kLastChar));
        put32(b + 40, static_cast<uint32_t>(kNumExtra));
        put32(b + 44, static_cast<uint32_t>(kGlyphCount));
        put32(b + 48, usedEmbedded_ ? kFlagEmbeddedFace : 0u);
        put32(b + 52, 0u);

        uint8_t* at = b + kHeaderBytes;
        for (const float m : { spaceAdvance_, ascent_, descent_, lineHeight_, capHeight_, xHeight_, maxDigitAdvance_ })
        {
            putFloat(at, m);
            at += 4;
        }
        for (size_t i = 0; i < kGlyphCount; ++i)
        {
            const Glyph& g = i < kAsciiCount ? glyphs_[i] : extras_[i - kAsciiCount];
            put32(at, codepointAt(i));
            at += 4;
            for (const float f : { g.u0, g.v0, g.u1, g.v1, g.w, g.h, g.bx, g.by, g.advance })
            {
                putFloat(at, f);
                at += 4;
            }
        }
        std::memcpy(at, pixels_.data(), kPixelBytes);

        put64(b + 56, fnv1a(pixels_.data(), kPixelBytes));
        put64(b + 64, fnv1a(b + kHeaderBytes, kTableBytes));
        return blob;
    }

    bool FontAtlasSdf::load(const uint8_t* data, size_t bytes)
    {
        // As bake(): the atlas is unbaked until this load has proved itself, and stays so when it fails.
        glyphs_.assign(kAsciiCount, {});
        extras_.assign(static_cast<size_t>(kNumExtra), {});
        pixels_.assign(kPixelBytes, 0);
        usedEmbedded_ = false;
        baked_ = false;

        if (data == nullptr || bytes != kBlobBytes || std::memcmp(data, kMagic, sizeof kMagic) != 0)
            return false;
        if (get32(data + 8) != kBlobVersion || get32(data + 12) != static_cast<uint32_t>(kBlobBytes)
            || get32(data + 16) != static_cast<uint32_t>(kAtlasW) || get32(data + 20) != static_cast<uint32_t>(kAtlasH)
            || get32(data + 24) != std::bit_cast<uint32_t>(kBasePx)
            || get32(data + 28) != static_cast<uint32_t>(kSpread)
            || get32(data + 32) != static_cast<uint32_t>(kFirstChar)
            || get32(data + 36) != static_cast<uint32_t>(kLastChar)
            || get32(data + 40) != static_cast<uint32_t>(kNumExtra)
            || get32(data + 44) != static_cast<uint32_t>(kGlyphCount)
            || (get32(data + 48) & ~kFlagEmbeddedFace) != 0u || get32(data + 52) != 0u)
            return false;

        const uint8_t* const table = data + kHeaderBytes;
        const uint8_t* const pixels = table + kTableBytes;
        if (get64(data + 56) != fnv1a(pixels, kPixelBytes) || get64(data + 64) != fnv1a(table, kTableBytes))
            return false;

        float metrics[kMetricCount] = {};
        const uint8_t* at = table;
        for (float& m : metrics)
        {
            if (!getFloat(at, m))
                return false;
            at += 4;
        }
        std::vector<Glyph> ascii(kAsciiCount), extras(static_cast<size_t>(kNumExtra));
        for (size_t i = 0; i < kGlyphCount; ++i)
        {
            if (get32(at) != codepointAt(i))
                return false;                            // another glyph set, or another order: every UV would shift
            at += 4;
            Glyph& g = i < kAsciiCount ? ascii[i] : extras[i - kAsciiCount];
            for (float* f : { &g.u0, &g.v0, &g.u1, &g.v1, &g.w, &g.h, &g.bx, &g.by, &g.advance })
            {
                if (!getFloat(at, *f))
                    return false;
                at += 4;
            }
        }

        // bake()'s own last check: a blob of a bake that missed part of the alphabet is not an atlas.
        for (const char ch : { '0', '9', 'A', 'Z', 'a', 'z', '%' })
        {
            const size_t index = static_cast<size_t>(static_cast<unsigned char>(ch)) - static_cast<size_t>(kFirstChar);
            if (!(ascii[index].w > 0.0f))
                return false;
        }

        glyphs_ = std::move(ascii);
        extras_ = std::move(extras);
        std::memcpy(pixels_.data(), pixels, kPixelBytes);
        spaceAdvance_    = metrics[0];
        ascent_          = metrics[1];
        descent_         = metrics[2];
        lineHeight_      = metrics[3];
        capHeight_       = metrics[4];
        xHeight_         = metrics[5];
        maxDigitAdvance_ = metrics[6];
        usedEmbedded_    = (get32(data + 48) & kFlagEmbeddedFace) != 0u;
        baked_ = true;
        return true;
    }
}

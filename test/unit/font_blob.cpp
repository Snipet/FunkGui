// FUNKGUI_TEST name=fg.font.blob timeout=300 gpu=0
//
// fg.font.blob (v0.12.0): the baked atlas as bytes, FontAtlasSdf::serialise() and load(). JUCE-free, so it runs in
// both builds over the process's own atlas (FontService): the live bake with JUCE, the committed bake that FunkGuiFonts
// embeds without it (FUNKGUI_WITH_JUCE=OFF; fg.font.baked holds that file to macOS's live bake).
//
// - The atlas the service hands out is real: baked from the bundled face, its hash the hash of its pixels.
// - serialise() writes the documented layout (src/text/FontAtlasSdf.cpp): the size, the magic, the version, the
//   constants and the two hashes are where the format says.
// - load() of those bytes gives the same atlas: every texel, every glyph record of the baked set bit for bit, the
//   seven metrics, the embedded-face flag; and it serialises to the same bytes again.
// - load() refuses what is not a whole blob of this build, and leaves the atlas unbaked: nothing, a truncated or
//   extended blob, another magic, version, atlas size or glyph set, a set reserved bit, a damaged texel or table byte
//   (the hashes), a table whose hash holds but whose codepoints are another order, a metric that is not finite. A
//   refused atlas loads a good blob afterwards.
// - An unbaked atlas serialises to nothing; without JUCE bake() refuses.
// Spec rows only.

#include <funkgui/core/HasJuce.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/test/Harness.h>
#include <funkgui/text/BundledFont.h>
#include <funkgui/text/FontAtlasSdf.h>
#include <funkgui/text/FontService.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace T = funkgui::test;
using funkgui::FontAtlasSdf;

namespace
{
    // The layout (src/text/FontAtlasSdf.cpp), restated: the test fails if the format moves without its version.
    constexpr size_t kHeader = 72, kMetrics = 7 * 4, kRecord = 4 + 9 * 4;
    constexpr size_t kGlyphs = static_cast<size_t>(FontAtlasSdf::kLastChar - FontAtlasSdf::kFirstChar + 1)
                             + static_cast<size_t>(FontAtlasSdf::kNumExtra);
    constexpr size_t kTable = kMetrics + kRecord * kGlyphs;
    constexpr size_t kPixels = static_cast<size_t>(FontAtlasSdf::kAtlasW) * FontAtlasSdf::kAtlasH;
    constexpr size_t kBlob = kHeader + kTable + kPixels;

    bool sameBits(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }

    uint32_t get32(const std::vector<uint8_t>& b, size_t at)
    {
        return static_cast<uint32_t>(b[at]) | (static_cast<uint32_t>(b[at + 1]) << 8)
             | (static_cast<uint32_t>(b[at + 2]) << 16) | (static_cast<uint32_t>(b[at + 3]) << 24);
    }

    uint64_t get64(const std::vector<uint8_t>& b, size_t at)
    {
        return static_cast<uint64_t>(get32(b, at)) | (static_cast<uint64_t>(get32(b, at + 4)) << 32);
    }

    void put32(std::vector<uint8_t>& b, size_t at, uint32_t v)
    {
        for (size_t i = 0; i < 4; ++i)
            b[at + i] = static_cast<uint8_t>(v >> (8 * i));
    }

    void put64(std::vector<uint8_t>& b, size_t at, uint64_t v)
    {
        put32(b, at, static_cast<uint32_t>(v));
        put32(b, at + 4, static_cast<uint32_t>(v >> 32));
    }

    // After an edit inside the table: make its hash hold again, so load() has to find the edit itself.
    void rehashTable(std::vector<uint8_t>& b) { put64(b, 64, T::fnv1a(b.data() + kHeader, kTable)); }

    std::vector<uint32_t> bakedSet()
    {
        std::vector<uint32_t> cps;
        for (uint32_t c = FontAtlasSdf::kFirstChar; c <= static_cast<uint32_t>(FontAtlasSdf::kLastChar); ++c)
            cps.push_back(c);
        cps.insert(cps.end(), std::begin(FontAtlasSdf::kExtraChars), std::end(FontAtlasSdf::kExtraChars));
        return cps;
    }

    int glyphsDiffering(const FontAtlasSdf& a, const FontAtlasSdf& b)
    {
        int n = 0;
        for (const uint32_t cp : bakedSet())
        {
            const auto* x = a.glyph(cp);
            const auto* y = b.glyph(cp);
            const bool same = x != nullptr && y != nullptr && sameBits(x->u0, y->u0) && sameBits(x->v0, y->v0)
                           && sameBits(x->u1, y->u1) && sameBits(x->v1, y->v1) && sameBits(x->w, y->w)
                           && sameBits(x->h, y->h) && sameBits(x->bx, y->bx) && sameBits(x->by, y->by)
                           && sameBits(x->advance, y->advance);
            n += same ? 0 : 1;
        }
        return n;
    }

    int metricsDiffering(const FontAtlasSdf& a, const FontAtlasSdf& b)
    {
        return (sameBits(a.spaceAdvance(), b.spaceAdvance()) ? 0 : 1) + (sameBits(a.ascent(), b.ascent()) ? 0 : 1)
             + (sameBits(a.descent(), b.descent()) ? 0 : 1) + (sameBits(a.lineHeight(), b.lineHeight()) ? 0 : 1)
             + (sameBits(a.capHeight(), b.capHeight()) ? 0 : 1) + (sameBits(a.xHeight(), b.xHeight()) ? 0 : 1)
             + (sameBits(a.maxDigitAdvance(), b.maxDigitAdvance()) ? 0 : 1);
    }
}

int main(int argc, char** argv)
{
    const funkgui::HeadlessGuiScope gui;                 // JUCE's GUI side, when there is JUCE: the atlas bakes there
    T::Probe P("fg.font.blob", "", argc, argv);

    // ---- the process's atlas ----------------------------------------------------------------------------------------
    auto& fonts = funkgui::FontService::get();
    const FontAtlasSdf& atlas = fonts.atlas();
    P.eq("service.ok", fonts.ok(), 1);
    P.eq("service.baked", atlas.baked(), 1);
    P.eq("service.embedded_face", atlas.usedEmbeddedFace(), 1);
    P.eq("service.texels", static_cast<int64_t>(atlas.pixels().size()), static_cast<int64_t>(kPixels));
    const uint64_t pixelHash = T::fnv1a(atlas.pixels().data(), atlas.pixels().size());
    P.eq("service.hash_is_the_pixels", fonts.atlasHash() == pixelHash && pixelHash != 0, 1);
    P.note("atlas", "\"" + T::detail::fmtHash(pixelHash) + "\"");
    P.note("juce", FUNKGUI_HAS_JUCE ? "true" : "false");
    if (!atlas.baked())
    {
        P.harnessError("the process's atlas is not baked");
        return P.finish();
    }

    // ---- serialise(): the layout ------------------------------------------------------------------------------------
    const std::vector<uint8_t> blob = atlas.serialise();
    P.eq("blob.bytes", static_cast<int64_t>(blob.size()), static_cast<int64_t>(kBlob));
    if (blob.size() != kBlob)
        return P.finish();
    P.eq("blob.magic", std::memcmp(blob.data(), "FGSDFATL", 8) == 0, 1);
    P.eq("blob.version", get32(blob, 8), FontAtlasSdf::kBlobVersion);
    P.eq("blob.version_is_1", FontAtlasSdf::kBlobVersion, 1);
    P.eq("blob.size_field", get32(blob, 12), static_cast<int64_t>(kBlob));
    P.eq("blob.atlas_size", get32(blob, 16) == static_cast<uint32_t>(FontAtlasSdf::kAtlasW)
                                && get32(blob, 20) == static_cast<uint32_t>(FontAtlasSdf::kAtlasH), 1);
    P.eq("blob.base_px", get32(blob, 24) == std::bit_cast<uint32_t>(FontAtlasSdf::kBasePx), 1);
    P.eq("blob.spread", get32(blob, 28), FontAtlasSdf::kSpread);
    P.eq("blob.glyph_set", get32(blob, 32) == static_cast<uint32_t>(FontAtlasSdf::kFirstChar)
                               && get32(blob, 36) == static_cast<uint32_t>(FontAtlasSdf::kLastChar)
                               && get32(blob, 40) == static_cast<uint32_t>(FontAtlasSdf::kNumExtra)
                               && get32(blob, 44) == static_cast<uint32_t>(kGlyphs), 1);
    P.eq("blob.flags_embedded_face", get32(blob, 48), 1);
    P.eq("blob.reserved_zero", get32(blob, 52), 0);
    P.eq("blob.pixel_hash", get64(blob, 56) == pixelHash, 1);
    P.eq("blob.table_hash", get64(blob, 64) == T::fnv1a(blob.data() + kHeader, kTable), 1);
    P.eq("blob.first_record_is_first_char", get32(blob, kHeader + kMetrics), FontAtlasSdf::kFirstChar);
    P.eq("blob.last_record_is_last_extra", get32(blob, kHeader + kMetrics + kRecord * (kGlyphs - 1)),
         FontAtlasSdf::kExtraChars[FontAtlasSdf::kNumExtra - 1]);
    P.eq("blob.pixels_at_the_end", std::memcmp(blob.data() + kHeader + kTable, atlas.pixels().data(), kPixels) == 0, 1);
    P.eq("blob.deterministic", atlas.serialise() == blob, 1);

    // ---- load(): the same atlas -------------------------------------------------------------------------------------
    FontAtlasSdf back;
    P.eq("unbaked.serialises_to_nothing", back.serialise().empty(), 1);
    P.eq("load.ok", back.load(blob.data(), blob.size()), 1);
    P.eq("load.baked", back.baked(), 1);
    P.eq("load.embedded_face", back.usedEmbeddedFace(), 1);
    P.eq("load.texels_equal", back.pixels() == atlas.pixels(), 1);
    P.eq("load.glyphs_differing", glyphsDiffering(back, atlas), 0);
    P.eq("load.metrics_differing", metricsDiffering(back, atlas), 0);
    P.eq("load.outside_the_set_is_null", back.glyph(0x4E00) == nullptr && back.glyph(' ') == nullptr, 1);
    P.eq("load.reserialises_equal", back.serialise() == blob, 1);
    {
        std::vector<uint8_t> plain = blob;                   // the flag is carried, not assumed
        put32(plain, 48, 0);
        FontAtlasSdf a;
        P.eq("load.flag_carried", a.load(plain.data(), plain.size()) && a.baked() && !a.usedEmbeddedFace(), 1);
    }

    // ---- load(): refusals -------------------------------------------------------------------------------------------
    const auto refused = [&](const char* key, const std::vector<uint8_t>& bytes, size_t size)
    {
        FontAtlasSdf a;
        const bool was = a.load(blob.data(), blob.size());                  // a good atlas first: a refusal unbakes it
        const bool ok = a.load(bytes.empty() ? nullptr : bytes.data(), size);
        P.eq(key, was && !ok && !a.baked() && !a.usedEmbeddedFace() && a.serialise().empty(), 1);
    };
    const auto edited = [&](size_t at, uint8_t xorWith, bool rehash)
    {
        std::vector<uint8_t> b = blob;
        b[at] = static_cast<uint8_t>(b[at] ^ xorWith);
        if (rehash)
            rehashTable(b);
        return b;
    };
    refused("refuse.null", {}, 0);
    refused("refuse.null_with_size", {}, kBlob);
    refused("refuse.truncated", blob, kBlob - 1);
    refused("refuse.header_only", blob, kHeader);
    {
        std::vector<uint8_t> longer = blob;
        longer.push_back(0);
        refused("refuse.extended", longer, longer.size());
    }
    refused("refuse.magic", edited(0, 0x20, false), kBlob);
    refused("refuse.version", edited(8, 0x03, false), kBlob);
    refused("refuse.size_field", edited(12, 0x01, false), kBlob);
    refused("refuse.atlas_width", edited(17, 0x04, false), kBlob);
    refused("refuse.base_px", edited(27, 0x01, false), kBlob);
    refused("refuse.spread", edited(28, 0x01, false), kBlob);
    refused("refuse.extra_count", edited(40, 0x01, false), kBlob);
    refused("refuse.glyph_count", edited(44, 0x01, false), kBlob);
    refused("refuse.reserved_flag_bit", edited(48, 0x02, false), kBlob);
    refused("refuse.reserved_word", edited(52, 0x01, false), kBlob);
    refused("refuse.damaged_texel", edited(kHeader + kTable + kPixels / 2, 0x01, false), kBlob);
    refused("refuse.damaged_last_texel", edited(kBlob - 1, 0x80, false), kBlob);
    refused("refuse.damaged_table", edited(kHeader + kMetrics + 5, 0x01, false), kBlob);
    refused("refuse.pixel_hash_field", edited(56, 0x01, false), kBlob);
    refused("refuse.table_hash_field", edited(64, 0x01, false), kBlob);
    {
        // Another glyph order whose table hash holds: the first two records' codepoints swapped.
        std::vector<uint8_t> b = blob;
        const size_t r0 = kHeader + kMetrics, r1 = r0 + kRecord;
        const uint32_t c0 = get32(b, r0), c1 = get32(b, r1);
        put32(b, r0, c1);
        put32(b, r1, c0);
        rehashTable(b);
        refused("refuse.glyph_order", b, kBlob);
    }
    {
        std::vector<uint8_t> b = blob;                       // an extra codepoint that is not this build's
        put32(b, kHeader + kMetrics + kRecord * (kGlyphs - 1), 0x2603);
        rehashTable(b);
        refused("refuse.other_extra_glyph", b, kBlob);
    }
    {
        std::vector<uint8_t> b = blob;                       // the ascent, not a number
        put32(b, kHeader + 4, std::bit_cast<uint32_t>(std::numeric_limits<float>::quiet_NaN()));
        rehashTable(b);
        refused("refuse.metric_nan", b, kBlob);
        b = blob;                                            // a glyph's advance, infinite
        put32(b, kHeader + kMetrics + 4 + 8 * 4, std::bit_cast<uint32_t>(std::numeric_limits<float>::infinity()));
        rehashTable(b);
        refused("refuse.glyph_infinite", b, kBlob);
    }
    {
        // A blob of a bake that missed a letter (a zero-width 'A') is not an atlas, whatever its hashes say.
        std::vector<uint8_t> b = blob;
        const size_t recA = kHeader + kMetrics + kRecord * static_cast<size_t>('A' - FontAtlasSdf::kFirstChar);
        put32(b, recA + 4 + 4 * 4, std::bit_cast<uint32_t>(0.0f));
        rehashTable(b);
        refused("refuse.missing_letter", b, kBlob);
    }
    {
        FontAtlasSdf a;
        const bool bad = a.load(blob.data(), kBlob - 1);
        P.eq("refuse.then_loads_again", !bad && a.load(blob.data(), blob.size()) && a.serialise() == blob, 1);
    }

#if !FUNKGUI_HAS_JUCE
    // ---- without JUCE there is no rasteriser: bake() says so, and the atlas above came from the embedded blob ------
    {
        FontAtlasSdf a;
        const bool was = a.load(blob.data(), blob.size());
        P.eq("nojuce.bake_refuses", was && !a.bake(funkgui::BundledFont::data(), funkgui::BundledFont::size())
                                        && !a.baked() && !a.bake(), 1);
    }
#endif
    return P.finish();
}

// AtlasBlob: the committed bake of the font atlas, fonts/FunkGuiAtlas-macos.bin (v0.12.0), written and held to account.
//
// A build without JUCE (FUNKGUI_WITH_JUCE=OFF: the browser, the native `nojuce` preset) cannot rasterise glyphs, so its
// FontService adopts this file, embedded by FunkGuiFonts, through FontAtlasSdf::load(). The file is the bake macOS
// makes at run time, serialised (FontAtlasSdf::serialise): a JUCE-free build then draws with exactly the glyph
// metrics, UVs and texels a macOS plug-in draws with, and its frames match the macOS goldens.
//
//   FunkGuiAtlasBlob write <out.bin>
//       Bakes the bundled face as the editor does and writes the blob (beside the destination, then renamed onto
//       it). Run it on macOS, into fonts/FunkGuiAtlas-macos.bin, whenever golden impact is `atlas`: a Glyphs.def
//       append, a new subset, a changed baker. Linux bakes other pixels (JUCE's software rasteriser): its blob is not
//       the committed one.
//
//   FunkGuiAtlasBlob <probe> <blob.bin> --golden-root <dir> --arch arm64|x86_64 [--bless-to <dir>] [--results <dir>]
//       The test fg.font.baked (macOS only: the bake is the platform's): the committed blob must be the live bake.
//       Spec rows only: the file reads and loads; no texel differs; no glyph record differs in any of its nine
//       numbers; the seven metrics are bit-equal; the embedded-face flag agrees; and the file is byte for byte what
//       serialise() writes today, from the live bake and again from the loaded atlas. A stale blob fails here, on the
//       machine that can regenerate it, instead of drawing shifted text in a browser.
//
// A JUCE tool (the live bake is JUCE's): it does not exist in a FUNKGUI_WITH_JUCE=OFF build.

#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/test/Harness.h>
#include <funkgui/text/BundledFont.h>
#include <funkgui/text/FontAtlasSdf.h>

#include <juce_core/juce_core.h>

#include <bit>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <string>
#include <vector>

namespace T = funkgui::test;
using funkgui::FontAtlasSdf;

namespace
{
    juce::File absolute(const std::string& path)
    {
        return juce::File::getCurrentWorkingDirectory().getChildFile(juce::String::fromUTF8(path.c_str()));
    }

    bool sameBits(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }

    // Every codepoint the atlas bakes, in atlas order: printable ASCII, then FontAtlasSdf::kExtraChars.
    std::vector<uint32_t> bakedSet()
    {
        std::vector<uint32_t> cps;
        for (uint32_t c = FontAtlasSdf::kFirstChar; c <= static_cast<uint32_t>(FontAtlasSdf::kLastChar); ++c)
            cps.push_back(c);
        cps.insert(cps.end(), std::begin(FontAtlasSdf::kExtraChars), std::end(FontAtlasSdf::kExtraChars));
        return cps;
    }

    bool sameGlyph(const FontAtlasSdf::Glyph* a, const FontAtlasSdf::Glyph* b)
    {
        if (a == nullptr || b == nullptr)
            return a == b;
        return sameBits(a->u0, b->u0) && sameBits(a->v0, b->v0) && sameBits(a->u1, b->u1) && sameBits(a->v1, b->v1)
            && sameBits(a->w, b->w) && sameBits(a->h, b->h) && sameBits(a->bx, b->bx) && sameBits(a->by, b->by)
            && sameBits(a->advance, b->advance);
    }

    int metricsDiffering(const FontAtlasSdf& a, const FontAtlasSdf& b)
    {
        int n = 0;
        n += sameBits(a.spaceAdvance(), b.spaceAdvance()) ? 0 : 1;
        n += sameBits(a.ascent(), b.ascent()) ? 0 : 1;
        n += sameBits(a.descent(), b.descent()) ? 0 : 1;
        n += sameBits(a.lineHeight(), b.lineHeight()) ? 0 : 1;
        n += sameBits(a.capHeight(), b.capHeight()) ? 0 : 1;
        n += sameBits(a.xHeight(), b.xHeight()) ? 0 : 1;
        n += sameBits(a.maxDigitAdvance(), b.maxDigitAdvance()) ? 0 : 1;
        return n;
    }

    uint64_t pixelHash(const FontAtlasSdf& a)
    {
        const auto& px = a.pixels();
        return T::fnv1a(px.data(), px.size() * sizeof(px[0]));
    }

    int writeMode(const std::string& out)
    {
        FontAtlasSdf atlas;
        if (!atlas.bake(funkgui::BundledFont::data(), funkgui::BundledFont::size()) || !atlas.usedEmbeddedFace())
        {
            std::fprintf(stderr, "FunkGuiAtlasBlob: the bundled face did not bake\n");
            return 1;
        }
        const std::vector<uint8_t> blob = atlas.serialise();
        FontAtlasSdf back;
        if (blob.empty() || !back.load(blob.data(), blob.size()) || back.serialise() != blob)
        {
            std::fprintf(stderr, "FunkGuiAtlasBlob: the bake does not survive serialise() and load()\n");
            return 1;
        }
        const juce::File dest = absolute(out);
        if (!dest.replaceWithData(blob.data(), blob.size()))         // a temporary beside it, then moved onto it
        {
            std::fprintf(stderr, "FunkGuiAtlasBlob: cannot write %s\n", dest.getFullPathName().toRawUTF8());
            return 1;
        }
        std::printf("wrote %s: %zu bytes, blob version %u, font.atlas %016llx\n", dest.getFullPathName().toRawUTF8(),
                    blob.size(), static_cast<unsigned>(FontAtlasSdf::kBlobVersion),
                    static_cast<unsigned long long>(pixelHash(atlas)));
        return 0;
    }
}

int main(int argc, char** argv)
{
    const funkgui::HeadlessGuiScope gui;                 // the bake goes through JUCE's font stack
    const std::vector<std::string> args = T::positionals(argc, argv);
    if (args.size() == 2 && args[0] == "write")
        return writeMode(args[1]);
    if (args.size() != 2)
    {
        std::fprintf(stderr, "usage: %s write <out.bin>\n       %s <probe> <blob.bin> --golden-root <dir> "
                             "--arch arm64|x86_64 [--bless-to <dir>] [--results <dir>]\n",
                     argc > 0 ? argv[0] : "FunkGuiAtlasBlob", argc > 0 ? argv[0] : "FunkGuiAtlasBlob");
        return 4;
    }
    T::Probe P(args[0], "", argc, argv);

    // The live bake: what a plug-in on this machine draws with.
    FontAtlasSdf live;
    const bool baked = live.bake(funkgui::BundledFont::data(), funkgui::BundledFont::size());
    P.eq("live.bakes", baked, 1);
    P.eq("live.embedded_face", live.usedEmbeddedFace(), 1);

    // The committed blob.
    const juce::File file = absolute(args[1]);
    juce::MemoryBlock bytes;
    const bool read = file.existsAsFile() && file.loadFileAsData(bytes);
    if (!P.eq("blob.read", read, 1))
        std::printf("cannot read %s\n", file.getFullPathName().toRawUTF8());
    if (!read || !baked)
        return P.finish();
    const auto* data = static_cast<const uint8_t*>(bytes.getData());
    FontAtlasSdf blob;
    const bool loaded = blob.load(data, bytes.getSize());
    P.eq("blob.loads", loaded, 1);
    P.eq("blob.baked", blob.baked(), 1);
    P.eq("blob.embedded_face", blob.usedEmbeddedFace(), 1);

    // Pixels, every glyph record, the metrics.
    const auto& pl = live.pixels();
    const auto& pb = blob.pixels();
    size_t texels = 0;
    for (size_t i = 0; i < pl.size() && i < pb.size(); ++i)
        texels += pl[i] != pb[i] ? 1u : 0u;
    int glyphs = 0;
    for (const uint32_t cp : bakedSet())
        if (!sameGlyph(live.glyph(cp), blob.glyph(cp)) && ++glyphs <= 8)
            std::printf("glyph U+%04X differs%s\n", static_cast<unsigned>(cp), glyphs == 8 ? " (and maybe more)" : "");
    std::printf("%-22s font.atlas %016llx   cap %.7f  x %.7f  ascent %.7f  digit %.7f\n", "live bake",
                static_cast<unsigned long long>(pixelHash(live)), static_cast<double>(live.capHeight()),
                static_cast<double>(live.xHeight()), static_cast<double>(live.ascent()),
                static_cast<double>(live.maxDigitAdvance()));
    std::printf("%-22s font.atlas %016llx   cap %.7f  x %.7f  ascent %.7f  digit %.7f   (%zu bytes)\n",
                file.getFileName().toRawUTF8(), static_cast<unsigned long long>(pixelHash(blob)),
                static_cast<double>(blob.capHeight()), static_cast<double>(blob.xHeight()),
                static_cast<double>(blob.ascent()), static_cast<double>(blob.maxDigitAdvance()), bytes.getSize());
    P.eq("blob.texel_count", static_cast<int64_t>(pb.size()), static_cast<int64_t>(pl.size()));
    P.eq("blob.texels_differing", static_cast<int64_t>(texels), 0);
    P.eq("blob.atlas_hash_is_live", pixelHash(blob) == pixelHash(live), 1);
    P.eq("blob.glyph_count", static_cast<int64_t>(bakedSet().size()),
         static_cast<int64_t>(FontAtlasSdf::kLastChar - FontAtlasSdf::kFirstChar + 1 + FontAtlasSdf::kNumExtra));
    P.eq("blob.glyphs_differing", glyphs, 0);
    P.eq("blob.metrics_differing", metricsDiffering(live, blob), 0);

    // And the bytes themselves: the file is what serialise() writes today, so a format change regenerates it too.
    const std::vector<uint8_t> fresh = live.serialise();
    const std::vector<uint8_t> again = blob.serialise();
    const std::vector<uint8_t> held(data, data + bytes.getSize());
    P.eq("blob.bytes", static_cast<int64_t>(held.size()), static_cast<int64_t>(fresh.size()));
    P.eq("blob.is_the_live_bake_serialised", !fresh.empty() && fresh == held, 1);
    P.eq("blob.reserialises_to_itself", !again.empty() && again == held, 1);
    if (fresh != held)
        std::printf("the committed blob is stale: on macOS, run `FunkGuiAtlasBlob write %s`\n",
                    file.getFullPathName().toRawUTF8());
    return P.finish();
}

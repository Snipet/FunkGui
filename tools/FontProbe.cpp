// FontProbe: the font pipeline's gate, and the proof that a candidate font file is interchangeable with the bundled
// one: same metrics, same coverage, and a bit-identical distance field. If the atlas pixels match, every glyph a panel
// draws is byte-for-byte the same and no layout can have shifted.
//
// The bundled face is baked and its atlas fingerprinted along with the metrics every layout constant was measured
// against (golden rows: a changed font file, subset, baker or rasteriser moves the hash; a changed cap height moves the
// type scale). That no glyph of the baked set is missing is a spec row, and so is every candidate's equivalence.
//
//   FunkGuiFontProbe <probe> [candidate.ttf ...] --golden-root <dir> --arch arm64|x86_64 [--bless-to <dir>]
//                    [--results <dir>] [--only <glob>] [--verbose]
//
// fg.font.probe runs `FunkGuiFontProbe fg.font.probe <FunkGui>/fonts/upstream/JetBrainsMono-Regular.ttf …`: the
// committed subset must stay interchangeable with the upstream face it was cut from (02 §3.11).
// (Seeded from HardwareReverb Tools/FontProbe.cpp; ported to Harness v2.)

#include <funkgui/test/Harness.h>
#include <funkgui/text/BundledFont.h>
#include <funkgui/text/FontAtlasSdf.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace T = funkgui::test;
using funkgui::FontAtlasSdf;

namespace
{
    // Every codepoint the atlas bakes: printable ASCII plus FontAtlasSdf::kExtraChars.
    std::vector<uint32_t> bakedSet()
    {
        std::vector<uint32_t> cps;
        for (uint32_t c = FontAtlasSdf::kFirstChar; c <= static_cast<uint32_t>(FontAtlasSdf::kLastChar); ++c)
            cps.push_back(c);
        cps.insert(cps.end(), std::begin(FontAtlasSdf::kExtraChars), std::end(FontAtlasSdf::kExtraChars));
        return cps;
    }

    int missingGlyphs(const FontAtlasSdf& a)
    {
        int missing = 0;
        for (const uint32_t cp : bakedSet())
        {
            const auto* g = a.glyph(cp);
            if (g == nullptr || g->w <= 0.0f)
                ++missing;
        }
        return missing;
    }

    uint64_t atlasHash(const FontAtlasSdf& a)
    {
        const auto& px = a.pixels();
        return T::fnv1a(px.data(), px.size() * sizeof(px[0]));
    }

    void describe(const char* what, const FontAtlasSdf& a)
    {
        std::printf("%-30s asc %7.3f desc %7.3f cap %7.3f x %7.3f space %7.3f digit %7.3f  missing %d\n", what,
                    static_cast<double>(a.ascent()), static_cast<double>(a.descent()),
                    static_cast<double>(a.capHeight()), static_cast<double>(a.xHeight()),
                    static_cast<double>(a.spaceAdvance()), static_cast<double>(a.maxDigitAdvance()),
                    missingGlyphs(a));
    }
}

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    const std::vector<std::string> args = T::positionals(argc, argv);
    if (args.empty())
    {
        std::fprintf(stderr, "usage: %s <probe> [candidate.ttf ...] --golden-root <dir> --arch arm64|x86_64 "
                             "[--bless-to <dir>] [--results <dir>]\n", argc > 0 ? argv[0] : "FunkGuiFontProbe");
        return 4;
    }
    T::Probe P(args[0], "", argc, argv);

    FontAtlasSdf ref;
    const bool baked = ref.bake(funkgui::BundledFont::data(), funkgui::BundledFont::size());
    P.eq("font.bundled.bakes", baked, 1);
    P.eq("font.bundled.embedded_face", ref.usedEmbeddedFace(), 1);
    if (!baked)
        return P.finish();
    describe("bundled (reference)", ref);

    // The reference, as numbers a golden can hold.
    P.hash("font.atlas", atlasHash(ref));
    P.num("font.cap", ref.capHeight(), T::Tol::abs(1.0e-3));
    P.num("font.x", ref.xHeight(), T::Tol::abs(1.0e-3));
    P.num("font.ascent", ref.ascent(), T::Tol::abs(1.0e-3));
    P.num("font.digit", ref.maxDigitAdvance(), T::Tol::abs(1.0e-3));
    P.eq("font.missing", missingGlyphs(ref), 0);

    for (std::size_t i = 1; i < args.size(); ++i)
    {
        const std::string key = "font.candidate." + std::to_string(i);
        const juce::File f = juce::File::getCurrentWorkingDirectory().getChildFile(juce::String(args[i]));
        juce::MemoryBlock mb;
        const bool read = f.existsAsFile() && f.loadFileAsData(mb);
        if (!P.eq(key + ".read", read, 1))
        {
            std::printf("cannot read %s\n", args[i].c_str());
            continue;
        }
        FontAtlasSdf cand;
        if (!P.eq(key + ".bakes", cand.bake(mb.getData(), mb.getSize()), 1))
            continue;
        describe(f.getFileName().toRawUTF8(), cand);

        // Per-glyph advances over the whole baked set, then the field itself.
        double worstAdvance = 0.0;
        for (const uint32_t cp : bakedSet())
        {
            const auto* a = ref.glyph(cp);
            const auto* b = cand.glyph(cp);
            if (a != nullptr && b != nullptr)
                worstAdvance = std::max(worstAdvance, std::fabs(static_cast<double>(a->advance - b->advance)));
        }
        const auto& pa = ref.pixels();
        const auto& pb = cand.pixels();
        std::size_t differing = 0;
        for (std::size_t k = 0; k < pa.size() && k < pb.size(); ++k)
            differing += pa[k] != pb[k] ? 1 : 0;
        std::printf("%-30s worst advance delta %.6f   atlas texels differing: %zu / %zu\n", "  vs reference:",
                    worstAdvance, differing, pa.size());

        P.eq(key + ".missing", missingGlyphs(cand), 0);
        P.eq(key + ".texel_count", static_cast<int64_t>(pb.size()), static_cast<int64_t>(pa.size()));
        P.eq(key + ".texels_differing", static_cast<int64_t>(differing), 0);
        P.near(key + ".worst_advance_delta", worstAdvance, 0.0, 0.0);
    }
    return P.finish();
}

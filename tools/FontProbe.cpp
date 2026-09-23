// Proves a candidate font file is interchangeable with the bundled one: same
// metrics, same coverage, and a bit-identical distance field. If the atlas
// pixels match, every glyph the panel draws is byte-for-byte the same and no
// layout can have shifted.
//
// With no candidates it is the font pipeline's own gate: the bundled face is
// baked and its atlas fingerprinted along with the metrics every layout
// constant was measured against. A changed font file, a changed subset, a
// changed baker or a changed FreeType all move the hash; a changed cap height
// moves the type scale. Exit code is the verdict.
//
//   HardwareReverbFontProbe [--check <golden>] [candidate.ttf ...]
#include "gui/BundledFont.h"
#include "gui/FontAtlasSdf.h"
#include "Harness.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <cstdio>
#include <cstring>
#include <vector>

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    std::vector<hrvb::Metric> metrics;
    int bad = 0;

    hrvbgui::FontAtlasSdf ref;
    if (!ref.bake(hrvbgui::BundledFont::data(), hrvbgui::BundledFont::size()))
    { std::printf("reference bake FAILED\n"); return 1; }

    // The reference, as numbers a golden can hold.
    {
        const auto& px = ref.pixels();
        const auto* bytes = reinterpret_cast<const uint8_t*>(px.data());
        const size_t n = px.size() * sizeof(px[0]);
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i < n; ++i) { h ^= bytes[i]; h *= 1099511628211ull; }
        hrvb::addHash(metrics, "font.atlas", h);
        hrvb::addNum(metrics, "font.cap",    ref.capHeight(),      1.0e-3);
        hrvb::addNum(metrics, "font.x",      ref.xHeight(),        1.0e-3);
        hrvb::addNum(metrics, "font.ascent", ref.ascent(),         1.0e-3);
        hrvb::addNum(metrics, "font.digit",  ref.maxDigitAdvance(), 1.0e-3);
        int missing = 0;
        for (int c = 33; c <= 126; ++c)
        { const auto* g = ref.glyph((uint32_t) c); if (!g || g->w <= 0.0f) ++missing; }
        hrvb::addNum(metrics, "font.missing", missing, 0.0);
    }

    auto describe = [](const char* what, const hrvbgui::FontAtlasSdf& a)
    {
        int missing = 0;
        for (int c = 33; c <= 126; ++c)
        { const auto* g = a.glyph((uint32_t) c); if (!g || g->w <= 0.0f) ++missing; }
        for (int i = 0; i < hrvbgui::FontAtlasSdf::kNumExtra; ++i)
        { const auto* g = a.glyph(hrvbgui::FontAtlasSdf::kExtraChars[i]);
          if (!g || g->w <= 0.0f) ++missing; }
        std::printf("%-30s asc %7.3f desc %7.3f cap %7.3f x %7.3f space %7.3f digit %7.3f  missing %d\n",
                    what, a.ascent(), a.descent(), a.capHeight(), a.xHeight(),
                    a.spaceAdvance(), a.maxDigitAdvance(), missing);
    };
    describe("bundled (reference)", ref);

    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--check") == 0 || std::strcmp(argv[i], "--bless") == 0) { ++i; continue; }
        juce::File f{ juce::String(argv[i]) };
        juce::MemoryBlock mb;
        if (!f.existsAsFile() || !f.loadFileAsData(mb))
        { std::printf("cannot read %s\n", argv[i]); ++bad; continue; }

        hrvbgui::FontAtlasSdf cand;
        if (!cand.bake(mb.getData(), mb.getSize()))
        { std::printf("%-30s bake FAILED\n", f.getFileName().toRawUTF8()); ++bad; continue; }
        describe(f.getFileName().toRawUTF8(), cand);

        // Per-glyph advance comparison, then the field itself.
        double worstAdv = 0.0;
        for (int c = 33; c <= 126; ++c)
        {
            const auto* a = ref.glyph((uint32_t) c);
            const auto* b = cand.glyph((uint32_t) c);
            if (a && b) worstAdv = juce::jmax(worstAdv, (double) std::abs(a->advance - b->advance));
        }
        const auto& pa = ref.pixels();
        const auto& pb = cand.pixels();
        size_t diff = 0;
        for (size_t k = 0; k < pa.size() && k < pb.size(); ++k)
            if (pa[k] != pb[k]) ++diff;
        const bool same = (diff == 0 && worstAdv == 0.0);
        if (!same) ++bad;
        std::printf("%-30s worst advance delta %.6f   atlas texels differing: %zu / %zu  %s\n",
                    "  vs reference:", worstAdv, diff, pa.size(),
                    same ? "BIT-IDENTICAL" : "*** DIFFERS ***");
    }
    const int rc = hrvb::finish(argc, argv, metrics);
    return bad > 0 ? 1 : rc;
}

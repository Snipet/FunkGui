// FUNKGUI_TEST name=fg.textfit timeout=600 gpu=0
//
// fg.textfit: text::decodeUtf8 / width / fits / fitEllipsis (text/TextFit.h; 02 §5.8) and FontAtlasSdf::baked(), over
// the bundled face. width() is checked against HR's SdfCanvas::textWidth arithmetic re-derived here from the atlas
// metrics, against 02 §8.3's kMicro figure (5.945 px per character, minus 1.4 on the last) and its worked detent-label
// pairs; fitEllipsis against its own contract. Spec rows only. The note "ellipsis_glyph" records whether the atlas has
// U+2026 (02 §4.6 adds it with Glyphs.def; until then the ellipsis measures 0 and draws nothing).

#include <funkgui/core/TypeScale.h>
#include <funkgui/test/Harness.h>
#include <funkgui/text/BundledFont.h>
#include <funkgui/text/FontAtlasSdf.h>
#include <funkgui/text/TextFit.h>
#include <funkgui/text/TextStyle.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

namespace T = funkgui::test;
namespace text = funkgui::text;
using funkgui::FontAtlasSdf;
using funkgui::TextStyle;

namespace
{
    bool sameBits(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }

    // Decodes all of s; false when any codepoint is malformed (0) — the output of fitEllipsis must never split one.
    bool wellFormed(const char* s)
    {
        for (const char* p = s; *p != 0;)
            if (text::decodeUtf8(p) == 0)
                return false;
        return true;
    }

    // One decode step: the codepoint and the bytes consumed.
    bool decodes(const char* s, uint32_t cp, long bytes)
    {
        const char* p = s;
        const uint32_t got = text::decodeUtf8(p);
        return got == cp && p - s == bytes;
    }

    // fitEllipsis's contract on one case: the result is a codepoint-whole prefix of s (trailing spaces trimmed) plus
    // U+2026, or s itself, or ""; it fits maxW and the buffer; and one codepoint more would not have fit.
    bool fitContract(const FontAtlasSdf& f, const char* s, const TextStyle& st, float maxW, size_t n)
    {
        char out[128];
        const int kept = text::fitEllipsis(f, s, st, maxW, out, n);
        const size_t len = std::strlen(s);
        const size_t outLen = std::strlen(out);
        if (kept < 0 || static_cast<size_t>(kept) > len || outLen + 1 > n || !wellFormed(out))
            return false;
        if (static_cast<size_t>(kept) == len)
            return std::strcmp(out, s) == 0 && text::fits(f, s, st, maxW);
        if (outLen == 0)
            return kept == 0 && (!text::fits(f, "\xE2\x80\xA6", st, maxW) || n < 4);
        const std::string want = std::string(s, static_cast<size_t>(kept)) + "\xE2\x80\xA6";
        if (want != out || !text::fits(f, out, st, maxW) || (kept > 0 && s[kept - 1] == ' '))
            return false;
        // The next longer cut (one more codepoint, then trimmed) must not fit the width or the buffer.
        size_t next = static_cast<size_t>(kept);
        while (next < len && s[next] == ' ')
            ++next;
        if (next >= len)
            return true;
        do
            ++next;
        while (next < len && (static_cast<unsigned char>(s[next]) & 0xC0u) == 0x80u);
        if (next >= len)
            return true;                                 // the next cut is the whole string, which did not fit
        const std::string longer = std::string(s, next) + "\xE2\x80\xA6";
        return !text::fits(f, longer.c_str(), st, maxW) || longer.size() + 1 > n;
    }
}

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;      // the atlas rasterises through JUCE's font stack
    T::Probe P("fg.textfit", "", argc, argv);

    const TextStyle label = funkgui::type::kLabel, micro = funkgui::type::kMicro, valueP = funkgui::type::kValueP;

    // ---- decodeUtf8 -------------------------------------------------------------------------------------------------
    P.eq("utf8.ascii", decodes("A", 'A', 1), 1);
    P.eq("utf8.two_bytes", decodes("\xC2\xB0", 0xB0, 2), 1);
    P.eq("utf8.three_bytes", decodes("\xE2\x88\x92", 0x2212, 3), 1);
    P.eq("utf8.four_bytes", decodes("\xF0\x9F\x98\x80", 0x1F600, 4), 1);
    P.eq("utf8.stray_continuation", decodes("\x80" "A", 0, 1), 1);
    P.eq("utf8.bad_lead", decodes("\xFF" "A", 0, 1), 1);
    P.eq("utf8.truncated_stops_at_nul", decodes("\xE2\x88", 0, 2), 1);
    P.eq("utf8.missing_continuation", decodes("\xE2" "AB", 0, 1), 1);

    // ---- an unbaked atlas measures nothing (and glyph() is safe before the first bake) ------------------------------
    FontAtlasSdf atlas;
    P.eq("unbaked.baked", atlas.baked(), 0);
    P.eq("unbaked.glyph_null", atlas.glyph('A') == nullptr && atlas.glyph(0xB0) == nullptr, 1);
    P.eq("unbaked.width_zero", text::width(atlas, "ABC", label) == 0.0f, 1);
    P.eq("unbaked.fits_only_empty", !text::fits(atlas, "A", label, -1.0f) && text::fits(atlas, "", label, 0.0f), 1);

    // ---- the bundled face -------------------------------------------------------------------------------------------
    const bool bakedOk = atlas.bake(funkgui::BundledFont::data(), funkgui::BundledFont::size());
    P.eq("baked.bake_ok", bakedOk, 1);
    P.eq("baked.baked", atlas.baked(), 1);
    if (!bakedOk)
    {
        P.harnessError("the bundled face did not bake");
        return P.finish();
    }
    P.note("ellipsis_glyph", atlas.glyph(0x2026) != nullptr ? "true" : "false");

    // HR's textWidth, operation for operation, from the atlas metrics.
    const float scaleL = label.px / FontAtlasSdf::kBasePx;
    const float advA = atlas.glyph('A')->advance * scaleL, advB = atlas.glyph('B')->advance * scaleL;
    const float space = atlas.spaceAdvance() * scaleL;
    P.eq("width.empty", text::width(atlas, "", label) == 0.0f && text::width(atlas, nullptr, label) == 0.0f, 1);
    {
        float w = 0.0f;
        w += advA + label.tracking;
        P.eq("width.one_glyph_no_tracking", sameBits(text::width(atlas, "A", label), w - label.tracking), 1);
        w += advB + label.tracking;
        P.eq("width.two_glyphs", sameBits(text::width(atlas, "AB", label), w - label.tracking), 1);
    }
    {
        float w = 0.0f;
        w += advA + label.tracking;
        w += space + label.tracking;
        w += advB + label.tracking;
        P.eq("width.space_advance", sameBits(text::width(atlas, "A B", label), w - label.tracking), 1);
    }
    {
        const float scaleV = valueP.px / FontAtlasSdf::kBasePx;
        const float digit = atlas.maxDigitAdvance() * scaleV;
        float w = 0.0f;
        w += digit + valueP.tracking;
        w += digit + valueP.tracking;
        P.eq("width.tabular_digits", sameBits(text::width(atlas, "10", valueP), w - valueP.tracking), 1);
    }
    P.eq("width.unknown_codepoint_adds_nothing",
         sameBits(text::width(atlas, "A\xC3\xA9" "B", label), text::width(atlas, "AB", label)), 1);
    P.eq("width.malformed_adds_nothing",
         sameBits(text::width(atlas, "A\xFF" "B", label), text::width(atlas, "AB", label)), 1);
    P.eq("width.extra_glyph_counts", text::width(atlas, "A\xC2\xB0", label) > text::width(atlas, "A", label), 1);

    // 02 §8.3: kMicro sets 5.945 px per character minus 1.4 on the last (JetBrains Mono is monospaced).
    P.near("micro.per_char", text::width(atlas, "ALL", micro) - text::width(atlas, "AL", micro), 5.945, 0.005);
    P.near("micro.all", text::width(atlas, "ALL", micro), 3 * 5.945 - 1.4, 0.01);
    P.near("micro.bright", text::width(atlas, "BRIGHT", micro), 6 * 5.945 - 1.4, 0.02);
    // The worked detent-label pairs of 02 §8.3: need = (w_i + w_i+1) / 2 + 2.5.
    const auto need = [&](const char* a, const char* b)
    { return (text::width(atlas, a, micro) + text::width(atlas, b, micro)) / 2.0f + 2.5f; };
    P.near("fit_rule.fet76_20_all", need("20", "ALL"), 16.0, 0.05);
    P.near("fit_rule.busg_1.2_auto", need("1.2", "AUTO"), 21.9, 0.05);
    P.near("fit_rule.clean_diode_bright", need("DIODE", "BRIGHT"), 33.8, 0.05);
    P.near("fit_rule.clean_ms_mid", need("M/S", "MID"), 18.9, 0.05);
    P.near("fit_rule.bus25_.03_.1", need(".03", ".1"), 16.0, 0.05);

    // ---- fits -------------------------------------------------------------------------------------------------------
    const float wRel = text::width(atlas, "RELEASE", label);
    P.eq("fits.exact_width", text::fits(atlas, "RELEASE", label, wRel), 1);
    P.eq("fits.just_under", text::fits(atlas, "RELEASE", label, wRel - 0.001f), 0);
    P.eq("fits.nan_width", text::fits(atlas, "RELEASE", label, std::numeric_limits<float>::quiet_NaN()), 0);

    // ---- fitEllipsis ----------------------------------------------------------------------------------------------
    {
        char out[64];
        P.eq("ellipsis.fits_unchanged", text::fitEllipsis(atlas, "RELEASE", label, wRel, out, sizeof out), 7);
        P.eq("ellipsis.fits_unchanged_text", std::strcmp(out, "RELEASE") == 0, 1);

        // "AB  CD" cut after the spaces: they are trimmed before the ellipsis.
        const float wAb = text::width(atlas, "AB\xE2\x80\xA6", label);
        P.eq("ellipsis.trims_spaces", text::fitEllipsis(atlas, "AB  CD", label, wAb, out, sizeof out), 2);
        P.eq("ellipsis.trims_spaces_text", std::strcmp(out, "AB\xE2\x80\xA6") == 0, 1);

        P.eq("ellipsis.nothing_fits", text::fitEllipsis(atlas, "ABC", label, -1.0f, out, sizeof out), 0);
        P.eq("ellipsis.nothing_fits_empty", out[0] == 0, 1);

        // The buffer also cuts: 4 bytes hold only the ellipsis, 6 hold "AB…".
        P.eq("ellipsis.buffer_4", text::fitEllipsis(atlas, "ABCDEF", label, 1000.0f, out, 4), 0);
        P.eq("ellipsis.buffer_4_text", std::strcmp(out, "\xE2\x80\xA6") == 0, 1);
        P.eq("ellipsis.buffer_6", text::fitEllipsis(atlas, "ABCDEF", label, 1000.0f, out, 6), 2);
        P.eq("ellipsis.buffer_6_text", std::strcmp(out, "AB\xE2\x80\xA6") == 0, 1);
        P.eq("ellipsis.buffer_3_too_small", text::fitEllipsis(atlas, "ABCDEF", label, 1000.0f, out, 3), 0);
        P.eq("ellipsis.buffer_3_empty", out[0] == 0, 1);
        P.eq("ellipsis.whole_string_exact_buffer", text::fitEllipsis(atlas, "ABC", label, 1000.0f, out, 4), 3);

        P.eq("ellipsis.null_out", text::fitEllipsis(atlas, "ABC", label, 100.0f, nullptr, 8), -1);
        P.eq("ellipsis.zero_size", text::fitEllipsis(atlas, "ABC", label, 100.0f, out, 0), -1);
        P.eq("ellipsis.null_text", text::fitEllipsis(atlas, nullptr, label, 100.0f, out, sizeof out), 0);
        P.eq("ellipsis.null_text_empty", out[0] == 0, 1);
    }
    {
        // The contract over a sweep of widths, on ASCII with spaces and on multi-byte text (° × → ·).
        const char* samples[] = { "PRESET NAME FROM ANOTHER MACHINE",
                                  "A\xC2\xB0" "B\xC3\x97" "C\xE2\x86\x92" "D\xC2\xB7" "E",
                                  "  LEADING AND TRAILING  ", "X" };
        int bad = 0, cases = 0;
        for (const char* s : samples)
        {
            const float full = text::width(atlas, s, label);
            for (int k = -2; k <= 42; ++k)
            {
                const float maxW = full * static_cast<float>(k) / 40.0f;
                for (const size_t n : { size_t{ 64 }, size_t{ 12 }, size_t{ 7 } })
                {
                    ++cases;
                    if (!fitContract(atlas, s, label, maxW, n))
                    {
                        ++bad;
                        std::printf("INFO     fitEllipsis contract broken: \"%s\" maxW %g n %zu\n", s,
                                    static_cast<double>(maxW), n);
                    }
                }
            }
        }
        P.eq("ellipsis.contract_sweep_failures", bad, 0);
        P.eq("ellipsis.contract_sweep_cases", cases, 4 * 45 * 3);
    }

    return P.finish();
}

// FUNKGUI_TEST name=fg.gallery.glyphs timeout=600 gpu=0 exe=FunkGuiGalleryProbe args="--section glyphs"
//
// The "glyphs" gallery section, the glyph sheet (FCompressor docs/design/02-funkgui-and-ui.md §3.11, §4.6; G4): every
// codepoint the atlas bakes -- printable ASCII and each FontAtlasSdf::kExtraChars entry, which Glyphs.def generates,
// so an appended glyph joins the sheet without an edit here -- at three sizes, then the strings the extras exist for
// (U+2212 minus, µs, ≤ ≥ ≈, Δ, — • ←, the ellipsis of text::fitEllipsis) in the type scale's styles. GalleryProbe
// requires zero missing glyphs, so a Glyphs.def entry the subset lacks fails here as well as in fg.font.probe. The
// line above registers fg.gallery.glyphs; the tools glob compiles this file into FunkGuiGalleryProbe.

#include "GalleryPanel.h"

#include <funkgui/canvas/Tags.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/text/FontAtlasSdf.h>
#include <funkgui/text/FontService.h>
#include <funkgui/text/TextFit.h>

#include <cstdint>
#include <string>

namespace funkgui::gallery
{
    namespace
    {
        // One codepoint as UTF-8.
        void appendUtf8(std::string& s, uint32_t cp)
        {
            if (cp < 0x80u)
                s += static_cast<char>(cp);
            else if (cp < 0x800u)
            {
                s += static_cast<char>(0xC0u | (cp >> 6));
                s += static_cast<char>(0x80u | (cp & 0x3Fu));
            }
            else if (cp < 0x10000u)
            {
                s += static_cast<char>(0xE0u | (cp >> 12));
                s += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
                s += static_cast<char>(0x80u | (cp & 0x3Fu));
            }
            else
            {
                s += static_cast<char>(0xF0u | (cp >> 18));
                s += static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
                s += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
                s += static_cast<char>(0x80u | (cp & 0x3Fu));
            }
        }

        // Printable ASCII from `first` to `last`, space-separated.
        std::string asciiRun(char first, char last)
        {
            std::string s;
            for (int c = first; c <= last; ++c)
            {
                if (!s.empty())
                    s += ' ';
                s += static_cast<char>(c);
            }
            return s;
        }

        // Every Glyphs.def entry, in atlas order, space-separated.
        std::string extrasRun()
        {
            std::string s;
            for (const uint32_t cp : FontAtlasSdf::kExtraChars)
            {
                if (!s.empty())
                    s += ' ';
                appendUtf8(s, cp);
            }
            return s;
        }

        class GlyphSection final : public Section
        {
        public:
            GlyphSection() : Section(640, 360) {}

            void draw(Canvas& c, const Theme& th) override
            {
                drawSheet(c, th);
                drawInUse(c, th);
            }

        private:
            // The whole baked set: ASCII in three rows, then the extras, at kLabel; the extras again at two sizes.
            static void drawSheet(Canvas& c, const Theme& th)
            {
                const Canvas::Scope s(c, tags::slotLabel, false);
                c.text(asciiRun('!', '?').c_str(), 16.0f, 16.0f, type::kLabel, th.ink100);
                c.text(asciiRun('@', '_').c_str(), 16.0f, 34.0f, type::kLabel, th.ink100);
                c.text(asciiRun('`', '~').c_str(), 16.0f, 52.0f, type::kLabel, th.ink100);
                const std::string extras = extrasRun();
                c.text(extras.c_str(), 16.0f, 74.0f, type::kLabel, th.ink100);
                c.text(extras.c_str(), 16.0f, 94.0f, type::kValueS, th.ink70);
                c.text(extras.c_str(), 16.0f, 124.0f, type::kMicro, th.ink52);
            }

            // What the extras are for (02 §4.6), in the styles that will set them.
            static void drawInUse(Canvas& c, const Theme& th)
            {
                {
                    const Canvas::Scope v(c, tags::slotValue, false);
                    const float top = 150.0f;
                    const char* value = "\xE2\x88\x92" "12.5";                            // −12.5 (U+2212)
                    const float w = c.textWidth(value, type::kDisplay);
                    c.text(value, 16.0f, top, type::kDisplay, th.ink100);
                    c.text("DB", 16.0f + w + 6.0f, c.sharedBaselineTop(top, type::kDisplay, type::kUnit),
                           type::kUnit, th.ink52);
                    c.text("20 \xC2\xB5S", 260.0f, 150.0f, type::kValueP, th.ink100);   // µ
                    c.text("\xE2\x88\x9E:1", 420.0f, 150.0f, type::kValueP, th.ink100);   // ∞
                    c.text("\xE2\x89\xA5 20:1", 260.0f, 184.0f, type::kValueS, th.ink70);  // ≥
                    c.text("\xE2\x89\xA4 0.1 DB", 420.0f, 184.0f, type::kValueS, th.ink70); // ≤
                }
                {
                    const Canvas::Scope sub(c, tags::slotSub, false);
                    c.text("\xE2\x89\x88 THR \xE2\x88\x92" "28 DB", 16.0f, 214.0f, type::kMicro, th.ink52);   // ≈ −
                    c.text("\xCE\x94GR \xE2\x88\x92" "3.0", 180.0f, 214.0f, type::kMicro, th.ink52);          // Δ
                    c.text("KNEE \xC2\xB1" "6 DB \xC2\xB7 12 DB/OCT", 300.0f, 214.0f, type::kMicro, th.ink52);
                }
                {
                    const Canvas::Scope latch(c, tags::latch, false);
                    c.text("\xCE\x94 DELTA", 16.0f, 240.0f, type::kLatch, th.accent);                         // Δ
                    c.text("\xE2\x86\x90 PANEL", 180.0f, 240.0f, type::kLatch, th.ink70);                     // ←
                    c.text("L \xE2\x86\x92 R  \xE2\x86\x91 \xE2\x86\x93", 330.0f, 240.0f, type::kLatch, th.ink70);
                }
                {
                    const Canvas::Scope hint(c, tags::hint, false);
                    c.text("PRESET \xE2\x80\x94 FACTORY \xE2\x80\x94 CLEAN", 16.0f, 272.0f, type::kCaption,
                           th.ink52);                                                               // —
                    c.text("\xE2\x80\xA2 CURRENT", 330.0f, 272.0f, type::kCaption, th.ink100);           // •
                    c.text("20 \xE2\x80\x93 800 \xC2\xB5S  \xC3\x97" "2  45\xC2\xB0", 16.0f, 296.0f,
                           type::kCaption, th.ink52);                                               // – × °
                    // The ellipsis as text::fitEllipsis sets it: a long preset name cut to 150 px.
                    char cut[64];
                    text::fitEllipsis(FontService::get().atlas(), "VOCAL BUS GLUE WITH SLOW RELEASE", type::kLabel,
                                      150.0f, cut, sizeof cut);
                    c.text(cut, 330.0f, 296.0f, type::kLabel, th.ink100);
                }
                c.text("GLYPHS", 16.0f, c.capCentreTop(336.0f, type::kWordmark), type::kWordmark, th.ink100);
            }
        };

        const Registration kGlyphs{ SectionInfo{
            "glyphs",
            [] { return std::make_unique<GlyphSection>(); },
            { State{ "rest", {} } } } };
    }
}

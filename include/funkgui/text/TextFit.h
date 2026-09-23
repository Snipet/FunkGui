#pragma once

// Text measurement without a canvas (02 §5.8). width() is the one definition of a run's width: Canvas::textWidth and
// the detent-label fit rule (02 §8.3) use it, so what a probe measures is what the panel draws. It needs only a baked
// atlas (FontAtlasSdf::baked()); an unbaked atlas measures every run as 0 and fits nothing but the empty string.
//
// Measurement is HR's SdfCanvas::textWidth: per codepoint advance * px / 48 + tracking, the space advance for ' ', the
// widest digit's advance for digits of a tabular style, and no tracking after the last glyph. A codepoint the atlas
// does not hold adds nothing (it would draw nothing: HR skips it; the recorder counts it as a missing glyph).
//
// Typed text (preset names) is made printable by text::printable in text/LineEdit.h, with LineEdit (G6).

#include <funkgui/text/FontAtlasSdf.h>
#include <funkgui/text/TextStyle.h>

#include <cstddef>
#include <cstdint>

namespace funkgui::text
{
    // Decodes one UTF-8 codepoint at p and advances p past it (HR SdfCanvas.cpp:31-51). Malformed input (a stray
    // continuation byte, a lead byte whose continuation is missing, a 5- or 6-byte lead) returns 0 having consumed at
    // least one byte, so a loop over a bad string always ends. p must not point at the terminating NUL.
    uint32_t decodeUtf8(const char*& p) noexcept;

    // The run's advance width in logical px (== Canvas::textWidth). nullptr and "" measure 0.
    float width(const FontAtlasSdf&, const char* utf8, const TextStyle&) noexcept;

    // width(utf8) <= maxW.
    bool fits(const FontAtlasSdf&, const char* utf8, const TextStyle&, float maxW) noexcept;

    // Writes utf8 into out when it fits maxW. Otherwise writes the longest prefix of whole codepoints, with trailing
    // spaces removed, followed by U+2026 HORIZONTAL ELLIPSIS, whose width fits maxW (HR PresetPanel.cpp:81, which
    // faked the ellipsis with ".."); when not even the ellipsis alone fits, writes "". The text is also cut to fit the
    // buffer: n bytes including the NUL. Returns the number of source bytes kept (the whole strlen(utf8) when nothing
    // was cut, so a caller tests `== strlen`), or -1 when out is null or n == 0.
    int fitEllipsis(const FontAtlasSdf&, const char* utf8, const TextStyle&, float maxW, char* out, size_t n) noexcept;
}

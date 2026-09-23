#include <funkgui/text/TextFit.h>

#include <cstring>

namespace funkgui::text
{
    namespace
    {
        constexpr char   kEllipsis[] = "\xE2\x80\xA6";          // U+2026 HORIZONTAL ELLIPSIS
        constexpr size_t kEllipsisBytes = sizeof(kEllipsis) - 1;

        constexpr bool isDigit(uint32_t cp) noexcept { return cp >= '0' && cp <= '9'; }
    }

    uint32_t decodeUtf8(const char*& p) noexcept
    {
        // HR SdfCanvas.cpp:31-51. char is signed on arm64, so each byte is read as unsigned: passing a raw char to
        // glyph() turned every byte of a multi-byte sequence negative and dropped it.
        const auto b0 = static_cast<unsigned char>(*p++);
        if (b0 < 0x80u)
            return b0;

        int extra = 0;
        uint32_t cp = 0;
        if ((b0 & 0xE0u) == 0xC0u)      { extra = 1; cp = b0 & 0x1Fu; }
        else if ((b0 & 0xF0u) == 0xE0u) { extra = 2; cp = b0 & 0x0Fu; }
        else if ((b0 & 0xF8u) == 0xF0u) { extra = 3; cp = b0 & 0x07u; }
        else return 0;

        for (int i = 0; i < extra; ++i)
        {
            const auto b = static_cast<unsigned char>(*p);
            if ((b & 0xC0u) != 0x80u)
                return 0;                                // also stops at the NUL, which is never consumed
            cp = (cp << 6) | (b & 0x3Fu);
            ++p;
        }
        return cp;
    }

    float width(const FontAtlasSdf& f, const char* utf8, const TextStyle& st) noexcept
    {
        if (utf8 == nullptr || !f.baked())
            return 0.0f;

        // HR SdfCanvas::textWidth, operation for operation.
        const float scale = st.px / FontAtlasSdf::kBasePx;
        const float digit = f.maxDigitAdvance() * scale;
        float w = 0.0f;
        for (const char* p = utf8; *p != 0;)
        {
            const uint32_t cp = decodeUtf8(p);
            if (cp == ' ')
            {
                w += f.spaceAdvance() * scale + st.tracking;
                continue;
            }
            if (st.tabular && isDigit(cp))
            {
                w += digit + st.tracking;
                continue;
            }
            if (const auto* g = f.glyph(cp))
                w += g->advance * scale + st.tracking;
        }
        // Tracking is trailing space after the last glyph; excluding it keeps centred and right-aligned runs
        // optically correct.
        return w > 0.0f ? w - st.tracking : 0.0f;
    }

    bool fits(const FontAtlasSdf& f, const char* utf8, const TextStyle& st, float maxW) noexcept
    {
        return width(f, utf8, st) <= maxW;
    }

    int fitEllipsis(const FontAtlasSdf& f, const char* utf8, const TextStyle& st, float maxW, char* out,
                    size_t n) noexcept
    {
        if (out == nullptr || n == 0)
            return -1;
        out[0] = 0;
        if (utf8 == nullptr)
            return 0;

        const size_t len = std::strlen(utf8);
        if (len < n && fits(f, utf8, st, maxW))
        {
            std::memcpy(out, utf8, len + 1);
            return static_cast<int>(len);
        }

        // The longest cut first: every codepoint boundary before the end, from the last one down to 0 (the ellipsis
        // alone). A candidate is written into out (it fits the buffer by construction) and measured there.
        size_t end = len;
        while (end > 0)
        {
            do
                --end;                                   // back to the previous codepoint's lead byte
            while (end > 0 && (static_cast<unsigned char>(utf8[end]) & 0xC0u) == 0x80u);

            size_t keep = end;
            while (keep > 0 && utf8[keep - 1] == ' ')
                --keep;                                  // no space before the ellipsis
            if (keep + kEllipsisBytes >= n)
                continue;                                // not in the buffer; a shorter cut may be
            std::memcpy(out, utf8, keep);
            std::memcpy(out + keep, kEllipsis, kEllipsisBytes + 1);
            if (fits(f, out, st, maxW))
                return static_cast<int>(keep);
        }
        out[0] = 0;
        return 0;
    }
}

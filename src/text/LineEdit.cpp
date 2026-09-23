#include <funkgui/text/LineEdit.h>

#include <funkgui/text/TextFit.h>

#include <juce_core/juce_core.h>

#include <cstddef>
#include <cstdint>

// LineEdit and text::printable (02 §5.8): HR's editKey (PresetPanel.cpp:386-421), its selected pre-fill
// (:317-327, :945-975) and printable (:63-75), over funkgui's KeyEvent instead of juce::KeyPress. The rules are in the
// header. The buffer is printable ASCII by construction; the boundary helpers below still step over UTF-8 continuation
// bytes, so a caller that fills the buffer directly cannot make key() cut a character in half.

namespace funkgui::text
{
    namespace
    {
        bool printableAscii(uint32_t c) noexcept { return c >= 32u && c <= 126u; }

        bool continuation(char c) noexcept { return (static_cast<unsigned char>(c) & 0xC0u) == 0x80u; }

        size_t prevBoundary(const std::string& s, size_t at) noexcept
        {
            if (at == 0)
                return 0;
            --at;
            while (at > 0 && continuation(s[at]))
                --at;
            return at;
        }

        size_t nextBoundary(const std::string& s, size_t at) noexcept
        {
            if (at >= s.size())
                return s.size();
            ++at;
            while (at < s.size() && continuation(s[at]))
                ++at;
            return at;
        }

        size_t codepoints(const std::string& s) noexcept
        {
            size_t n = 0;
            for (const char c : s)
                n += continuation(c) ? 0u : 1u;
            return n;
        }

        // The character a key types, or 0 when it types none (HR: ch 32..126 and neither Cmd nor Ctrl held).
        char32_t typed(const KeyEvent& e) noexcept
        {
            if (e.mods.cmd || e.mods.ctrl)
                return U'\0';
            if (e.key == Key::space)
                return U' ';
            return e.key == Key::character && printableAscii(static_cast<uint32_t>(e.ch)) ? e.ch : U'\0';
        }
    }

    std::string printable(std::string_view utf8)
    {
        std::string out;
        out.reserve(utf8.size());
        const std::string src(utf8);                     // NUL-terminated for decodeUtf8
        for (const char* p = src.c_str(); *p != 0;)
        {
            const uint32_t cp = decodeUtf8(p);
            out += printableAscii(cp) ? static_cast<char>(cp) : '?';
        }
        return out;
    }

    std::string printable(const char* utf8)
    {
        return utf8 != nullptr ? printable(std::string_view(utf8)) : std::string();
    }

    std::string printable(const std::string& utf8)
    {
        return printable(std::string_view(utf8));
    }

    juce::String printable(const juce::String& s)
    {
        juce::String out;
        out.preallocateBytes(s.getNumBytesAsUTF8() + 1);
        for (auto p = s.getCharPointer(); !p.isEmpty(); ++p)
        {
            const juce::juce_wchar c = *p;
            out += c >= 32 && c <= 126 ? juce::String::charToString(c) : juce::String("?");
        }
        return out;
    }

    void LineEdit::set(std::string_view text, int maxLength)
    {
        buffer = printable(text);
        const size_t limit = maxLength > 0 ? static_cast<size_t>(maxLength) : 0u;
        if (buffer.size() > limit)
            buffer.resize(limit);                        // printable ASCII: one byte per character
        caret = static_cast<int>(buffer.size());
        allSelected = !buffer.empty();
    }

    bool LineEdit::key(const KeyEvent& e, int maxLength)
    {
        if (e.key == Key::enter || e.key == Key::escape)
            return false;                                // the product's: commit / cancel; the selection stays
        if (allSelected)
        {
            // The pre-filled text is selected: a character or a delete replaces it all, an arrow keeps it and drops
            // the selection (HR PresetPanel.cpp:954-972).
            allSelected = false;
            const bool erase = e.key == Key::backspace || e.key == Key::del;
            if (erase || typed(e) != U'\0')
            {
                buffer.clear();
                caret = 0;
                if (!erase)
                    edit(e, maxLength);
                return true;
            }
            if (e.key == Key::left)
            {
                caret = 0;
                return true;
            }
            if (e.key == Key::right)
            {
                caret = static_cast<int>(buffer.size());
                return true;
            }
        }
        return edit(e, maxLength);
    }

    bool LineEdit::edit(const KeyEvent& e, int maxLength)
    {
        const size_t len = buffer.size();
        size_t at = caret <= 0 ? 0u : static_cast<size_t>(caret);
        if (at > len)
            at = len;
        while (at > 0 && at < len && continuation(buffer[at]))
            --at;                                        // onto a character boundary
        caret = static_cast<int>(at);

        switch (e.key)
        {
            case Key::backspace:
            {
                if (e.mods.cmd)
                {
                    buffer.erase(0, at);                 // the whole line before the caret
                    caret = 0;
                    return true;
                }
                if (at == 0)
                    return true;
                size_t from = prevBoundary(buffer, at);
                if (e.mods.alt)
                {
                    while (from > 0 && buffer[from] == ' ')
                        --from;                          // over the spaces before the caret
                    while (from > 0 && buffer[from - 1] != ' ')
                        --from;                          // back to the start of the word
                }
                buffer.erase(from, at - from);
                caret = static_cast<int>(from);
                return true;
            }
            case Key::del:
                if (at < len)
                    buffer.erase(at, nextBoundary(buffer, at) - at);
                return true;
            case Key::left:
                caret = static_cast<int>(e.mods.cmd ? 0u : prevBoundary(buffer, at));
                return true;
            case Key::right:
                caret = static_cast<int>(e.mods.cmd ? len : nextBoundary(buffer, at));
                return true;
            case Key::home:
                caret = 0;
                return true;
            case Key::end:
                caret = static_cast<int>(len);
                return true;
            case Key::character:
            case Key::space:
            {
                const char32_t ch = typed(e);
                if (ch == U'\0' || maxLength <= 0 || codepoints(buffer) >= static_cast<size_t>(maxLength))
                    return false;
                buffer.insert(at, 1, static_cast<char>(ch));
                caret = static_cast<int>(at + 1);
                return true;
            }
            case Key::tab:
            case Key::up:
            case Key::down:
            case Key::pageUp:
            case Key::pageDown:
            case Key::escape:
            case Key::enter:
                return false;
        }
        return false;
    }
}

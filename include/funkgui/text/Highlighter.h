#pragma once

// Syntax highlighting for the multi-line editor (widgets/TextEditor.h): the interface a product implements for its own
// language, the span and style records the editor draws from, and KeywordHighlighter, a data-driven highlighter for
// the common shape of a language (keywords, type names, line and block comments, strings, numbers, operators and a
// preprocessor line), so a product highlights C, a shader language or a config file by filling in a table.
//
// A highlighter sees one line at a time and carries a state across lines, so a construct that spans lines (a block
// comment, a here-document) costs no re-tokenising of the whole document: the editor keeps the state every line starts
// in and, after an edit, re-runs the highlighter from the first changed line only until a line's start state comes out
// as it was. Drawing tokenises only the rows on screen. Core, JUCE-free, platform-neutral (also wasm32).

#include <funkgui/core/Col.h>
#include <funkgui/core/Theme.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace funkgui::text
{
    // One run of a line in one style: the bytes [start, end) of the line. `style` indexes the editor's palette
    // (TextEditor::setPalette); 0 is the plain text style, and so is any index past the palette's end.
    struct Span
    {
        int      start = 0;
        int      end = 0;
        uint16_t style = 0;
    };

    class Highlighter
    {
    public:
        virtual ~Highlighter() = default;

        // Tokenises one line (no line break in it; printable ASCII and tabs, as TextEditor keeps its lines).
        // `stateIn` is the state the line starts in: 0 for a document's first line, and for every other line what the
        // previous line returned. Appends spans to `out`, in order and without overlap, each within [0, line.size());
        // a gap between spans is style 0. Returns the state the next line starts in. Pure: the same line and state
        // give the same spans and state, every time, on every platform (the editor relies on it to stop early).
        virtual uint32_t line(std::string_view line, uint32_t stateIn, std::vector<Span>& out) const = 0;
    };

    // How a style index draws: an ink of the theme (so the palette follows a theme switch) or one fixed colour, a
    // weight added to the editor's TextStyle::weight (0.05 reads semibold), and an optional underline.
    struct HighlightStyle
    {
        enum class Ink : uint8_t { ink100, ink70, ink52, ink32, accent, signal, custom };

        Ink   ink = Ink::ink100;
        Col   colour{};                                  // read when ink == Ink::custom
        float weight = 0.0f;
        bool  underline = false;

        constexpr Col resolve(const Theme& th) const noexcept
        {
            switch (ink)
            {
                case Ink::ink70:  return th.ink70;
                case Ink::ink52:  return th.ink52;
                case Ink::ink32:  return th.ink32;
                case Ink::accent: return th.accent;
                case Ink::signal: return th.signal;
                case Ink::custom: return colour;
                case Ink::ink100: break;
            }
            return th.ink100;
        }
    };

    // The table KeywordHighlighter reads. Every field has a default, so `{ .keywords = { "if", "else" } }` is a
    // complete rule set. Words are matched whole: a keyword inside an identifier is not one.
    struct KeywordRules
    {
        std::vector<std::string> keywords{};             // Style::keyword
        std::vector<std::string> types{};                // Style::type
        std::string lineComment{};                       // "//", "#", "--": a comment to the end of the line; "" = none
        std::string blockOpen{}, blockClose{};           // "/*" and "*/": a comment that may span lines; "" = none
        std::string quotes = "\"'";                      // each character opens and closes a string of its own; a
                                                         // backslash escapes the next character; a string ends at the
                                                         // line end at the latest
        std::string operators = "+-*/%=<>!&|^~?:";       // each character is one Style::op span (runs are merged)
        char        preprocessor = 0;                    // '#': a line whose first non-blank character is this is one
                                                         // Style::preprocessor span, to its end; 0 = none
        bool        numbers = true;                      // Style::number: a digit, or '.' then a digit, then any run of
                                                         // letters, digits, '.' and '_' (so 0x1F, 1e-3 is two spans)
        bool        caseSensitive = true;                // keywords and types compared with case

        // C, C++, Java, JavaScript and the shader languages: // and /* */ comments, " and ' strings, # lines, the
        // keywords and types the four share (plus C++'s most common).
        static KeywordRules cLike();
        // Python, shell and config files: # comments, " and ' strings, no block comment, Python's keywords.
        static KeywordRules scriptLike();
    };

    class KeywordHighlighter final : public Highlighter
    {
    public:
        // The style indices it emits; a palette for it has `count` entries (TextEditor::defaultPalette() does).
        enum Style : uint16_t { plain = 0, keyword = 1, type = 2, comment = 3, string = 4, number = 5, op = 6,
                                preprocessor = 7, count = 8 };

        // The states it carries: 0 outside everything, kInBlockComment inside a block comment.
        static constexpr uint32_t kInBlockComment = 1;

        explicit KeywordHighlighter(KeywordRules rules);

        uint32_t line(std::string_view line, uint32_t stateIn, std::vector<Span>& out) const override;

        const KeywordRules& rules() const noexcept { return rules_; }

    private:
        uint16_t wordStyle(std::string_view word) const noexcept;

        KeywordRules             rules_;
        std::vector<std::string> keywords_, types_;      // sorted; lower-cased when !caseSensitive
    };
}

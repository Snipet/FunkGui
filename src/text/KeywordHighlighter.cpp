#include <funkgui/text/Highlighter.h>

#include <algorithm>
#include <cctype>
#include <utility>

// KeywordHighlighter (text/Highlighter.h): one pass over the line, left to right. Precedence at a position: the block
// comment the line started in, then the line comment, the block comment open, a string quote, a preprocessor line
// (only at the first non-blank character), a number, an identifier (keyword, type or plain), an operator run.
// Everything is byte-based: the editor's lines are printable ASCII and tabs.

namespace funkgui::text
{
    namespace
    {
        bool isIdentStart(char c) noexcept { return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_'; }
        bool isIdentChar(char c) noexcept  { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }
        bool isDigitChar(char c) noexcept  { return c >= '0' && c <= '9'; }
        bool isNumberChar(char c) noexcept { return isIdentChar(c) || c == '.'; }
        bool isBlank(char c) noexcept      { return c == ' ' || c == '\t'; }

        std::string lowered(std::string_view s)
        {
            std::string out(s);
            for (char& c : out)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return out;
        }

        bool startsAt(std::string_view s, size_t at, std::string_view what) noexcept
        {
            return !what.empty() && s.size() >= at + what.size() && s.compare(at, what.size(), what) == 0;
        }

        void push(std::vector<Span>& out, size_t start, size_t end, uint16_t style)
        {
            if (end <= start || style == KeywordHighlighter::plain)
                return;                                  // plain text is the gap between spans
            // Adjacent spans of one style merge, so an operator run such as "->" is one span.
            if (!out.empty() && out.back().style == style && out.back().end == static_cast<int>(start))
            {
                out.back().end = static_cast<int>(end);
                return;
            }
            out.push_back({ static_cast<int>(start), static_cast<int>(end), style });
        }
    }

    KeywordRules KeywordRules::cLike()
    {
        KeywordRules r;
        r.keywords = { "alignas", "alignof", "auto", "break", "case", "catch", "class", "const", "constexpr",
                       "continue", "default", "delete", "do", "else", "enum", "explicit", "export", "extern", "false",
                       "final", "for", "friend", "function", "goto", "if", "import", "in", "inline", "instanceof",
                       "let", "mutable", "namespace", "new", "noexcept", "nullptr", "null", "operator", "override",
                       "private", "protected", "public", "return", "sizeof", "static", "struct", "switch",
                       "template", "this", "throw", "true", "try", "typedef", "typename", "undefined", "union",
                       "using", "var", "virtual", "volatile", "while", "yield" };
        r.types = { "bool", "char", "double", "float", "int", "int8_t", "int16_t", "int32_t", "int64_t", "long",
                    "short", "size_t", "string", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "unsigned", "void",
                    "vec2", "vec3", "vec4", "mat3", "mat4" };
        r.lineComment = "//";
        r.blockOpen = "/*";
        r.blockClose = "*/";
        r.preprocessor = '#';
        return r;
    }

    KeywordRules KeywordRules::scriptLike()
    {
        KeywordRules r;
        r.keywords = { "and", "as", "assert", "async", "await", "break", "case", "class", "continue", "def", "del",
                       "do", "done", "elif", "else", "esac", "except", "export", "fi", "finally", "for", "from",
                       "function", "global", "if", "import", "in", "is", "lambda", "local", "nonlocal", "not", "or",
                       "pass", "raise", "return", "then", "try", "while", "with", "yield" };
        r.types = { "False", "None", "True", "bool", "dict", "float", "int", "list", "set", "str", "tuple" };
        r.lineComment = "#";
        return r;
    }

    KeywordHighlighter::KeywordHighlighter(KeywordRules rules) : rules_(std::move(rules))
    {
        const auto prepare = [this](const std::vector<std::string>& in, std::vector<std::string>& out) {
            out.clear();
            for (const std::string& w : in)
                if (!w.empty())
                    out.push_back(rules_.caseSensitive ? w : lowered(w));
            std::sort(out.begin(), out.end());
            out.erase(std::unique(out.begin(), out.end()), out.end());
        };
        prepare(rules_.keywords, keywords_);
        prepare(rules_.types, types_);
    }

    uint16_t KeywordHighlighter::wordStyle(std::string_view word) const noexcept
    {
        const std::string key = rules_.caseSensitive ? std::string(word) : lowered(word);
        if (std::binary_search(keywords_.begin(), keywords_.end(), key))
            return keyword;
        if (std::binary_search(types_.begin(), types_.end(), key))
            return type;
        return plain;
    }

    uint32_t KeywordHighlighter::line(std::string_view s, uint32_t stateIn, std::vector<Span>& out) const
    {
        const size_t n = s.size();
        size_t i = 0;
        bool inBlock = stateIn == kInBlockComment && !rules_.blockClose.empty();
        bool sawCode = false;                            // something other than blanks before this position

        while (i < n)
        {
            if (inBlock)
            {
                const size_t close = rules_.blockClose.empty() ? std::string_view::npos
                                                               : s.find(rules_.blockClose, i);
                if (close == std::string_view::npos)
                {
                    push(out, i, n, comment);
                    return kInBlockComment;              // the comment goes on past this line
                }
                push(out, i, close + rules_.blockClose.size(), comment);
                i = close + rules_.blockClose.size();
                inBlock = false;
                sawCode = true;
                continue;
            }

            const char c = s[i];
            if (isBlank(c))
            {
                ++i;
                continue;
            }
            if (startsAt(s, i, rules_.lineComment))
            {
                push(out, i, n, comment);
                return 0;
            }
            if (startsAt(s, i, rules_.blockOpen) && !rules_.blockClose.empty())
            {
                const size_t from = i;
                i += rules_.blockOpen.size();
                const size_t close = s.find(rules_.blockClose, i);
                if (close == std::string_view::npos)
                {
                    push(out, from, n, comment);
                    return kInBlockComment;
                }
                i = close + rules_.blockClose.size();
                push(out, from, i, comment);
                sawCode = true;
                continue;
            }
            if (rules_.quotes.find(c) != std::string::npos)
            {
                const size_t from = i++;
                while (i < n && s[i] != c)
                    i += (s[i] == '\\' && i + 1 < n) ? 2u : 1u;   // an escaped character, or any other
                if (i < n)
                    ++i;                                 // the closing quote
                push(out, from, i, string);
                sawCode = true;
                continue;
            }
            if (!sawCode && rules_.preprocessor != 0 && c == rules_.preprocessor)
            {
                push(out, i, n, preprocessor);
                return 0;
            }
            if (rules_.numbers && (isDigitChar(c) || (c == '.' && i + 1 < n && isDigitChar(s[i + 1]))))
            {
                const size_t from = i++;
                while (i < n && isNumberChar(s[i]))
                    ++i;
                push(out, from, i, number);
                sawCode = true;
                continue;
            }
            if (isIdentStart(c))
            {
                const size_t from = i++;
                while (i < n && isIdentChar(s[i]))
                    ++i;
                push(out, from, i, wordStyle(s.substr(from, i - from)));
                sawCode = true;
                continue;
            }
            if (rules_.operators.find(c) != std::string::npos)
                push(out, i, i + 1, op);
            ++i;                                         // punctuation, brackets: plain
            sawCode = true;
        }
        return inBlock ? kInBlockComment : 0u;
    }
}

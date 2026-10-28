#include <funkgui/widgets/TextEditor.h>

#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Ease.h>
#include <funkgui/core/Theme.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/text/FontService.h>
#include <funkgui/text/TextFit.h>
#include <funkgui/widgets/FocusRing.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <initializer_list>
#include <utility>

// TextEditor (widgets/TextEditor.h): the rules are in the header; fg.texteditor pins them. The text is a vector of
// lines; every change goes through spliceText(), which splices the lines, lays the changed ones out again (their
// width, and their wrap breaks when word wrap is on), re-runs the highlighter from the first changed line until a
// start state comes out unchanged and moves the decorations, the line marks and the auto-inserted closers with the
// text; change() records it as one undo step (or extends the last one), and applyEdits() records several splices as
// one step of several pieces, undone in reverse. Layout is in px from the
// font's advances at options().style, one table for the 95 printable characters, so hit tests and the caret agree
// with what Canvas::text draws: a run's glyphs sit at pen += advance + tracking, and the x of column k is the sum of
// the advances before it, tabs jumping to the next stop. A visual row is a line, or one piece of a wrapped line; rows
// are indexed through rowOfLine_, the prefix sum of each line's row count.

namespace funkgui
{
    namespace
    {
        constexpr float kBorder = 1.0f, kRadius = 2.0f;
        constexpr float kCaretW = 1.5f;
        constexpr float kBarW = 4.0f, kBarInset = 2.0f, kBarMin = 12.0f;
        constexpr float kGutterPad = 6.0f;
        constexpr float kWheelPxPerUnit = 512.0f;        // smooth: JUCE's precise device gives 0.5/256 per point
        constexpr float kWheelNotch = 50.0f / 256.0f;    // one notch of a notched wheel (JUCE on X11)
        constexpr float kRowsPerNotch = 3.0f;
        constexpr float kWrapSlack = 2.0f;               // a wrapped row leaves this much room for the caret

        bool isWordChar(char c) noexcept { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }
        bool isBlank(char c) noexcept { return c == ' ' || c == '\t'; }
        bool printableAscii(uint32_t c) noexcept { return c >= 32u && c <= 126u; }

        char32_t lowerAscii(char32_t ch) noexcept
        {
            return ch >= U'A' && ch <= U'Z' ? ch + (U'a' - U'A') : ch;
        }

        char foldChar(char c, bool caseSensitive) noexcept
        {
            return caseSensitive ? c : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        int digitsOf(int n) noexcept
        {
            int d = 1;
            while (n >= 10)
            {
                n /= 10;
                ++d;
            }
            return d;
        }

        int clampInt(int v, int lo, int hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }

        // The first match of needle in s at or after `from`, or -1.
        int findIn(const std::string& s, std::string_view needle, int from, bool caseSensitive) noexcept
        {
            const int n = static_cast<int>(s.size()), m = static_cast<int>(needle.size());
            for (int at = std::max(from, 0); at + m <= n; ++at)
            {
                bool hit = true;
                for (int k = 0; k < m && hit; ++k)
                    hit = foldChar(s[static_cast<size_t>(at + k)], caseSensitive)
                       == foldChar(needle[static_cast<size_t>(k)], caseSensitive);
                if (hit)
                    return at;
            }
            return -1;
        }

        // The last match of needle in s that ends at or before `before`, or -1.
        int rfindIn(const std::string& s, std::string_view needle, int before, bool caseSensitive) noexcept
        {
            const int m = static_cast<int>(needle.size());
            for (int at = std::min(before, static_cast<int>(s.size())) - m; at >= 0; --at)
            {
                bool hit = true;
                for (int k = 0; k < m && hit; ++k)
                    hit = foldChar(s[static_cast<size_t>(at + k)], caseSensitive)
                       == foldChar(needle[static_cast<size_t>(k)], caseSensitive);
                if (hit)
                    return at;
            }
            return -1;
        }

        std::vector<std::string> splitLines(const std::string& s)
        {
            std::vector<std::string> out;
            size_t pos = 0;
            for (;;)
            {
                const size_t nl = s.find('\n', pos);
                if (nl == std::string::npos)
                {
                    out.push_back(s.substr(pos));
                    return out;
                }
                out.push_back(s.substr(pos, nl - pos));
                pos = nl + 1;
            }
        }

        template <typename V>
        void splice(V& v, int at, int oldCount, int newCount, const typename V::value_type& fill)
        {
            const auto first = v.begin() + static_cast<typename V::difference_type>(at);
            v.erase(first, first + static_cast<typename V::difference_type>(oldCount));
            v.insert(v.begin() + static_cast<typename V::difference_type>(at), static_cast<size_t>(newCount), fill);
        }

        // ---- v0.22.0: the code-editor hooks -------------------------------------------------------------------------

        constexpr float kSquiggleHalf = 2.0f;            // half a period of the wavy underline
        constexpr float kSquiggleAmp = 2.0f;             // its height
        constexpr float kMapRowH = 2.0f, kMapColW = 1.0f, kMapBarH = 1.5f, kMapPad = 4.0f, kMapMinW = 16.0f;
        constexpr int   kBracketScan = 20000;            // characters a bracket match looks through
        constexpr int   kGuideScan = 100;                // lines a blank line looks through for its guides

        bool isOpenBracket(char c) noexcept { return c == '(' || c == '[' || c == '{'; }
        bool isCloseBracket(char c) noexcept { return c == ')' || c == ']' || c == '}'; }
        bool isQuote(char c) noexcept { return c == '"' || c == '\''; }

        char closerOf(char c) noexcept
        {
            switch (c)
            {
                case '(': return ')';
                case '[': return ']';
                case '{': return '}';
                case '"': return '"';
                case '\'': return '\'';
                default:  return 0;
            }
        }

        char openerOf(char c) noexcept
        {
            switch (c)
            {
                case ')': return '(';
                case ']': return '[';
                case '}': return '{';
                default:  return 0;
            }
        }

        // autoClosePairs closes an opener only before these (or a blank, or the line's end).
        bool closesBefore(char c) noexcept
        {
            return isBlank(c) || std::string_view(";:.,=}])>").find(c) != std::string_view::npos;
        }

        // A position at or after b, moved with the text after a splice of [a, b) whose replacement ends at `end`.
        TextPos shiftAfter(TextPos p, TextPos b, TextPos end) noexcept
        {
            return { p.line + (end.line - b.line), p.line == b.line ? end.col + (p.col - b.col) : p.col };
        }

        // A point (a caret, a range's start, an empty range): before the span it stays, inside it goes to the
        // replacement's end, at or after the span's end it moves with the text there.
        TextPos mapPoint(TextPos p, TextPos a, TextPos b, TextPos end) noexcept
        {
            return p < a ? p : (p < b ? end : shiftAfter(p, b, end));
        }

        // A range's end: at or before the span's start it stays (text typed there does not widen the range), inside the
        // span it goes to the span's start.
        TextPos mapEnd(TextPos p, TextPos a, TextPos b, TextPos end) noexcept
        {
            return p <= a ? p : (p < b ? a : shiftAfter(p, b, end));
        }

        // The position of byte `off` of a text joined with LF.
        TextPos posOfOffset(const std::string& s, size_t off) noexcept
        {
            off = std::min(off, s.size());
            int line = 0;
            size_t lineStart = 0;
            for (size_t k = 0; k < off; ++k)
                if (s[k] == '\n')
                {
                    ++line;
                    lineStart = k + 1;
                }
            return { line, static_cast<int>(off - lineStart) };
        }

        int severity(LineMark::Kind k) noexcept { return static_cast<int>(k); }
    }

    // ---- construction and configuration -----------------------------------------------------------------------------

    TextEditor::TextEditor(uint32_t a11yId) : a11yId_(a11yId)
    {
        lines_.emplace_back();
        breaks_.emplace_back();
        lineW_.push_back(0.0f);
        lineState_.push_back(0);
        rowOfLine_ = { 0, 1 };
        rebuildMetrics();
    }

    TextEditor::~TextEditor() = default;

    void TextEditor::setOptions(const TextEditorOptions& o)
    {
        options_ = o;
        options_.tabWidth = std::max(1, options_.tabWidth);
        options_.maxUndo = std::max(0, options_.maxUndo);
        options_.padding = std::max(0.0f, options_.padding);
        if (!(options_.lineSpacing > 0.0f))
            options_.lineSpacing = 1.0f;
        if (!std::isfinite(options_.minimapWidth) || options_.minimapWidth < kMapMinW)
            options_.minimapWidth = kMapMinW;
        pairValid_ = false;
        rebuildMetrics();
        relayoutAll();
        clampScroll();
    }

    void TextEditor::setFont(EditorFont f) { setStyle(editorFont(f)); }

    void TextEditor::setStyle(const TextStyle& st)
    {
        options_.style = st;
        rebuildMetrics();
        relayoutAll();
        clampScroll();
    }

    void TextEditor::setBounds(const Rect& r)
    {
        const bool widthChanged = !ease::sameBits(r.w, bounds_.w);   // no -Wfloat-equal in a consumer's build
        bounds_ = r;
        if (widthChanged)
            relayoutAll();
        clampScroll();
    }

    void TextEditor::attach(HostServices* host) { host_ = host; }

    void TextEditor::setHighlighter(std::unique_ptr<text::Highlighter> h)
    {
        highlighter_ = std::move(h);
        std::fill(lineState_.begin(), lineState_.end(), 0u);
        spansLine_ = -1;
        ++styleGen_;
        rehighlightFrom(0, lineCount() - 1);
    }

    void TextEditor::setPalette(std::vector<text::HighlightStyle> p) { palette_ = std::move(p); }

    std::vector<text::HighlightStyle> TextEditor::defaultPalette()
    {
        using HS = text::HighlightStyle;
        using Ink = HS::Ink;
        return {
            HS{ Ink::ink100, {}, 0.00f, false },         // plain
            HS{ Ink::accent, {}, 0.03f, false },         // keyword
            HS{ Ink::ink70,  {}, 0.03f, false },         // type
            HS{ Ink::ink32,  {}, 0.00f, false },         // comment
            HS{ Ink::ink70,  {}, 0.00f, false },         // string
            HS{ Ink::ink100, {}, 0.00f, false },         // number
            HS{ Ink::ink52,  {}, 0.00f, false },         // operator
            HS{ Ink::ink52,  {}, 0.02f, false },         // preprocessor
        };
    }

    void TextEditor::rebuildMetrics()
    {
        const FontAtlasSdf& f = FontService::get().atlas();
        const TextStyle& st = options_.style;
        fontOk_ = f.baked();
        scale_ = st.px / FontAtlasSdf::kBasePx;
        for (float& a : adv_)
            a = 0.0f;
        digitW_ = 0.0f;
        if (fontOk_)
        {
            for (int c = 32; c <= 126; ++c)
            {
                float a = 0.0f;
                if (c == ' ')
                    a = f.spaceAdvance() * scale_;
                else if (st.tabular && c >= '0' && c <= '9')
                    a = f.maxDigitAdvance() * scale_;
                else if (const auto* g = f.glyph(static_cast<uint32_t>(c)))
                    a = g->advance * scale_;
                adv_[c] = a + st.tracking;
            }
            digitW_ = f.maxDigitAdvance() * scale_ + st.tracking;   // the gutter sets its numbers tabular
        }
        spaceW_ = adv_[static_cast<int>(' ')];
        tabW_ = spaceW_ * static_cast<float>(std::max(1, options_.tabWidth));
        rowH_ = std::max(1.0f, std::round(st.px * options_.lineSpacing));
        textTop_ = (rowH_ - st.px) * 0.5f;
    }

    // ---- layout -----------------------------------------------------------------------------------------------------

    float TextEditor::penAfter(const std::string& s, int k, float pen) const noexcept
    {
        const auto c = static_cast<unsigned char>(s[static_cast<size_t>(k)]);
        if (c == '\t')
            return tabW_ > 0.0f ? (std::floor(pen / tabW_ + 1.0e-4f) + 1.0f) * tabW_ : pen;
        return pen + (c < 128u ? adv_[c] : 0.0f);
    }

    float TextEditor::xOf(const std::string& s, int from, int col) const noexcept
    {
        float pen = 0.0f;
        const int to = std::min(col, static_cast<int>(s.size()));
        for (int k = from; k < to; ++k)
            pen = penAfter(s, k, pen);
        return pen;
    }

    int TextEditor::colAt(const std::string& s, int from, int to, float x) const noexcept
    {
        float pen = 0.0f;
        for (int k = from; k < to; ++k)
        {
            const float next = penAfter(s, k, pen);
            if (x < (pen + next) * 0.5f)
                return k;
            pen = next;
        }
        return to;
    }

    void TextEditor::relayoutLine(int i)
    {
        const std::string& s = lines_[static_cast<size_t>(i)];
        std::vector<int>& br = breaks_[static_cast<size_t>(i)];
        br.clear();
        const int n = static_cast<int>(s.size());
        const float wrapW = textArea().w - kWrapSlack;
        const bool wrap = options_.wordWrap && spaceW_ > 0.0f && wrapW >= 2.0f * spaceW_;
        float pen = 0.0f, widest = 0.0f;
        int rowStart = 0, lastBlankEnd = -1, k = 0;
        while (k < n)
        {
            const float next = penAfter(s, k, pen);
            if (wrap && next > wrapW && k > rowStart)
            {
                const int brk = lastBlankEnd > rowStart ? lastBlankEnd : k;   // after the last blank, else here
                br.push_back(brk);
                widest = std::max(widest, xOf(s, rowStart, brk));
                rowStart = brk;
                pen = 0.0f;
                lastBlankEnd = -1;
                k = brk;
                continue;
            }
            pen = next;
            if (isBlank(s[static_cast<size_t>(k)]))
                lastBlankEnd = k + 1;
            ++k;
        }
        lineW_[static_cast<size_t>(i)] = std::max(widest, pen);
    }

    void TextEditor::reindexRows()
    {
        const size_t n = lines_.size();
        rowOfLine_.resize(n + 1);
        rowOfLine_[0] = 0;
        for (size_t i = 0; i < n; ++i)
            rowOfLine_[i + 1] = rowOfLine_[i] + static_cast<int>(breaks_[i].size()) + 1;
        maxLineW_ = 0.0f;
        for (const float w : lineW_)
            maxLineW_ = std::max(maxLineW_, w);
    }

    void TextEditor::relayoutAll()
    {
        for (int i = 0; i < lineCount(); ++i)
            relayoutLine(i);
        reindexRows();
    }

    int TextEditor::rowCount() const noexcept { return rowOfLine_.back(); }

    void TextEditor::rowRange(int row, int& ln, int& from, int& to) const noexcept
    {
        const int n = lineCount();
        const auto it = std::upper_bound(rowOfLine_.begin(), rowOfLine_.end(), row);   // the first > row
        ln = clampInt(static_cast<int>(it - rowOfLine_.begin()) - 1, 0, n - 1);
        const std::vector<int>& br = breaks_[static_cast<size_t>(ln)];
        const int sub = clampInt(row - rowOfLine_[static_cast<size_t>(ln)], 0, static_cast<int>(br.size()));
        from = sub <= 0 ? 0 : br[static_cast<size_t>(sub - 1)];
        to = sub < static_cast<int>(br.size()) ? br[static_cast<size_t>(sub)]
                                               : static_cast<int>(lines_[static_cast<size_t>(ln)].size());
    }

    int TextEditor::rowOf(TextPos p) const
    {
        p = clampPos(p);
        const std::vector<int>& br = breaks_[static_cast<size_t>(p.line)];
        const auto it = std::upper_bound(br.begin(), br.end(), p.col);              // breaks <= col are passed
        return rowOfLine_[static_cast<size_t>(p.line)] + static_cast<int>(it - br.begin());
    }

    int TextEditor::rowOfCaret() const noexcept
    {
        int row = rowOf(caret_);
        if (caretUp_)
        {
            // The caret sits at a wrap break and belongs to the row before it (End on a wrapped row, a click past
            // its last character).
            const int sub = row - rowOfLine_[static_cast<size_t>(caret_.line)];
            const std::vector<int>& br = breaks_[static_cast<size_t>(caret_.line)];
            if (sub > 0 && br[static_cast<size_t>(sub - 1)] == caret_.col)
                --row;
        }
        return row;
    }

    void TextEditor::caretRowRange(int& from, int& to) const noexcept
    {
        int ln;
        rowRange(rowOfCaret(), ln, from, to);
    }

    int TextEditor::rowStartOf(TextPos p) const noexcept
    {
        int ln, from, to;
        rowRange(rowOf(p), ln, from, to);
        return from;
    }

    int TextEditor::rowEndOf(TextPos p) const noexcept
    {
        int ln, from, to;
        rowRange(rowOf(p), ln, from, to);
        return to;
    }

    Rect TextEditor::innerRect() const noexcept { return bounds_.reduced(kBorder); }

    float TextEditor::markColumnWidth() const noexcept
    {
        return options_.markColumn ? std::max(1.0f, std::round(options_.style.px)) : 0.0f;
    }

    float TextEditor::gutterWidth() const
    {
        const float marks = markColumnWidth();
        if (!options_.lineNumbers)
            return marks;
        return marks + static_cast<float>(std::max(2, digitsOf(lineCount()))) * digitW_ + 2.0f * kGutterPad;
    }

    float TextEditor::rightEdge() const noexcept
    {
        const Rect inner = innerRect();
        if (!options_.minimap)
            return inner.right();
        return inner.right() - std::min(options_.minimapWidth, inner.w * 0.5f);
    }

    Rect TextEditor::textArea() const
    {
        const Rect inner = innerRect();
        const float pad = options_.padding;
        const float x = inner.x + gutterWidth() + pad, y = inner.y + pad;
        return { x, y, std::max(0.0f, rightEdge() - pad - x), std::max(0.0f, inner.h - 2.0f * pad) };
    }

    Rect TextEditor::rowsClip() const noexcept
    {
        const Rect inner = innerRect(), ta = textArea();
        return { inner.x, ta.y, std::max(0.0f, rightEdge() - inner.x), ta.h };
    }

    Rect TextEditor::minimapArea() const noexcept
    {
        if (!options_.minimap)
            return {};
        const Rect inner = innerRect(), ta = textArea();
        const float x = rightEdge();
        return { x, ta.y, std::max(0.0f, inner.right() - x), ta.h };
    }

    // The minimap's own scroll (px of its rows above its top), and its slider. The slider shades the rows in view and
    // runs over the rows the strip shows, top to bottom as the text scrolls top to bottom; the strip scrolls only when
    // its rows do not fit.
    void TextEditor::minimapGeometry(float& offset, float& sliderY, float& sliderH) const noexcept
    {
        const Rect m = minimapArea(), ta = textArea();
        const float content = static_cast<float>(rowCount()) * kMapRowH;
        const float shown = std::min(m.h, content);
        sliderH = std::min(shown, ta.h / rowH_ * kMapRowH);
        const float range = maxScrollY();
        const float t = range > 0.0f ? scrollY_ / range : 0.0f;
        offset = t * std::max(0.0f, content - m.h);
        sliderY = m.y + t * (shown - sliderH);
    }

    bool TextEditor::vbarShown() const noexcept { return contentHeight() > textArea().h + 0.5f; }

    bool TextEditor::hbarShown() const noexcept
    {
        return !options_.wordWrap && maxLineW_ + kCaretW > textArea().w + 0.5f;
    }

    float TextEditor::maxScrollY() const noexcept { return std::max(0.0f, contentHeight() - textArea().h); }

    float TextEditor::maxScrollX() const noexcept
    {
        return options_.wordWrap ? 0.0f : std::max(0.0f, maxLineW_ + kCaretW + 1.0f - textArea().w);
    }

    Rect TextEditor::vbarRect(float& thumbY, float& thumbH) const noexcept
    {
        const Rect ta = textArea();
        const Rect r{ rightEdge() - kBarInset - kBarW, ta.y, kBarW,
                      std::max(0.0f, ta.h - (hbarShown() ? kBarW + kBarInset : 0.0f)) };
        const float content = std::max(contentHeight(), 1.0f);
        thumbH = std::min(r.h, std::max(kBarMin, r.h * ta.h / content));
        const float range = maxScrollY();
        thumbY = r.y + (range > 0.0f ? scrollY_ / range : 0.0f) * (r.h - thumbH);
        return r;
    }

    Rect TextEditor::hbarRect(float& thumbX, float& thumbW) const noexcept
    {
        const Rect inner = innerRect(), ta = textArea();
        const Rect r{ ta.x, inner.bottom() - kBarInset - kBarW,
                      std::max(0.0f, ta.w - (vbarShown() ? kBarW + kBarInset : 0.0f)), kBarW };
        const float content = std::max(maxLineW_ + kCaretW + 1.0f, 1.0f);
        thumbW = std::min(r.w, std::max(kBarMin, r.w * ta.w / content));
        const float range = maxScrollX();
        thumbX = r.x + (range > 0.0f ? scrollX_ / range : 0.0f) * (r.w - thumbW);
        return r;
    }

    void TextEditor::clampScroll()
    {
        scrollX_ = std::min(std::max(0.0f, scrollX_), maxScrollX());
        scrollY_ = std::min(std::max(0.0f, scrollY_), maxScrollY());
    }

    void TextEditor::setScroll(float x, float y)
    {
        scrollX_ = std::isfinite(x) ? x : 0.0f;
        scrollY_ = std::isfinite(y) ? y : 0.0f;
        clampScroll();
    }

    void TextEditor::scrollToCaret()
    {
        const Rect ta = textArea();
        const int row = rowOfCaret();
        const float top = static_cast<float>(row) * rowH_, bottom = top + rowH_;
        if (top < scrollY_)
            scrollY_ = top;
        else if (bottom > scrollY_ + ta.h)
            scrollY_ = bottom - ta.h;
        if (!options_.wordWrap)
        {
            int ln, from, to;
            rowRange(row, ln, from, to);
            const float x = xOf(lines_[static_cast<size_t>(ln)], from, caret_.col);
            if (x < scrollX_)
                scrollX_ = x;
            else if (x + kCaretW > scrollX_ + ta.w)
                scrollX_ = x + kCaretW - ta.w;
        }
        clampScroll();
    }

    Point TextEditor::caretPoint() const
    {
        const Rect ta = textArea();
        const int row = rowOfCaret();
        int ln, from, to;
        rowRange(row, ln, from, to);
        const float x = xOf(lines_[static_cast<size_t>(ln)], from, caret_.col);
        return { ta.x - scrollX_ + x, ta.y - scrollY_ + static_cast<float>(row) * rowH_ };
    }

    TextPos TextEditor::hitTest(Point p, bool& up) const
    {
        const Rect ta = textArea();
        const int rows = rowCount();
        const float y = p.y - ta.y + scrollY_;
        const int row = clampInt(static_cast<int>(std::floor(y / rowH_)), 0, rows - 1);
        int ln, from, to;
        rowRange(row, ln, from, to);
        const std::string& s = lines_[static_cast<size_t>(ln)];
        const int col = colAt(s, from, to, p.x - ta.x + scrollX_);
        up = col == to && to < static_cast<int>(s.size());
        return { ln, col };
    }

    TextPos TextEditor::positionAt(Point p) const
    {
        bool up = false;
        return hitTest(p, up);
    }

    // ---- geometry for overlays --------------------------------------------------------------------------------------

    Point TextEditor::pointOf(TextPos p) const
    {
        p = clampPos(p);
        const Rect ta = textArea();
        const int row = rowOf(p);
        int ln, from, to;
        rowRange(row, ln, from, to);
        const float x = xOf(lines_[static_cast<size_t>(ln)], from, p.col);
        return { ta.x - scrollX_ + x, ta.y - scrollY_ + static_cast<float>(row) * rowH_ };
    }

    std::optional<TextPos> TextEditor::posAt(Point p) const
    {
        const Rect ta = textArea();
        const float left = innerRect().x + gutterWidth();
        if (!(p.x >= left && p.x < rightEdge() && p.y >= ta.y && p.y < ta.bottom()))
            return std::nullopt;
        if (p.y - ta.y + scrollY_ >= static_cast<float>(rowCount()) * rowH_)
            return std::nullopt;                         // below the last row
        bool up = false;
        return hitTest(p, up);
    }

    LineRange TextEditor::visibleLineRange() const
    {
        const Rect ta = textArea();
        const int rows = rowCount();
        const int first = clampInt(static_cast<int>(std::floor(scrollY_ / rowH_)), 0, rows - 1);
        const int last = clampInt(static_cast<int>(std::ceil((scrollY_ + ta.h) / rowH_)) - 1, first, rows - 1);
        int l0, l1, from, to;
        rowRange(first, l0, from, to);
        rowRange(last, l1, from, to);
        return { l0, l1 };
    }

    TextRange TextEditor::wordRangeAt(TextPos p) const
    {
        p = clampPos(p);
        const std::string& s = lines_[static_cast<size_t>(p.line)];
        int a = p.col, b = p.col;
        while (a > 0 && isWordChar(s[static_cast<size_t>(a - 1)]))
            --a;
        while (b < static_cast<int>(s.size()) && isWordChar(s[static_cast<size_t>(b)]))
            ++b;
        return { { p.line, a }, { p.line, b } };
    }

    // ---- the text ---------------------------------------------------------------------------------------------------

    std::string TextEditor::sanitise(std::string_view utf8)
    {
        std::string out;
        out.reserve(utf8.size());
        const std::string src(utf8);                     // NUL-terminated for decodeUtf8
        for (const char* p = src.c_str(); *p != 0;)
        {
            if (*p == '\r')
            {
                ++p;
                if (*p == '\n')
                    ++p;                                 // CR LF is one line break
                out += '\n';
                continue;
            }
            if (*p == '\n' || *p == '\t')
            {
                out += *p++;
                continue;
            }
            const uint32_t cp = text::decodeUtf8(p);
            out += printableAscii(cp) ? static_cast<char>(cp) : '?';
        }
        return out;
    }

    void TextEditor::setText(std::string_view utf8)
    {
        lines_ = splitLines(sanitise(utf8));
        const size_t n = lines_.size();
        breaks_.assign(n, {});
        lineW_.assign(n, 0.0f);
        lineState_.assign(n, 0u);
        relayoutAll();
        rehighlightFrom(0, static_cast<int>(n) - 1);
        caret_ = anchor_ = {};
        goalX_ = -1.0f;
        caretUp_ = false;
        scrollX_ = scrollY_ = 0.0f;
        blinkT_ = 0.0f;
        undo_.clear();
        redo_.clear();
        decorations_.clear();
        lineMarks_.clear();
        autoClosers_.clear();
        drag_ = Drag::none;
        ++revision_;
        modified_ = false;
        spansLine_ = -1;
    }

    std::string TextEditor::text() const { return rangeText({}, endPos()); }

    const std::string& TextEditor::line(int i) const
    {
        static const std::string empty;
        return i >= 0 && i < lineCount() ? lines_[static_cast<size_t>(i)] : empty;
    }

    TextPos TextEditor::endPos() const noexcept
    {
        const int last = lineCount() - 1;
        return { last, static_cast<int>(lines_[static_cast<size_t>(last)].size()) };
    }

    TextPos TextEditor::clampPos(TextPos p) const noexcept
    {
        p.line = clampInt(p.line, 0, lineCount() - 1);
        p.col = clampInt(p.col, 0, static_cast<int>(lines_[static_cast<size_t>(p.line)].size()));
        return p;
    }

    TextPos TextEditor::endOf(TextPos at, std::string_view inserted) const noexcept
    {
        const size_t lastNl = inserted.rfind('\n');
        if (lastNl == std::string_view::npos)
            return { at.line, at.col + static_cast<int>(inserted.size()) };
        const int breaks = static_cast<int>(std::count(inserted.begin(), inserted.end(), '\n'));
        return { at.line + breaks, static_cast<int>(inserted.size() - lastNl - 1) };
    }

    std::string TextEditor::rangeText(TextPos a, TextPos b) const
    {
        a = clampPos(a);
        b = clampPos(b);
        if (b < a)
            std::swap(a, b);
        const std::string& first = lines_[static_cast<size_t>(a.line)];
        if (a.line == b.line)
            return first.substr(static_cast<size_t>(a.col), static_cast<size_t>(b.col - a.col));
        std::string out = first.substr(static_cast<size_t>(a.col));
        for (int ln = a.line + 1; ln < b.line; ++ln)
        {
            out += '\n';
            out += lines_[static_cast<size_t>(ln)];
        }
        out += '\n';
        out.append(lines_[static_cast<size_t>(b.line)], 0, static_cast<size_t>(b.col));
        return out;
    }

    TextPos TextEditor::change(TextPos a, TextPos b, std::string_view txt, Kind kind)
    {
        a = clampPos(a);
        b = clampPos(b);
        if (b < a)
            std::swap(a, b);
        std::string removed = rangeText(a, b);
        if (removed.empty() && txt.empty())
            return a;
        const TextPos caretBefore = caret_, anchorBefore = anchor_;
        const TextPos end = spliceText(a, b, txt);
        if (!recording_)
            return end;

        if (!undo_.empty() && undo_.back().pieces.size() == 1 && undo_.back().kind == kind)
        {
            Piece& last = undo_.back().pieces.front();
            // A step never holds a line break with anything else: one it typed or removed stands alone.
            const bool oneLineIn = txt.find('\n') == std::string_view::npos
                                && last.inserted.find('\n') == std::string::npos;
            const bool oneLineOut = removed.find('\n') == std::string::npos
                                 && last.removed.find('\n') == std::string::npos;
            bool merged = false;
            if (kind == Kind::typing && removed.empty() && oneLineIn && endOf(last.from, last.inserted) == a)
            {
                last.inserted.append(txt);
                merged = true;
            }
            else if (kind == Kind::backspace && txt.empty() && last.inserted.empty() && oneLineOut && b == last.from)
            {
                last.removed.insert(0, removed);
                last.from = a;
                merged = true;
            }
            else if (kind == Kind::del && txt.empty() && last.inserted.empty() && oneLineOut && a == last.from)
            {
                last.removed.append(removed);
                merged = true;
            }
            if (merged)
            {
                redo_.clear();
                return end;
            }
        }
        Edit e;
        e.pieces.push_back(Piece{ a, std::move(removed), std::string(txt) });
        e.caretBefore = caretBefore;
        e.anchorBefore = anchorBefore;
        e.kind = kind;
        pushStep(std::move(e));
        return end;
    }

    void TextEditor::pushStep(Edit&& e)
    {
        if (!recording_)
            return;
        undo_.push_back(std::move(e));
        if (static_cast<int>(undo_.size()) > options_.maxUndo)
            undo_.erase(undo_.begin(),
                        undo_.begin() + static_cast<std::ptrdiff_t>(static_cast<int>(undo_.size()) - options_.maxUndo));
        redo_.clear();
    }

    TextPos TextEditor::spliceText(TextPos a, TextPos b, std::string_view txt)
    {
        const float gutterBefore = gutterWidth();

        // The lines a..b become the lines of head + text + tail.
        std::string middle = lines_[static_cast<size_t>(a.line)].substr(0, static_cast<size_t>(a.col));
        middle.append(txt);
        middle.append(lines_[static_cast<size_t>(b.line)], static_cast<size_t>(b.col), std::string::npos);
        std::vector<std::string> fresh = splitLines(middle);
        const int oldCount = b.line - a.line + 1, newCount = static_cast<int>(fresh.size());
        const uint32_t startState = lineState_[static_cast<size_t>(a.line)];   // set by the lines before: kept
        if (newCount != oldCount)
        {
            splice(lines_, a.line, oldCount, newCount, std::string());
            splice(breaks_, a.line, oldCount, newCount, std::vector<int>());
            splice(lineW_, a.line, oldCount, newCount, 0.0f);
            splice(lineState_, a.line, oldCount, newCount, 0u);
        }
        for (int i = 0; i < newCount; ++i)
            lines_[static_cast<size_t>(a.line + i)] = std::move(fresh[static_cast<size_t>(i)]);
        lineState_[static_cast<size_t>(a.line)] = startState;
        const int lastChanged = a.line + newCount - 1;

        if (!ease::sameBits(gutterWidth(), gutterBefore))
            relayoutAll();                               // the gutter grew or shrank: every row's width moved
        else
        {
            for (int i = a.line; i <= lastChanged; ++i)
                relayoutLine(i);
            reindexRows();
        }
        rehighlightFrom(a.line, lastChanged);
        spansLine_ = -1;
        ++revision_;
        modified_ = true;

        const TextPos end = endOf(a, txt);
        mapThrough(a, b, end);
        return end;
    }

    void TextEditor::rehighlightFrom(int firstLine, int lastChanged)
    {
        if (!highlighter_)
            return;
        const int n = lineCount();
        uint32_t st = firstLine <= 0 ? 0u : lineState_[static_cast<size_t>(firstLine)];
        for (int i = std::max(firstLine, 0); i < n; ++i)
        {
            lineState_[static_cast<size_t>(i)] = st;
            spans_.clear();
            st = highlighter_->line(lines_[static_cast<size_t>(i)], st, spans_);
            if (i + 1 < n && i >= lastChanged && lineState_[static_cast<size_t>(i + 1)] == st)
                break;                                   // the lines after start as they did: nothing moves
        }
        spansLine_ = -1;
    }

    void TextEditor::tokenise(int ln) const
    {
        if (spansLine_ == ln && spansRev_ == revision_)
            return;
        spans_.clear();
        if (highlighter_)
            highlighter_->line(lines_[static_cast<size_t>(ln)], lineState_[static_cast<size_t>(ln)], spans_);
        spansLine_ = ln;
        spansRev_ = revision_;
    }

    uint16_t TextEditor::styleIn(const std::vector<text::Span>& spans, int col) const noexcept
    {
        const auto it = std::upper_bound(spans.begin(), spans.end(), col,
                                         [](int c, const text::Span& sp) noexcept { return c < sp.end; });
        return it != spans.end() && it->start <= col ? it->style : uint16_t{ 0 };
    }

    // ---- decorations ------------------------------------------------------------------------------------------------

    void TextEditor::mapThrough(TextPos a, TextPos b, TextPos end)
    {
        size_t keep = 0;
        for (size_t i = 0; i < decorations_.size(); ++i)
        {
            const Decoration d = decorations_[i];
            const bool point = d.from == d.to;
            const TextPos f = mapPoint(d.from, a, b, end);
            const TextPos t = point ? f : mapEnd(d.to, a, b, end);
            if (!point && !(f < t))
                continue;                                // its text is all gone
            decorations_[keep++] = { f, t, d.kind };     // mapping keeps the order by `from`
        }
        decorations_.resize(keep);
        for (LineMark& m : lineMarks_)
            m.line = mapPoint({ m.line, 0 }, a, b, end).line;
        keep = 0;
        for (size_t i = 0; i < autoClosers_.size(); ++i)
        {
            const TextPos p = autoClosers_[i];
            if (p < a || !(p < b))                       // a closer inside the span is gone
                autoClosers_[keep++] = mapPoint(p, a, b, end);
        }
        autoClosers_.resize(keep);
    }

    void TextEditor::setDecorations(std::vector<Decoration> list, uint32_t kinds)
    {
        const auto inMask = [kinds](DecorationKind k) noexcept {
            return static_cast<unsigned>(k) <= static_cast<unsigned>(DecorationKind::bracket)
                && (kinds & decorationBit(k)) != 0;
        };
        decorations_.erase(std::remove_if(decorations_.begin(), decorations_.end(),
                                          [&](const Decoration& d) { return inMask(d.kind); }),
                           decorations_.end());
        for (Decoration& d : list)
        {
            if (!inMask(d.kind))
                continue;
            d.from = clampPos(d.from);
            d.to = clampPos(d.to);
            if (d.to < d.from)
                std::swap(d.from, d.to);
            decorations_.push_back(d);
        }
        std::stable_sort(decorations_.begin(), decorations_.end(),
                         [](const Decoration& x, const Decoration& y) noexcept { return x.from < y.from; });
    }

    void TextEditor::setLineMarks(std::vector<LineMark> marks)
    {
        lineMarks_.clear();
        for (const LineMark& m : marks)
            if (m.line >= 0 && m.line < lineCount() && severity(m.kind) <= severity(LineMark::Kind::error))
                lineMarks_.push_back(m);
        std::stable_sort(lineMarks_.begin(), lineMarks_.end(),
                         [](const LineMark& x, const LineMark& y) noexcept { return x.line < y.line; });
    }

    const LineMark* TextEditor::markOn(int ln) const noexcept
    {
        auto it = std::lower_bound(lineMarks_.begin(), lineMarks_.end(), ln,
                                   [](const LineMark& m, int l) noexcept { return m.line < l; });
        const LineMark* best = nullptr;
        for (; it != lineMarks_.end() && it->line == ln; ++it)
            if (best == nullptr || severity(it->kind) > severity(best->kind))
                best = &*it;
        return best;
    }

    // From the bracket at `at` (excluded) towards its partner: `open` and `close` nest, and with a style (>= 0, and a
    // highlighter) only brackets of that style count.
    bool TextEditor::scanBracket(TextPos at, char open, char close, bool forward, int style, TextPos& out) const
    {
        const bool styled = style >= 0 && highlighter_ != nullptr;
        const int n = lineCount(), step = forward ? 1 : -1;
        int depth = 0, budget = kBracketScan;
        for (int ln = at.line; ln >= 0 && ln < n; ln += step)
        {
            const std::string& s = lines_[static_cast<size_t>(ln)];
            const int len = static_cast<int>(s.size());
            if (styled)
            {
                scanSpans_.clear();
                highlighter_->line(s, lineState_[static_cast<size_t>(ln)], scanSpans_);
            }
            int k = ln == at.line ? at.col + step : (forward ? 0 : len - 1);
            for (; k >= 0 && k < len; k += step)
            {
                if (--budget < 0)
                    return false;
                const char c = s[static_cast<size_t>(k)];
                if (c != open && c != close)
                    continue;
                if (styled && styleIn(scanSpans_, k) != style)
                    continue;
                if (c == (forward ? open : close))
                    ++depth;
                else if (depth == 0)
                {
                    out = { ln, k };
                    return true;
                }
                else
                    --depth;
            }
        }
        return false;
    }

    bool TextEditor::bracketPair(TextPos at, TextPos& open, TextPos& close) const
    {
        at = clampPos(at);
        const std::string& s = lines_[static_cast<size_t>(at.line)];
        const int len = static_cast<int>(s.size());
        for (const int k : { at.col - 1, at.col })      // the character before the caret, then the one after it
        {
            if (k < 0 || k >= len)
                continue;
            const char c = s[static_cast<size_t>(k)];
            if (!isOpenBracket(c) && !isCloseBracket(c))
                continue;
            int style = -1;
            if (highlighter_ != nullptr)
            {
                tokenise(at.line);
                style = styleIn(spans_, k);
            }
            TextPos m;
            if (isOpenBracket(c) && scanBracket({ at.line, k }, c, closerOf(c), true, style, m))
            {
                open = { at.line, k };
                close = m;
                return true;
            }
            if (isCloseBracket(c) && scanBracket({ at.line, k }, openerOf(c), c, false, style, m))
            {
                open = m;
                close = { at.line, k };
                return true;
            }
        }
        return false;
    }

    int TextEditor::indentLevelsOf(int ln) const noexcept
    {
        const std::string& s = lines_[static_cast<size_t>(ln)];
        size_t k = 0;
        while (k < s.size() && isBlank(s[k]))
            ++k;
        if (k == s.size())
            return -1;
        if (!(tabW_ > 0.0f))
            return 0;
        return static_cast<int>(std::floor(xOf(s, 0, static_cast<int>(k)) / tabW_ + 1.0e-4f));
    }

    int TextEditor::guideLevels(int ln) const
    {
        const int own = indentLevelsOf(ln);
        if (own >= 0)
            return own;
        int above = -1, below = -1;
        for (int i = ln - 1; i >= 0 && i >= ln - kGuideScan && above < 0; --i)
            above = indentLevelsOf(i);
        for (int i = ln + 1; i < lineCount() && i <= ln + kGuideScan && below < 0; ++i)
            below = indentLevelsOf(i);
        if (above < 0 || below < 0)
            return 0;
        return above == below ? above : std::min(above, below) + 1;
    }

    std::string TextEditor::indentUnit() const
    {
        return options_.softTabs ? std::string(static_cast<size_t>(options_.tabWidth), ' ') : std::string("\t");
    }

    // ---- the caret and the selection --------------------------------------------------------------------------------

    TextPos TextEditor::selectionStart() const noexcept { return caret_ < anchor_ ? caret_ : anchor_; }
    TextPos TextEditor::selectionEnd() const noexcept { return caret_ < anchor_ ? anchor_ : caret_; }
    std::string TextEditor::selectedText() const { return rangeText(selectionStart(), selectionEnd()); }

    void TextEditor::moveCaret(TextPos p, bool extend, bool keepGoal, bool up)
    {
        caret_ = clampPos(p);
        if (!extend)
            anchor_ = caret_;
        if (!keepGoal)
            goalX_ = -1.0f;
        caretUp_ = up;
        blinkT_ = 0.0f;
        if (!autoClosers_.empty())                       // a closer stays steppable while the caret is on its line,
        {                                                // before it
            const TextPos c = caret_;
            autoClosers_.erase(std::remove_if(autoClosers_.begin(), autoClosers_.end(),
                                              [c](TextPos q) noexcept { return q.line != c.line || q.col < c.col; }),
                               autoClosers_.end());
        }
        scrollToCaret();
    }

    void TextEditor::setCaret(TextPos p, bool extend) { moveCaret(p, extend); }

    void TextEditor::select(TextPos a, TextPos c)
    {
        anchor_ = clampPos(a);
        moveCaret(c, true);
    }

    void TextEditor::selectAll() { select({}, endPos()); }

    bool TextEditor::find(std::string_view needle, bool forward, bool caseSensitive, bool wrapAround)
    {
        if (needle.empty() || needle.find('\n') != std::string_view::npos)
            return false;
        const int n = lineCount(), m = static_cast<int>(needle.size());
        if (forward)
        {
            const TextPos start = selectionEnd();
            for (int step = 0; step <= n; ++step)
            {
                const int ln = (start.line + step) % n;
                const int at = findIn(lines_[static_cast<size_t>(ln)], needle, step == 0 ? start.col : 0,
                                      caseSensitive);
                if (at >= 0)
                {
                    select({ ln, at }, { ln, at + m });
                    return true;
                }
                if (!wrapAround && ln == n - 1)
                    break;
            }
            return false;
        }
        const TextPos start = selectionStart();
        for (int step = 0; step <= n; ++step)
        {
            const int ln = ((start.line - step) % n + n) % n;
            const int at = rfindIn(lines_[static_cast<size_t>(ln)], needle,
                                   step == 0 ? start.col : static_cast<int>(lines_[static_cast<size_t>(ln)].size()),
                                   caseSensitive);
            if (at >= 0)
            {
                select({ ln, at }, { ln, at + m });
                return true;
            }
            if (!wrapAround && ln == 0)
                break;
        }
        return false;
    }

    // ---- movement ---------------------------------------------------------------------------------------------------

    TextPos TextEditor::prevChar(TextPos p) const noexcept
    {
        if (p.col > 0)
            return { p.line, p.col - 1 };
        if (p.line > 0)
            return { p.line - 1, static_cast<int>(lines_[static_cast<size_t>(p.line - 1)].size()) };
        return p;
    }

    TextPos TextEditor::nextChar(TextPos p) const noexcept
    {
        if (p.col < static_cast<int>(lines_[static_cast<size_t>(p.line)].size()))
            return { p.line, p.col + 1 };
        if (p.line < lineCount() - 1)
            return { p.line + 1, 0 };
        return p;
    }

    TextPos TextEditor::prevWord(TextPos p) const noexcept
    {
        if (p.col == 0)
            return prevChar(p);
        const std::string& s = lines_[static_cast<size_t>(p.line)];
        int k = p.col;
        while (k > 0 && isBlank(s[static_cast<size_t>(k - 1)]))
            --k;
        if (k > 0 && isWordChar(s[static_cast<size_t>(k - 1)]))
            while (k > 0 && isWordChar(s[static_cast<size_t>(k - 1)]))
                --k;
        else
            while (k > 0 && !isWordChar(s[static_cast<size_t>(k - 1)]) && !isBlank(s[static_cast<size_t>(k - 1)]))
                --k;
        return { p.line, k };
    }

    TextPos TextEditor::nextWord(TextPos p) const noexcept
    {
        const std::string& s = lines_[static_cast<size_t>(p.line)];
        const int n = static_cast<int>(s.size());
        if (p.col >= n)
            return nextChar(p);
        int k = p.col;
        while (k < n && isBlank(s[static_cast<size_t>(k)]))
            ++k;
        if (k < n && isWordChar(s[static_cast<size_t>(k)]))
            while (k < n && isWordChar(s[static_cast<size_t>(k)]))
                ++k;
        else
            while (k < n && !isWordChar(s[static_cast<size_t>(k)]) && !isBlank(s[static_cast<size_t>(k)]))
                ++k;
        return { p.line, k };
    }

    void TextEditor::wordAt(TextPos p, TextPos& start, TextPos& end) const noexcept
    {
        p = clampPos(p);
        const std::string& s = lines_[static_cast<size_t>(p.line)];
        const int n = static_cast<int>(s.size());
        if (n == 0)
        {
            start = end = p;
            return;
        }
        const int at = std::min(p.col, n - 1);
        const char c = s[static_cast<size_t>(at)];
        const auto same = [c](char d) noexcept {
            return isWordChar(c) ? isWordChar(d) : (isBlank(c) ? isBlank(d) : (!isWordChar(d) && !isBlank(d)));
        };
        int a = at, b = at + 1;
        while (a > 0 && same(s[static_cast<size_t>(a - 1)]))
            --a;
        while (b < n && same(s[static_cast<size_t>(b)]))
            ++b;
        start = { p.line, a };
        end = { p.line, b };
    }

    void TextEditor::lineAt(TextPos p, TextPos& start, TextPos& end) const noexcept
    {
        p = clampPos(p);
        start = { p.line, 0 };
        end = p.line < lineCount() - 1 ? TextPos{ p.line + 1, 0 } : endPos();
    }

    float TextEditor::goalXOfCaret() const noexcept
    {
        int ln, from, to;
        rowRange(rowOfCaret(), ln, from, to);
        return xOf(lines_[static_cast<size_t>(ln)], from, caret_.col);
    }

    TextPos TextEditor::rowMove(int fromRow, int rows, float goalX, bool& up) const noexcept
    {
        const int row = clampInt(fromRow + rows, 0, rowCount() - 1);
        up = false;
        if (row == fromRow)
            return rows < 0 ? TextPos{} : endPos();      // the first or last row: to the document's end
        int ln, from, to;
        rowRange(row, ln, from, to);
        const std::string& s = lines_[static_cast<size_t>(ln)];
        const int col = colAt(s, from, to, goalX);
        up = col == to && to < static_cast<int>(s.size());
        return { ln, col };
    }

    bool TextEditor::commandKeyIsMeta() const noexcept
    {
        if (host_ != nullptr)
            return host_->commandKeyIsMeta();
       #if defined(__APPLE__)
        return true;
       #else
        return false;
       #endif
    }

    // ---- editing ----------------------------------------------------------------------------------------------------

    bool TextEditor::typeText(std::string_view txt, Kind kind)
    {
        const TextPos a = selectionStart(), b = selectionEnd();
        if (txt.empty() && a == b)
            return false;
        const TextPos end = change(a, b, txt, kind);
        moveCaret(end, false);
        return true;
    }

    bool TextEditor::insert(std::string_view utf8)
    {
        if (!editable())
            return false;
        return typeText(sanitise(utf8), Kind::other);
    }

    bool TextEditor::replaceRange(TextPos from, TextPos to, std::string_view utf8)
    {
        if (!editable())
            return false;
        from = clampPos(from);
        to = clampPos(to);
        if (to < from)
            std::swap(from, to);
        const std::string txt = sanitise(utf8);
        if (rangeText(from, to) == txt)
            return false;                                // nothing would change
        const TextPos end = change(from, to, txt, Kind::other);
        moveCaret(end, false);
        return true;
    }

    bool TextEditor::applyEdits(std::vector<TextEdit> edits)
    {
        if (!editable())
            return false;
        for (TextEdit& e : edits)
        {
            e.from = clampPos(e.from);
            e.to = clampPos(e.to);
            if (e.to < e.from)
                std::swap(e.from, e.to);
            e.text = sanitise(e.text);
        }
        std::stable_sort(edits.begin(), edits.end(), [](const TextEdit& x, const TextEdit& y) noexcept {
            return x.from < y.from || (x.from == y.from && x.to < y.to);
        });
        for (size_t i = 0; i + 1 < edits.size(); ++i)
            if (edits[i + 1].from < edits[i].to)
                return false;                            // two overlap: refused whole
        edits.erase(std::remove_if(edits.begin(), edits.end(),
                                   [this](const TextEdit& e) { return rangeText(e.from, e.to) == e.text; }),
                    edits.end());
        if (edits.empty())
            return false;

        // From the last edit to the first, so each one's positions are still those of the text as it stood; the
        // caret and the anchor move through every one.
        Edit step;
        step.caretBefore = caret_;
        step.anchorBefore = anchor_;
        step.kind = Kind::other;
        TextPos c = caret_, an = anchor_;
        for (size_t i = edits.size(); i-- > 0;)
        {
            const TextEdit& e = edits[i];
            std::string removed = rangeText(e.from, e.to);
            const TextPos end = spliceText(e.from, e.to, e.text);
            c = mapPoint(c, e.from, e.to, end);
            an = mapPoint(an, e.from, e.to, end);
            step.pieces.push_back(Piece{ e.from, std::move(removed), e.text });
        }
        pushStep(std::move(step));
        anchor_ = clampPos(an);
        moveCaret(c, true);
        return true;
    }

    bool TextEditor::replaceText(std::string_view utf8)
    {
        if (!editable())
            return false;
        const std::string next = sanitise(utf8), cur = text();
        if (next == cur)
            return false;
        const size_t n = std::min(next.size(), cur.size());
        size_t pre = 0, suf = 0;
        while (pre < n && cur[pre] == next[pre])
            ++pre;
        while (suf < n - pre && cur[cur.size() - 1 - suf] == next[next.size() - 1 - suf])
            ++suf;
        const TextPos caretWas = caret_, anchorWas = anchor_;
        change(posOfOffset(cur, pre), posOfOffset(cur, cur.size() - suf),
               std::string_view(next).substr(pre, next.size() - pre - suf), Kind::other);
        anchor_ = clampPos(anchorWas);
        moveCaret(caretWas, true);
        return true;
    }

    bool TextEditor::deleteSelection()
    {
        if (!editable() || !hasSelection())
            return false;
        return typeText({}, Kind::other);
    }

    bool TextEditor::undo()
    {
        if (!editable() || undo_.empty())
            return false;
        Edit e = std::move(undo_.back());
        undo_.pop_back();
        e.caretAfter = caret_;
        e.anchorAfter = anchor_;
        recording_ = false;
        for (auto p = e.pieces.rbegin(); p != e.pieces.rend(); ++p)
            change(p->from, endOf(p->from, p->inserted), p->removed, Kind::other);
        recording_ = true;
        anchor_ = clampPos(e.anchorBefore);
        moveCaret(e.caretBefore, true);
        redo_.push_back(std::move(e));
        return true;
    }

    bool TextEditor::redo()
    {
        if (!editable() || redo_.empty())
            return false;
        Edit e = std::move(redo_.back());
        redo_.pop_back();
        recording_ = false;
        for (const Piece& p : e.pieces)
            change(p.from, endOf(p.from, p.removed), p.inserted, Kind::other);
        recording_ = true;
        anchor_ = clampPos(e.anchorAfter);
        moveCaret(e.caretAfter, true);
        undo_.push_back(std::move(e));
        return true;
    }

    bool TextEditor::copy()
    {
        if (!hasSelection())
            return false;
        clipboard_ = selectedText();
        if (host_ != nullptr)
            host_->copyText(clipboard_);
        return true;
    }

    bool TextEditor::cut()
    {
        if (!editable() || !hasSelection())
            return false;
        copy();
        return deleteSelection();
    }

    bool TextEditor::paste()
    {
        if (!editable())
            return false;
        std::string t = host_ != nullptr ? host_->clipboardText() : std::string();
        if (t.empty())
            t = clipboard_;                              // a host that reads empty (a browser): what was copied here
        if (t.empty())
            return false;
        return insert(t);
    }

    bool TextEditor::eraseBackward(bool word, bool toRowStart)
    {
        if (hasSelection())
            return deleteSelection();
        TextPos from;
        if (toRowStart)
        {
            int rs, re;
            caretRowRange(rs, re);
            from = { caret_.line, rs };
            if (from == caret_)
                from = prevChar(caret_);
        }
        else if (word)
            from = prevWord(caret_);
        else
        {
            from = prevChar(caret_);
            const std::string& s = lines_[static_cast<size_t>(caret_.line)];
            if (options_.softTabs && caret_.col > 0
                && s.find_first_not_of(' ') >= static_cast<size_t>(caret_.col))
                from.col = ((caret_.col - 1) / options_.tabWidth) * options_.tabWidth;   // to the previous stop
            else if (options_.autoClosePairs && caret_.col > 0 && caret_.col < static_cast<int>(s.size()))
            {
                const char before = s[static_cast<size_t>(caret_.col - 1)];
                if (closerOf(before) != 0 && s[static_cast<size_t>(caret_.col)] == closerOf(before))
                {
                    change({ caret_.line, caret_.col - 1 }, { caret_.line, caret_.col + 1 }, {}, Kind::backspace);
                    moveCaret({ caret_.line, caret_.col - 1 }, false);   // an empty pair: both go
                    return true;
                }
            }
        }
        if (from == caret_)
            return true;                                 // at the start: an edit owns the key (LineEdit's rule)
        change(from, caret_, {}, word || toRowStart ? Kind::other : Kind::backspace);
        moveCaret(from, false);
        return true;
    }

    bool TextEditor::eraseForward(bool word, bool toRowEnd)
    {
        if (hasSelection())
            return deleteSelection();
        TextPos to;
        if (toRowEnd)
        {
            int rs, re;
            caretRowRange(rs, re);
            to = { caret_.line, re };
            if (to == caret_)
                to = nextChar(caret_);
        }
        else
            to = word ? nextWord(caret_) : nextChar(caret_);
        if (to == caret_)
            return true;
        change(caret_, to, {}, word || toRowEnd ? Kind::other : Kind::del);
        moveCaret(caret_, false);
        return true;
    }

    bool TextEditor::typeReturn()
    {
        const TextPos a = selectionStart(), b = selectionEnd();   // the break lands where the selection starts
        const std::string& sa = lines_[static_cast<size_t>(a.line)];
        size_t lead = 0;                                 // the line's own indent: its leading blanks before the caret
        while (lead < static_cast<size_t>(a.col) && lead < sa.size() && isBlank(sa[lead]))
            ++lead;
        std::string indent;
        if (options_.autoIndent)
            indent.assign(sa, 0, lead);
        if (options_.braceIndent)
        {
            int i = a.col;
            while (i > 0 && isBlank(sa[static_cast<size_t>(i - 1)]))
                --i;
            const char open = i > 0 ? sa[static_cast<size_t>(i - 1)] : '\0';
            if (isOpenBracket(open))
            {
                const std::string& sb = lines_[static_cast<size_t>(b.line)];
                int j = b.col;
                while (j < static_cast<int>(sb.size()) && isBlank(sb[static_cast<size_t>(j)]))
                    ++j;
                const bool closerNext = j < static_cast<int>(sb.size()) && sb[static_cast<size_t>(j)] == closerOf(open);
                const std::string own(sa, 0, lead);      // from the line's own indent, with or without autoIndent
                const std::string unit = indentUnit();
                std::string ins = "\n" + own + unit;
                if (closerNext)
                    ins += "\n" + own;
                const TextPos from{ a.line, i }, to = closerNext ? TextPos{ b.line, j } : b;
                change(from, to, ins, Kind::other);
                moveCaret({ a.line + 1, static_cast<int>(own.size() + unit.size()) }, false);
                return true;
            }
        }
        return typeText("\n" + indent, Kind::other);
    }

    bool TextEditor::typeChar(char c)
    {
        if (options_.autoClosePairs)
        {
            const std::string& s = lines_[static_cast<size_t>(caret_.line)];
            const int col = caret_.col, len = static_cast<int>(s.size());
            // A closer typed just before one the editor inserted steps over it.
            if (!hasSelection() && (isCloseBracket(c) || isQuote(c)) && col < len && s[static_cast<size_t>(col)] == c)
            {
                const auto it = std::find(autoClosers_.begin(), autoClosers_.end(), caret_);
                if (it != autoClosers_.end())
                {
                    autoClosers_.erase(it);
                    moveCaret({ caret_.line, col + 1 }, false);
                    return true;
                }
            }
            const char closer = closerOf(c);
            if (closer != 0 && hasSelection())
            {
                // Over a selection an opener or a quote wraps it; the selection stays on the text inside. Two
                // insertions (one undo step), not a replacement of the selection, so the decorations, line marks and
                // auto-closers inside it stay on its text.
                const TextPos a = selectionStart(), b = selectionEnd();
                const std::string inner = rangeText(a, b);
                applyEdits({ { a, a, std::string(1, c) }, { b, b, std::string(1, closer) } });
                const TextPos innerFrom{ a.line, a.col + 1 };
                anchor_ = innerFrom;
                moveCaret(endOf(innerFrom, inner), true);
                return true;
            }
            if (closer != 0)
            {
                const char next = col < len ? s[static_cast<size_t>(col)] : '\0';
                const char prev = col > 0 ? s[static_cast<size_t>(col - 1)] : '\0';
                bool close = next == '\0' || closesBefore(next);
                if (isQuote(c))
                    close = close && !isWordChar(prev) && prev != c;
                if (close)
                {
                    const char pair[2] = { c, closer };
                    change(caret_, caret_, std::string_view(pair, 2), Kind::typing);
                    moveCaret({ caret_.line, col + 1 }, false);
                    autoClosers_.push_back({ caret_.line, col + 1 });
                    return true;
                }
            }
        }
        if (options_.braceIndent && isCloseBracket(c) && !hasSelection() && outdentCloser(c))
            return true;
        return typeText(std::string_view(&c, 1), Kind::typing);
    }

    bool TextEditor::outdentCloser(char c)
    {
        const std::string& s = lines_[static_cast<size_t>(caret_.line)];
        const int col = caret_.col;
        if (col == 0 || s.find_first_not_of(" \t") < static_cast<size_t>(col))
            return false;                                // not blank before the caret
        std::string target;
        TextPos open;
        if (scanBracket(caret_, openerOf(c), c, false, -1, open))
        {
            const std::string& o = lines_[static_cast<size_t>(open.line)];
            target.assign(o, 0, std::min(o.find_first_not_of(" \t"), o.size()));
        }
        else
        {
            target.assign(s, 0, static_cast<size_t>(col));
            if (target.back() == '\t')
                target.pop_back();
            else
            {
                size_t k = target.size();                // spaces back to the previous stop
                const size_t stop = ((k - 1) / static_cast<size_t>(options_.tabWidth))
                                  * static_cast<size_t>(options_.tabWidth);
                while (k > stop && target[k - 1] == ' ')
                    --k;
                target.resize(k);
            }
        }
        if (!(xOf(target, 0, static_cast<int>(target.size())) < xOf(s, 0, col)))
            return false;                                // not shallower: typed as it is
        change({ caret_.line, 0 }, caret_, target + c, Kind::other);
        moveCaret({ caret_.line, static_cast<int>(target.size()) + 1 }, false);
        return true;
    }

    bool TextEditor::typeTab()
    {
        if (!options_.softTabs)
            return typeText("\t", Kind::typing);
        const int col = selectionStart().col;
        const int spaces = options_.tabWidth - col % options_.tabWidth;
        return typeText(std::string(static_cast<size_t>(spaces), ' '), Kind::typing);
    }

    bool TextEditor::indentLines(bool outdent)
    {
        const TextPos a = selectionStart(), b = selectionEnd();
        const int l0 = a.line;
        const int l1 = (b.col == 0 && b.line > l0) ? b.line - 1 : b.line;   // a selection ending at a line's start
        const std::string unit = indentUnit();
        // A piece per line (one undo step), not a replacement of the block, so the caret, the anchor, the decorations,
        // the line marks and the auto-closers move with each line's own text: an insertion at a line's start shifts
        // what follows it; inside removed leading blanks a position goes to the line's start. applyEdits() is false
        // with no change when no line changes.
        std::vector<TextEdit> edits;
        for (int ln = l0; ln <= l1; ++ln)
        {
            const std::string& s = lines_[static_cast<size_t>(ln)];
            if (!outdent)
            {
                if (!s.empty() || l0 == l1)
                    edits.push_back({ { ln, 0 }, { ln, 0 }, unit });
                continue;
            }
            int k = 0;
            if (!s.empty() && s[0] == '\t')
                k = 1;
            else
                while (k < static_cast<int>(s.size()) && k < options_.tabWidth && s[static_cast<size_t>(k)] == ' ')
                    ++k;
            if (k > 0)
                edits.push_back({ { ln, 0 }, { ln, k }, {} });
        }
        return applyEdits(std::move(edits));
    }

    // ---- focus and the frame ----------------------------------------------------------------------------------------

    void TextEditor::setFocused(bool f)
    {
        focused_ = f;
        blinkT_ = 0.0f;
        if (!f)
            drag_ = Drag::none;
    }

    void TextEditor::tick(float dt)
    {
        if (!focused_ || !options_.blinkCaret || !(options_.blinkPeriod > 0.0f) || !(dt > 0.0f) || !std::isfinite(dt))
            return;
        blinkT_ = std::fmod(blinkT_ + dt, options_.blinkPeriod);   // one call, however long the host slept
    }

    // ---- input ------------------------------------------------------------------------------------------------------

    void TextEditor::pointerDown(const PointerEvent& e)
    {
        const Point p{ e.x, e.y };
        drag_ = Drag::none;
        if (!bounds_.contains(p) || e.popup)
            return;
        focused_ = true;
        blinkT_ = 0.0f;

        float ty = 0.0f, th = 0.0f;
        if (options_.minimap && p.x >= rightEdge())
        {
            // The minimap: a press on the slider drags it; beside it, the slider centres there and drags on.
            float off = 0.0f;
            minimapGeometry(off, ty, th);
            drag_ = Drag::minimap;
            dragOffset_ = p.y >= ty && p.y < ty + th ? p.y - ty : th * 0.5f;
            pointerDrag(e);
            return;
        }
        if (vbarShown())
        {
            const Rect r = vbarRect(ty, th);
            if (r.expanded(2.0f).contains(p))
            {
                drag_ = Drag::vbar;
                if (p.y >= ty && p.y < ty + th)
                    dragOffset_ = p.y - ty;
                else
                {
                    dragOffset_ = th * 0.5f;             // a press on the track: the thumb jumps under the pointer
                    const float track = r.h - th;
                    scrollY_ = track > 0.0f ? (p.y - r.y - dragOffset_) / track * maxScrollY() : 0.0f;
                    clampScroll();
                }
                return;
            }
        }
        if (hbarShown())
        {
            const Rect r = hbarRect(ty, th);
            if (r.expanded(2.0f).contains(p))
            {
                drag_ = Drag::hbar;
                if (p.x >= ty && p.x < ty + th)
                    dragOffset_ = p.x - ty;
                else
                {
                    dragOffset_ = th * 0.5f;
                    const float track = r.w - th;
                    scrollX_ = track > 0.0f ? (p.x - r.x - dragOffset_) / track * maxScrollX() : 0.0f;
                    clampScroll();
                }
                return;
            }
        }

        const Rect ta = textArea();
        if (gutterWidth() > 0.0f && p.x < ta.x - options_.padding * 0.5f)
        {
            TextPos s, en;
            lineAt(positionAt(p), s, en);
            select(s, en);
            drag_ = Drag::lines;
            dragStart_ = s;
            dragEnd_ = en;
            return;
        }
        bool up = false;
        const TextPos pos = hitTest(p, up);
        if (e.clicks >= 3)
        {
            TextPos s, en;
            lineAt(pos, s, en);
            select(s, en);
            drag_ = Drag::lines;
            dragStart_ = s;
            dragEnd_ = en;
            return;
        }
        if (e.clicks == 2)
        {
            TextPos s, en;
            wordAt(pos, s, en);
            select(s, en);
            drag_ = Drag::words;
            dragStart_ = s;
            dragEnd_ = en;
            return;
        }
        moveCaret(pos, e.mods.shift, false, up);
        drag_ = Drag::chars;
    }

    void TextEditor::pointerDrag(const PointerEvent& e)
    {
        const Point p{ e.x, e.y };
        switch (drag_)
        {
            case Drag::none:
                return;
            case Drag::chars:
            {
                bool up = false;
                const TextPos pos = hitTest(p, up);
                moveCaret(pos, true, false, up);
                return;
            }
            case Drag::words:
            case Drag::lines:
            {
                const TextPos pos = positionAt(p);
                TextPos s, en;
                if (drag_ == Drag::words)
                    wordAt(pos, s, en);
                else
                    lineAt(pos, s, en);
                if (pos < dragStart_)
                    select(dragEnd_, s);
                else
                    select(dragStart_, en);
                return;
            }
            case Drag::vbar:
            {
                float ty = 0.0f, th = 0.0f;
                const Rect r = vbarRect(ty, th);
                const float track = r.h - th;
                scrollY_ = track > 0.0f ? (p.y - dragOffset_ - r.y) / track * maxScrollY() : 0.0f;
                clampScroll();
                return;
            }
            case Drag::hbar:
            {
                float tx = 0.0f, tw = 0.0f;
                const Rect r = hbarRect(tx, tw);
                const float track = r.w - tw;
                scrollX_ = track > 0.0f ? (p.x - dragOffset_ - r.x) / track * maxScrollX() : 0.0f;
                clampScroll();
                return;
            }
            case Drag::minimap:
            {
                float off = 0.0f, sy = 0.0f, sh = 0.0f;
                minimapGeometry(off, sy, sh);
                const Rect m = minimapArea();
                const float track = std::min(m.h, static_cast<float>(rowCount()) * kMapRowH) - sh;
                scrollY_ = track > 0.0f ? (p.y - dragOffset_ - m.y) / track * maxScrollY() : 0.0f;
                clampScroll();
                return;
            }
        }
    }

    void TextEditor::pointerUp(const PointerEvent&) { drag_ = Drag::none; }

    void TextEditor::doubleClick(const PointerEvent& e)
    {
        const Point p{ e.x, e.y };
        if (!textArea().contains(p) || e.popup)
            return;
        TextPos s, en;
        wordAt(positionAt(p), s, en);
        select(s, en);
        drag_ = Drag::words;
        dragStart_ = s;
        dragEnd_ = en;
    }

    bool TextEditor::wheel(const WheelEvent& e)
    {
        if (!bounds_.contains({ e.x, e.y }))
            return false;
        const bool canScroll = vbarShown() || hbarShown();
        if (!canScroll)
            return false;
        const float px = e.smooth ? kWheelPxPerUnit : kRowsPerNotch * rowH_ / kWheelNotch;
        scrollY_ -= e.dy * px;
        scrollX_ -= e.dx * px;
        clampScroll();
        return true;                                     // an edge bump stays here: the parent never scrolls under it
    }

    bool TextEditor::key(const KeyEvent& e)
    {
        if (!focused_)
            return false;
        const bool meta = commandKeyIsMeta();
        const bool cmd = e.mods.cmd, shift = e.mods.shift;
        const bool word = meta ? e.mods.alt : (e.mods.cmd || e.mods.alt);   // Alt on macOS, Ctrl (or Alt) elsewhere
        const bool rowExt = meta && cmd;                 // Cmd-Left/Right, Cmd-Backspace/Delete on macOS

        switch (e.key)
        {
            case Key::left:
            case Key::right:
            {
                const bool left = e.key == Key::left;
                if (!shift && !cmd && !e.mods.alt && hasSelection())
                {
                    moveCaret(left ? selectionStart() : selectionEnd(), false);
                    return true;
                }
                TextPos t;
                if (rowExt)
                {
                    int rs, re;
                    caretRowRange(rs, re);
                    t = { caret_.line, left ? rs : re };
                }
                else if (word)
                    t = left ? prevWord(caret_) : nextWord(caret_);
                else
                    t = left ? prevChar(caret_) : nextChar(caret_);
                const bool up = rowExt && !left && t.col < static_cast<int>(lines_[static_cast<size_t>(t.line)].size());
                moveCaret(t, shift, false, up);
                return true;
            }
            case Key::up:
            case Key::down:
            {
                const bool upKey = e.key == Key::up;
                if (cmd)
                {
                    moveCaret(upKey ? TextPos{} : endPos(), shift);
                    return true;
                }
                const float gx = goalX_ >= 0.0f ? goalX_ : goalXOfCaret();
                bool up = false;
                const TextPos t = rowMove(rowOfCaret(), upKey ? -1 : 1, gx, up);
                moveCaret(t, shift, true, up);
                goalX_ = gx;
                return true;
            }
            case Key::home:
            {
                if (cmd)
                {
                    moveCaret({}, shift);
                    return true;
                }
                int rs, re;
                caretRowRange(rs, re);
                const std::string& s = lines_[static_cast<size_t>(caret_.line)];
                int k = rs;
                while (k < re && isBlank(s[static_cast<size_t>(k)]))
                    ++k;
                const int target = (k == re || caret_.col == k) ? rs : k;   // all blank, or already there: column 0
                moveCaret({ caret_.line, target }, shift);
                return true;
            }
            case Key::end:
            {
                if (cmd)
                {
                    moveCaret(endPos(), shift);
                    return true;
                }
                int rs, re;
                caretRowRange(rs, re);
                moveCaret({ caret_.line, re }, shift, false,
                          re < static_cast<int>(lines_[static_cast<size_t>(caret_.line)].size()));
                return true;
            }
            case Key::pageUp:
            case Key::pageDown:
            {
                const int rows = std::max(1, static_cast<int>(std::floor(textArea().h / rowH_)));
                const int dir = e.key == Key::pageUp ? -1 : 1;
                const float gx = goalX_ >= 0.0f ? goalX_ : goalXOfCaret();
                bool up = false;
                const TextPos t = rowMove(rowOfCaret(), dir * rows, gx, up);
                scrollY_ += static_cast<float>(dir * rows) * rowH_;
                clampScroll();
                moveCaret(t, shift, true, up);
                goalX_ = gx;
                return true;
            }
            case Key::backspace:
                return editable() && eraseBackward(word, rowExt);
            case Key::del:
                return editable() && eraseForward(word, rowExt);
            case Key::enter:
                return !cmd && editable() && typeReturn();
            case Key::tab:
                if (cmd || !editable())
                    return false;
                if (shift)
                    return indentLines(true);
                if (hasSelection() && selectionStart().line != selectionEnd().line)
                    return indentLines(false);
                return typeTab();
            case Key::space:
                return !cmd && !e.mods.ctrl && editable() && typeText(" ", Kind::typing);
            case Key::escape:
                return false;
            case Key::character:
                break;
        }

        const char32_t ch = lowerAscii(e.ch);
        if (cmd)
        {
            switch (ch)
            {
                case U'a': selectAll(); return true;
                case U'c': copy(); return true;
                case U'x': return cut();
                case U'v': return paste();
                case U'z': return shift ? redo() : undo();
                case U'y': return redo();
                default:   return false;
            }
        }
        if (e.mods.ctrl || !printableAscii(static_cast<uint32_t>(e.ch)) || !editable())
            return false;
        return typeChar(static_cast<char>(e.ch));
    }

    Cursor TextEditor::cursorAt(Point) const noexcept { return Cursor::normal; }

    // ---- accessibility ----------------------------------------------------------------------------------------------

    void TextEditor::accessibility(A11yItem& it) const
    {
        it.id = a11yId_;
        it.role = a11yrole::editableText;
        it.bounds = bounds_;
        it.title = options_.a11yTitle;
        it.value = text();
        it.readOnly = options_.readOnly;
        char buf[96];
        const int selected = hasSelection() ? static_cast<int>(selectedText().size()) : 0;
        if (selected > 0)
            std::snprintf(buf, sizeof buf, "line %d, column %d, %d selected", caret_.line + 1, caret_.col + 1,
                          selected);
        else
            std::snprintf(buf, sizeof buf, "line %d, column %d", caret_.line + 1, caret_.col + 1);
        it.description = buf;
    }

    // ---- drawing ----------------------------------------------------------------------------------------------------

    namespace
    {
        Col markInk(const DecorationInks& inks, LineMark::Kind k, const Theme& th) noexcept
        {
            switch (k)
            {
                case LineMark::Kind::info:    return inks.info.resolve(th);
                case LineMark::Kind::warning: return inks.warning.resolve(th);
                case LineMark::Kind::error:   break;
            }
            return inks.error.resolve(th);
        }

        bool isBackground(DecorationKind k) noexcept
        {
            return k == DecorationKind::highlight || k == DecorationKind::findMatch || k == DecorationKind::findCurrent;
        }
    }

    void TextEditor::draw(Canvas& c, const Theme& th, bool focusRing) const
    {
        if (bounds_.isEmpty())
            return;
        {
            const Canvas::Scope sc(c, tags::editFrame, false);
            c.rrect(bounds_.x, bounds_.y, bounds_.w, bounds_.h, kRadius, th.ink16.withAlpha(0.0f), kBorder, th.ink16);
        }
        const Rect inner = innerRect(), ta = textArea(), clip = rowsClip();
        const float gw = gutterWidth();
        if (!clip.isEmpty() && fontOk_)
        {
            const float cx0 = c.snapX(clip.x), cy0 = c.snapY(clip.y), cx1 = c.snapX(clip.right());
            const float cy1 = c.snapY(clip.bottom());
            const Canvas::ClipScope cs(c, { cx0, cy0, cx1 - cx0, cy1 - cy0 });
            const int rows = rowCount();
            const int first = clampInt(static_cast<int>(std::floor(scrollY_ / rowH_)), 0, rows - 1);
            const int last = clampInt(static_cast<int>(std::floor((scrollY_ + ta.h) / rowH_)), 0, rows - 1);
            if (gw > 0.0f)
            {
                {
                    const Canvas::Scope sc(c, tags::editFrame, false);
                    c.hairlineV(inner.x + gw - 1.0f, clip.y, clip.h, th.ink16);
                }
                for (int row = first; row <= last; ++row)
                {
                    const float top = ta.y + static_cast<float>(row) * rowH_ - scrollY_;
                    if (options_.markColumn)
                        drawLineMark(c, th, row, top);
                    if (options_.lineNumbers)
                        drawLineNumber(c, th, row, top);
                }
            }

            // The decorations on the lines in view, and the bracket pair at the caret (cached by revision and caret).
            shown_.clear();
            if (!decorations_.empty())
            {
                int l0, l1, f, t;
                rowRange(first, l0, f, t);
                rowRange(last, l1, f, t);
                for (const Decoration& d : decorations_)
                {
                    if (d.from.line > l1)
                        break;                           // by `from`: none after this one is in view
                    if (d.to.line >= l0)
                        shown_.push_back(&d);
                }
            }
            bool pair = false;
            if (options_.matchBrackets && focused_)
            {
                if (!pairValid_ || pairRev_ != revision_ || pairAt_ != caret_ || pairGen_ != styleGen_)
                {
                    pairOk_ = bracketPair(caret_, pairOpen_, pairClose_);
                    pairValid_ = true;
                    pairRev_ = revision_;
                    pairAt_ = caret_;
                    pairGen_ = styleGen_;
                }
                pair = pairOk_;
            }

            // What scrolls sideways (the band, the selection, the text, the caret) is cut again at the gutter's edge,
            // so text scrolled to the left never runs over the line numbers.
            const float tx0 = std::min(c.snapX(inner.x + gw), cx1);
            const Canvas::ClipScope ts(c, { tx0, cy0, cx1 - tx0, cy1 - cy0 });
            for (int row = first; row <= last; ++row)
                drawRow(c, th, row, ta.y + static_cast<float>(row) * rowH_ - scrollY_, pair);
            if (!options_.placeholder.empty() && lines_.size() == 1 && lines_[0].empty())
            {
                const Canvas::Scope sc(c, tags::editText, false);
                c.text(options_.placeholder.c_str(), ta.x, ta.y + textTop_, options_.style, th.ink32);
            }
            const bool caretOn = focused_ && (!options_.blinkCaret || blinkT_ < options_.blinkPeriod * 0.5f);
            if (caretOn)
            {
                const Canvas::Scope sc(c, tags::editCaret, false);
                const Point cp = caretPoint();
                c.rrect(cp.x, cp.y + 1.0f, kCaretW, rowH_ - 2.0f, 0.0f, th.accent);
            }
        }
        if (options_.minimap && fontOk_)
            drawMinimap(c, th);
        {
            const Canvas::Scope sc(c, tags::editFrame, false);
            float t0 = 0.0f, t1 = 0.0f;
            if (vbarShown())
            {
                const Rect r = vbarRect(t0, t1);
                c.rrect(r.x, t0, r.w, t1, kBarW * 0.5f, drag_ == Drag::vbar ? th.ink52 : th.ink32);
            }
            if (hbarShown())
            {
                const Rect r = hbarRect(t0, t1);
                c.rrect(t0, r.y, t1, r.h, kBarW * 0.5f, drag_ == Drag::hbar ? th.ink52 : th.ink32);
            }
        }
        if (focusRing)
            drawFocusRing(c, bounds_, th.accent);
    }

    void TextEditor::drawLineNumber(Canvas& c, const Theme& th, int row, float rowTop) const
    {
        int ln, from, to;
        rowRange(row, ln, from, to);
        if (from != 0)
            return;                                      // a wrapped line is numbered on its first row only
        const Canvas::Scope sc(c, tags::editGutter, false);
        char buf[16];
        std::snprintf(buf, sizeof buf, "%d", ln + 1);
        TextStyle num = options_.style;
        num.tabular = true;
        Col ink = ln == caret_.line ? th.ink70 : th.ink32;
        if (!options_.markColumn)
            if (const LineMark* m = markOn(ln))
                ink = markInk(options_.inks, m->kind, th);   // no column for the marks: the number takes the ink
        c.text(buf, innerRect().x + gutterWidth() - kGutterPad, rowTop + textTop_, num, ink, Align::right);
    }

    void TextEditor::drawLineMark(Canvas& c, const Theme& th, int row, float rowTop) const
    {
        int ln, from, to;
        rowRange(row, ln, from, to);
        const LineMark* m = from == 0 ? markOn(ln) : nullptr;
        if (m == nullptr)
            return;
        const Canvas::Scope sc(c, tags::editMark, false);
        const float r = std::max(2.0f, std::round(options_.style.px * 0.25f));
        const float cx = innerRect().x + markColumnWidth() * 0.5f, cy = rowTop + rowH_ * 0.5f;
        const Col ink = markInk(options_.inks, m->kind, th);
        switch (m->kind)
        {
            case LineMark::Kind::error:   c.disc(cx, cy, r, ink); break;
            case LineMark::Kind::warning: c.rrect(cx - r, cy - r, 2.0f * r, 2.0f * r, 1.0f, ink); break;
            case LineMark::Kind::info:    c.disc(cx, cy, r, ink.withAlpha(0.0f), 1.0f, ink); break;
        }
    }

    void TextEditor::drawRow(Canvas& c, const Theme& th, int row, float rowTop, bool pair) const
    {
        int ln, from, to;
        rowRange(row, ln, from, to);
        const std::string& s = lines_[static_cast<size_t>(ln)];
        const Rect inner = innerRect(), ta = textArea();
        const float gw = gutterWidth();
        const float x0 = ta.x - scrollX_;
        const int cnt = to - from;
        const int len = static_cast<int>(s.size());

        xs_.resize(static_cast<size_t>(cnt) + 1);
        xs_[0] = 0.0f;
        for (int k = 0; k < cnt; ++k)
            xs_[static_cast<size_t>(k + 1)] = penAfter(s, from + k, xs_[static_cast<size_t>(k)]);

        if (focused_ && options_.highlightLine && ln == caret_.line)
        {
            const Canvas::Scope sc(c, tags::editSelect, false);
            c.rrect(inner.x + gw, rowTop, rightEdge() - inner.x - gw, rowH_, 0.0f, th.ink16.withAlpha(0.45f));
        }
        if (hasSelection())
        {
            const TextPos a = selectionStart(), b = selectionEnd();
            if (a.line <= ln && ln <= b.line)
            {
                const int sFrom = a.line == ln ? a.col : 0, sTo = b.line == ln ? b.col : len;
                const bool pastEnd = b.line > ln && to == len;   // the line break is selected: shown on its last row
                const int rf = std::max(sFrom, from), rt = std::min(sTo, to);
                if (rf < rt || (pastEnd && rf <= to))
                {
                    const float xa = xs_[static_cast<size_t>(rf - from)];
                    const float xb = (rt > rf ? xs_[static_cast<size_t>(rt - from)] : xa) + (pastEnd ? spaceW_ : 0.0f);
                    const Canvas::Scope sc(c, tags::editSelect, false);
                    c.rrect(x0 + xa, rowTop, xb - xa, rowH_, 0.0f, focused_ ? th.accentDim : th.ink16);
                }
            }
        }

        // The row's part of a range, as row-local x: false when it has none. An empty range is one space width from
        // its position, on the row its position is on.
        const auto extent = [&](TextPos f, TextPos t, float& xa, float& xb) {
            int rf = 0, rt = 0;
            if (f == t)
            {
                if (f.line != ln || rowOf(f) != row)
                    return false;
                rf = rt = f.col;
            }
            else
            {
                if (f.line > ln || t.line < ln)
                    return false;
                rf = std::max(f.line == ln ? f.col : 0, from);
                rt = std::min(t.line == ln ? t.col : len, to);
                if (rf >= rt)
                    return false;
            }
            xa = xs_[static_cast<size_t>(rf - from)];
            xb = rt > rf ? xs_[static_cast<size_t>(rt - from)] : xa + spaceW_;
            return true;
        };

        // Decoration backgrounds, under the text: occurrences, then find matches, then the current match.
        if (!shown_.empty())
        {
            const Canvas::Scope sc(c, tags::editMark, false);
            for (const DecorationKind kind :
                 { DecorationKind::highlight, DecorationKind::findMatch, DecorationKind::findCurrent })
                for (const Decoration* d : shown_)
                {
                    float xa = 0.0f, xb = 0.0f;
                    if (d->kind != kind || !extent(d->from, d->to, xa, xb))
                        continue;
                    if (kind == DecorationKind::highlight)
                        c.rrect(x0 + xa, rowTop, xb - xa, rowH_, 0.0f, th.ink16);
                    else if (kind == DecorationKind::findMatch)
                        c.rrect(x0 + xa, rowTop, xb - xa, rowH_, 0.0f, th.accentDim.withAlpha(0.5f));
                    else
                        c.rrect(x0 + xa, rowTop, xb - xa, rowH_, 0.0f, th.accentDim, 1.0f, th.accent);
                }
        }

        // Indent guides, on a line's first row.
        if (options_.indentGuides && from == 0 && tabW_ > 0.0f)
        {
            const int levels = guideLevels(ln);
            if (levels > 0)
            {
                const Canvas::Scope sc(c, tags::editGuide, false);
                for (int k = 0; k < levels; ++k)
                    c.hairlineV(x0 + static_cast<float>(k) * tabW_, rowTop, rowH_, th.ink16);
            }
        }

        if (cnt > 0)
        {
            tokenise(ln);
            const Canvas::Scope sc(c, tags::editText, false);
            size_t si = 0;
            int runStart = -1;
            uint16_t runStyle = 0;
            const auto styleAt = [this, &si](int col) noexcept -> uint16_t {
                while (si < spans_.size() && spans_[si].end <= col)
                    ++si;
                return si < spans_.size() && spans_[si].start <= col ? spans_[si].style : uint16_t{ 0 };
            };
            const auto flush = [&](int endCol) {
                if (runStart < 0)
                    return;
                run_.assign(s, static_cast<size_t>(runStart), static_cast<size_t>(endCol - runStart));
                const text::HighlightStyle hs =
                    runStyle < palette_.size() ? palette_[runStyle] : text::HighlightStyle{};
                TextStyle st = options_.style;
                st.weight += hs.weight;
                const Col col = hs.resolve(th);
                const float xa = x0 + xs_[static_cast<size_t>(runStart - from)];
                c.text(run_.c_str(), xa, rowTop + textTop_, st, col);
                if (hs.underline)
                    c.hairlineH(xa, rowTop + rowH_ - 2.0f,
                                xs_[static_cast<size_t>(endCol - from)] - xs_[static_cast<size_t>(runStart - from)],
                                col);
                runStart = -1;
            };
            for (int k = from; k < to; ++k)
            {
                if (s[static_cast<size_t>(k)] == '\t')
                {
                    flush(k);                            // a tab is a jump of the pen, never a glyph
                    continue;
                }
                const uint16_t st = styleAt(k);
                if (runStart >= 0 && st != runStyle)
                    flush(k);
                if (runStart < 0)
                {
                    runStart = k;
                    runStyle = st;
                }
            }
            flush(to);
        }

        // Trailing blanks: a dot per space, a rule across a tab.
        if (options_.showTrailingBlanks)
        {
            const size_t lastOther = s.find_last_not_of(" \t");
            const int k0 = std::max(lastOther == std::string::npos ? 0 : static_cast<int>(lastOther) + 1, from);
            if (k0 < to)
            {
                const Canvas::Scope sc(c, tags::editGuide, false);
                const float mid = rowTop + rowH_ * 0.5f;
                for (int k = k0; k < to; ++k)
                {
                    const float xa = x0 + xs_[static_cast<size_t>(k - from)];
                    const float xb = x0 + xs_[static_cast<size_t>(k + 1 - from)];
                    if (s[static_cast<size_t>(k)] == ' ')
                        c.rrect((xa + xb) * 0.5f - 0.75f, mid - 0.75f, 1.5f, 1.5f, 0.75f, th.ink32);
                    else
                        c.hairlineH(xa + 2.0f, mid, std::max(0.0f, xb - xa - 4.0f), th.ink32);
                }
            }
        }

        // Over the text: the underlines of the diagnostics, the product's bracket boxes and the pair at the caret.
        if (!shown_.empty() || pair)
        {
            const Canvas::Scope sc(c, tags::editMark, false);
            const float visL = inner.x + gw - x0 - kSquiggleHalf, visR = rightEdge() - x0 + kSquiggleHalf;
            const float yLow = rowTop + rowH_ - 1.0f, yHigh = yLow - kSquiggleAmp;
            const auto squiggle = [&](float xa, float xb, Col ink) {
                // Vertices every kSquiggleHalf px from the row's origin, low on even ones; only what can be seen.
                const float lo = std::max(xa, visL), hi = std::min(xb, visR);
                for (int k = static_cast<int>(std::floor(std::max(lo, 0.0f) / kSquiggleHalf));
                     static_cast<float>(k) * kSquiggleHalf < hi; ++k)
                {
                    const float xk = static_cast<float>(k) * kSquiggleHalf;
                    const float yk = (k & 1) == 0 ? yLow : yHigh, yk1 = (k & 1) == 0 ? yHigh : yLow;
                    const float sa = std::max(xk, lo), sb = std::min(xk + kSquiggleHalf, hi);
                    if (!(sb > sa))
                        continue;
                    const float ya = yk + (yk1 - yk) * (sa - xk) / kSquiggleHalf;
                    const float yb = yk + (yk1 - yk) * (sb - xk) / kSquiggleHalf;
                    c.segment(x0 + sa, ya, x0 + sb, yb, 1.0f, ink);
                }
            };
            const auto box = [&](float xa, float xb) {
                c.rrect(x0 + xa, rowTop + 1.0f, xb - xa, rowH_ - 2.0f, 1.0f, th.ink52.withAlpha(0.0f), 1.0f, th.ink52);
            };
            for (const Decoration* d : shown_)
            {
                float xa = 0.0f, xb = 0.0f;
                if (isBackground(d->kind) || !extent(d->from, d->to, xa, xb))
                    continue;
                switch (d->kind)
                {
                    case DecorationKind::error:   squiggle(xa, xb, options_.inks.error.resolve(th)); break;
                    case DecorationKind::warning: squiggle(xa, xb, options_.inks.warning.resolve(th)); break;
                    case DecorationKind::info:    squiggle(xa, xb, options_.inks.info.resolve(th)); break;
                    case DecorationKind::bracket: box(xa, xb); break;
                    case DecorationKind::highlight:
                    case DecorationKind::findMatch:
                    case DecorationKind::findCurrent: break;
                }
            }
            if (pair)
                for (const TextPos p : { pairOpen_, pairClose_ })
                {
                    float xa = 0.0f, xb = 0.0f;
                    if (extent(p, { p.line, p.col + 1 }, xa, xb))
                        box(xa, xb);
                }
        }
    }

    void TextEditor::drawMinimap(Canvas& c, const Theme& th) const
    {
        const Rect inner = innerRect(), m = minimapArea();
        if (m.isEmpty())
            return;
        const Canvas::Scope sc(c, tags::editMap, false);
        c.hairlineV(m.x, inner.y, inner.h, th.ink16);
        float offset = 0.0f, sliderY = 0.0f, sliderH = 0.0f;
        minimapGeometry(offset, sliderY, sliderH);
        const float mx0 = c.snapX(m.x + 1.0f), my0 = c.snapY(m.y), mx1 = c.snapX(m.right()), my1 = c.snapY(m.bottom());
        const Canvas::ClipScope cs(c, { mx0, my0, std::max(0.0f, mx1 - mx0), std::max(0.0f, my1 - my0) });

        const int rows = rowCount(), tw = std::max(1, options_.tabWidth);
        const int first = clampInt(static_cast<int>(std::floor(offset / kMapRowH)), 0, rows - 1);
        const int last = clampInt(static_cast<int>(std::floor((offset + m.h) / kMapRowH)), 0, rows - 1);
        const float left = m.x + kMapPad;
        const int maxCols = std::max(0, static_cast<int>(std::floor((m.right() - left) / kMapColW)));
        for (int row = first; row <= last; ++row)
        {
            int ln, from, to;
            rowRange(row, ln, from, to);
            const std::string& s = lines_[static_cast<size_t>(ln)];
            const float y = m.y + static_cast<float>(row) * kMapRowH - offset + (kMapRowH - kMapBarH) * 0.5f;
            tokenise(ln);
            int col = 0, runCol = -1;
            uint16_t runStyle = 0;
            const auto flush = [&](int endCol) {
                if (runCol < 0)
                    return;
                const text::HighlightStyle hs =
                    runStyle < palette_.size() ? palette_[runStyle] : text::HighlightStyle{};
                c.rrect(left + static_cast<float>(runCol) * kMapColW, y, static_cast<float>(endCol - runCol) * kMapColW,
                        kMapBarH, 0.0f, hs.resolve(th).withAlpha(0.6f));
                runCol = -1;
            };
            for (int k = from; k < to && col < maxCols; ++k)
            {
                const char ch = s[static_cast<size_t>(k)];
                if (isBlank(ch))
                {
                    flush(col);
                    col = ch == '\t' ? (col / tw + 1) * tw : col + 1;
                    continue;
                }
                const uint16_t st = styleIn(spans_, k);
                if (runCol >= 0 && st != runStyle)
                    flush(col);
                if (runCol < 0)
                {
                    runCol = col;
                    runStyle = st;
                }
                ++col;
            }
            flush(std::min(col, maxCols));
        }
        c.rrect(m.x + 1.0f, sliderY, std::max(0.0f, m.w - 1.0f), sliderH, 0.0f,
                (drag_ == Drag::minimap ? th.ink32 : th.ink16).withAlpha(0.5f));
    }
}

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
#include <utility>

// TextEditor (widgets/TextEditor.h): the rules are in the header; fg.texteditor pins them. The text is a vector of
// lines; every change goes through replaceRange(), which splices the lines, lays the changed ones out again (their
// width, and their wrap breaks when word wrap is on), re-runs the highlighter from the first changed line until a
// start state comes out unchanged, and records one undo step (or extends the last one). Layout is in px from the
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

    float TextEditor::gutterWidth() const
    {
        if (!options_.lineNumbers)
            return 0.0f;
        return static_cast<float>(std::max(2, digitsOf(lineCount()))) * digitW_ + 2.0f * kGutterPad;
    }

    Rect TextEditor::textArea() const
    {
        const Rect inner = innerRect();
        const float pad = options_.padding;
        const float x = inner.x + gutterWidth() + pad, y = inner.y + pad;
        return { x, y, std::max(0.0f, inner.right() - pad - x), std::max(0.0f, inner.h - 2.0f * pad) };
    }

    Rect TextEditor::rowsClip() const noexcept
    {
        const Rect inner = innerRect(), ta = textArea();
        return { inner.x, ta.y, inner.w, ta.h };
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
        const Rect inner = innerRect(), ta = textArea();
        const Rect r{ inner.right() - kBarInset - kBarW, ta.y, kBarW,
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

    TextPos TextEditor::replaceRange(TextPos a, TextPos b, std::string_view txt, Kind kind)
    {
        a = clampPos(a);
        b = clampPos(b);
        if (b < a)
            std::swap(a, b);
        std::string removed = rangeText(a, b);
        if (removed.empty() && txt.empty())
            return a;
        const TextPos caretBefore = caret_, anchorBefore = anchor_;
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

        if (recording_)
        {
            bool merged = false;
            if (!undo_.empty())
            {
                Edit& last = undo_.back();
                // A step never holds a line break with anything else: one it typed or removed stands alone.
                const bool oneLineIn = txt.find('\n') == std::string_view::npos
                                    && last.inserted.find('\n') == std::string::npos;
                const bool oneLineOut = removed.find('\n') == std::string::npos
                                     && last.removed.find('\n') == std::string::npos;
                if (kind == Kind::typing && last.kind == Kind::typing && removed.empty() && oneLineIn
                    && endOf(last.from, last.inserted) == a)
                {
                    last.inserted.append(txt);
                    merged = true;
                }
                else if (kind == Kind::backspace && last.kind == Kind::backspace && txt.empty()
                         && last.inserted.empty() && oneLineOut && b == last.from)
                {
                    last.removed.insert(0, removed);
                    last.from = a;
                    merged = true;
                }
                else if (kind == Kind::del && last.kind == Kind::del && txt.empty() && last.inserted.empty()
                         && oneLineOut && a == last.from)
                {
                    last.removed.append(removed);
                    merged = true;
                }
            }
            if (!merged)
            {
                Edit e;
                e.from = a;
                e.removed = std::move(removed);
                e.inserted.assign(txt);
                e.caretBefore = caretBefore;
                e.anchorBefore = anchorBefore;
                e.kind = kind;
                undo_.push_back(std::move(e));
                if (static_cast<int>(undo_.size()) > options_.maxUndo)
                    undo_.erase(undo_.begin(), undo_.begin() + static_cast<std::ptrdiff_t>(
                                                                 static_cast<int>(undo_.size()) - options_.maxUndo));
            }
            redo_.clear();
        }
        return endOf(a, txt);
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
        const TextPos end = replaceRange(a, b, txt, kind);
        moveCaret(end, false);
        return true;
    }

    bool TextEditor::insert(std::string_view utf8)
    {
        if (!editable())
            return false;
        return typeText(sanitise(utf8), Kind::other);
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
        replaceRange(e.from, endOf(e.from, e.inserted), e.removed, Kind::other);
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
        replaceRange(e.from, endOf(e.from, e.removed), e.inserted, Kind::other);
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
        }
        if (from == caret_)
            return true;                                 // at the start: an edit owns the key (LineEdit's rule)
        replaceRange(from, caret_, {}, word || toRowStart ? Kind::other : Kind::backspace);
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
        replaceRange(caret_, to, {}, word || toRowEnd ? Kind::other : Kind::del);
        moveCaret(caret_, false);
        return true;
    }

    bool TextEditor::typeReturn()
    {
        std::string ins = "\n";
        if (options_.autoIndent)
        {
            const TextPos a = selectionStart();          // the break lands where the selection starts
            const std::string& s = lines_[static_cast<size_t>(a.line)];
            size_t k = 0;
            while (k < static_cast<size_t>(a.col) && k < s.size() && isBlank(s[k]))
                ++k;
            ins.append(s, 0, k);
        }
        return typeText(ins, Kind::other);
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
        const std::string unit = options_.softTabs ? std::string(static_cast<size_t>(options_.tabWidth), ' ')
                                                   : std::string("\t");
        std::string out;
        std::vector<int> delta(static_cast<size_t>(l1 - l0 + 1), 0);
        for (int ln = l0; ln <= l1; ++ln)
        {
            std::string s = lines_[static_cast<size_t>(ln)];
            const int before = static_cast<int>(s.size());
            if (!outdent)
            {
                if (!s.empty() || l0 == l1)
                    s.insert(0, unit);
            }
            else if (!s.empty() && s[0] == '\t')
                s.erase(0, 1);
            else
            {
                size_t k = 0;
                while (k < s.size() && k < static_cast<size_t>(options_.tabWidth) && s[k] == ' ')
                    ++k;
                s.erase(0, k);
            }
            delta[static_cast<size_t>(ln - l0)] = static_cast<int>(s.size()) - before;
            if (ln > l0)
                out += '\n';
            out += s;
        }
        const TextPos from{ l0, 0 }, to{ l1, static_cast<int>(lines_[static_cast<size_t>(l1)].size()) };
        if (out == rangeText(from, to))
            return false;
        const TextPos caretWas = caret_, anchorWas = anchor_;
        replaceRange(from, to, out, Kind::other);
        const auto shifted = [&](TextPos p) {
            if (p.line >= l0 && p.line <= l1)
                p.col = std::max(0, p.col + delta[static_cast<size_t>(p.line - l0)]);
            return clampPos(p);
        };
        anchor_ = shifted(anchorWas);
        moveCaret(shifted(caretWas), true);
        return true;
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
        const char c = static_cast<char>(e.ch);
        return typeText(std::string_view(&c, 1), Kind::typing);
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
                    drawLineNumber(c, th, row, ta.y + static_cast<float>(row) * rowH_ - scrollY_);
            }
            // What scrolls sideways (the band, the selection, the text, the caret) is cut again at the gutter's edge,
            // so text scrolled to the left never runs over the line numbers.
            const float tx0 = std::min(c.snapX(inner.x + gw), cx1);
            const Canvas::ClipScope ts(c, { tx0, cy0, cx1 - tx0, cy1 - cy0 });
            for (int row = first; row <= last; ++row)
                drawRow(c, th, row, ta.y + static_cast<float>(row) * rowH_ - scrollY_);
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
        c.text(buf, innerRect().x + gutterWidth() - kGutterPad, rowTop + textTop_, num,
               ln == caret_.line ? th.ink70 : th.ink32, Align::right);
    }

    void TextEditor::drawRow(Canvas& c, const Theme& th, int row, float rowTop) const
    {
        int ln, from, to;
        rowRange(row, ln, from, to);
        const std::string& s = lines_[static_cast<size_t>(ln)];
        const Rect inner = innerRect(), ta = textArea();
        const float gw = gutterWidth();
        const float x0 = ta.x - scrollX_;
        const int cnt = to - from;

        xs_.resize(static_cast<size_t>(cnt) + 1);
        xs_[0] = 0.0f;
        for (int k = 0; k < cnt; ++k)
            xs_[static_cast<size_t>(k + 1)] = penAfter(s, from + k, xs_[static_cast<size_t>(k)]);

        if (focused_ && options_.highlightLine && ln == caret_.line)
        {
            const Canvas::Scope sc(c, tags::editSelect, false);
            c.rrect(inner.x + gw, rowTop, inner.w - gw, rowH_, 0.0f, th.ink16.withAlpha(0.45f));
        }
        if (hasSelection())
        {
            const TextPos a = selectionStart(), b = selectionEnd();
            if (a.line <= ln && ln <= b.line)
            {
                const int len = static_cast<int>(s.size());
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
        if (cnt <= 0)
            return;

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
            const text::HighlightStyle hs = runStyle < palette_.size() ? palette_[runStyle] : text::HighlightStyle{};
            TextStyle st = options_.style;
            st.weight += hs.weight;
            const Col col = hs.resolve(th);
            const float xa = x0 + xs_[static_cast<size_t>(runStart - from)];
            c.text(run_.c_str(), xa, rowTop + textTop_, st, col);
            if (hs.underline)
                c.hairlineH(xa, rowTop + rowH_ - 2.0f,
                            xs_[static_cast<size_t>(endCol - from)] - xs_[static_cast<size_t>(runStart - from)], col);
            runStart = -1;
        };
        for (int k = from; k < to; ++k)
        {
            if (s[static_cast<size_t>(k)] == '\t')
            {
                flush(k);                                // a tab is a jump of the pen, never a glyph
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
}

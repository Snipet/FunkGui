#pragma once

// A multi-line text editor: the counterpart of text/LineEdit.h for a document (a script, a patch's notes, a code
// field). Unlike LineEdit it is a whole widget: it owns its text, lays it out, draws itself and takes the pointer,
// the wheel and the keys; a product gives it bounds, options, its host (the clipboard) and the input it receives,
// and reads the text back. Core, JUCE-free and platform-neutral: the same source draws the same primitives with
// EditorHost, in a browser and headless, where fg.texteditor pins every rule below and fg.gallery.texteditor the
// pictures.
//
// Text. Lines of printable ASCII (32..126) and tabs, what the atlas draws for typed text (text::printable's rule):
// setText(), insert() and paste() make the text so (CR LF and a lone CR become LF, every other codepoint and every
// malformed sequence becomes '?'), and key() types nothing else. A TextPos is a line index and a byte offset into
// that line, 0-based, so a column is a character count; a position past the end of a line, or past the last line, is
// clamped by every call that takes one. A tab is drawn as the advance to the next tab stop, tabWidth space widths
// apart from the start of its row.
//
// Fonts. The editor draws in one TextStyle (options().style): a preset of editorFont() (code, the default: 12 px,
// no tracking, tabular digits; prose: 13 px, a little weight; compact: 10 px; large: 16 px) or any style of the type
// scale. FunkGui bundles a single face, JetBrains Mono, so the presets differ in size, weight, tracking and digit
// slots, not in face (core/TypeScale.h's rule); a product that bundles another face changes the atlas, not this
// widget. The row height is round(px * lineSpacing).
//
// Keys (key() returns true for what it consumed). "Cmd" is Mods::cmd: Cmd on macOS, and Ctrl on Linux, where
// EditorHost sets cmd and ctrl together; the two keyboard conventions follow the host's commandKeyIsMeta():
// - Meta (macOS): Cmd-Left/Right the row's start/end, Cmd-Up/Down the document's, Alt-Left/Right a word, Alt-Backspace
//   and Alt-Delete a word, Cmd-Backspace to the row's start, Cmd-Delete to its end.
// - Not meta (Linux, a browser on Linux or Windows): Ctrl-Left/Right a word (Alt too), Ctrl-Backspace/Delete a word,
//   Home/End the row, Ctrl-Home/End the document.
// Either way: the arrows move by a character or a row (Up/Down keep the column they started from across shorter
// rows, in px, until a horizontal move), Home toggles between the first non-blank character and column 0, End goes to
// the row's end, Page Up/Down move by the rows in view and scroll by the same; Shift with any move extends the
// selection, and a plain Left/Right with a selection collapses it to its start/end. Return breaks the line and, with
// autoIndent, copies the line's leading blanks; Tab inserts a tab (softTabs: spaces to the next stop), and with a
// selection over several lines indents them, Shift-Tab outdenting; Backspace and Delete remove the selection, else
// one character (softTabs: Backspace over leading spaces removes to the previous stop), joining lines at the ends;
// typing replaces the selection. Cmd-A selects all, Cmd-C/X/V copy, cut and paste (through the host; see below),
// Cmd-Z undoes, Cmd-Shift-Z and Cmd-Y redo. Escape and Return-with-Cmd are the product's (false). A read-only editor
// moves and copies and refuses every edit (false). Nothing is handled while the editor is not focused.
//
// Pointer. A press in the text focuses the editor and puts the caret at the nearest character boundary (Shift
// extends the selection); a drag selects; a double click (clicks == 2, or doubleClick()) selects the word and a
// triple click (clicks == 3) the line, and a drag after either extends by words or lines; a press in the gutter
// selects the line. A popup press (PointerEvent::popup) does nothing: the product's menu. The wheel scrolls (dy rows,
// dx columns; a notch is three rows, a smooth unit 512 px as JUCE's precise device delivers 0.5/256 per point) and
// returns true whenever the text overflows the box, false when nothing can scroll (the event is then the parent's).
// The two scrollbars, drawn only when the text overflows, can be dragged, and a press on a bar's track jumps there.
//
// Clipboard and undo. copy() and cut() put the selection on the host's clipboard (HostServices::copyText) and keep
// it in the editor as well; paste() inserts the host's clipboard text (clipboardText()) when there is any, else the
// editor's own, which is how a web host (whose read is empty) still pastes what was copied here. Every edit is
// undoable: consecutive typed characters coalesce into one step, so do consecutive Backspaces and Deletes; a line
// break typed or removed is a step of its own, and a paste, an indent, a selection replaced or any other kind of edit
// starts a new step; redo is cleared by a new edit; at most maxUndo steps are kept. Undo restores the caret and the
// selection as they were before the step.
//
// Highlighting. setHighlighter() gives the editor a text::Highlighter; the editor keeps the state every line starts
// in, re-runs the highlighter after an edit from the first changed line until a start state comes out unchanged, and
// tokenises only the rows it draws. setPalette() maps style indices to inks of the theme, weights and underlines
// (defaultPalette() fits KeywordHighlighter's indices); index 0 and any index past the palette are plain ink100.
//
// Drawing (tags EDIT_FRAME, EDIT_GUTTER, EDIT_TEXT, EDIT_SELECT, EDIT_CARET): a 1 px ink16 frame with 2 px corners;
// line numbers right-aligned in ink32 (the caret's line ink70) on the first row of each line, with an ink16 rule; the
// caret's line banded in ink16 while focused (highlightLine; every row of it when it wraps); the selection in
// accentDim (ink16 while not focused), a space width past a line end when the selection crosses it; the text; a
// 1.5 px accent caret while focused (and, with blinkCaret, in the first half of each blinkPeriod, which tick()
// advances: at a host's idle rate, as it needs no full rate); the placeholder in ink32 while the text is empty; 4 px
// ink32 scrollbars (ink52 while dragged) at the right and the bottom while the text overflows; and the focus ring
// around the bounds when asked. Everything
// that scrolls is clipped to the box (Canvas::pushClip), so only the rows in view are recorded, and what scrolls
// sideways (the band, the selection, the text, the caret) is cut again at the gutter's edge, clear of the numbers.
//
// Accessibility: one editableText item (a11yrole::editableText), its value the whole text, its description the
// caret's line and column (1-based) and the selection's length.
//
// ---- v0.22.0: hooks for a code editor -------------------------------------------------------------------------------
// What a product needs to make a language's editor of this widget (diagnostics, occurrences, a find bar, completion,
// hover cards, a formatter). The editor draws what the product marks and gives it the geometry and the edits; the
// product's overlays (a completion list, a hover card, signature help, a find bar) are the product's, drawn above the
// editor, and the product sees every key first (so it keeps Up, Down, Return and Escape while its list is open).
// Every option below is off by default: an editor that does not ask draws exactly as before.
//
// Decorations (tag EDIT_MARK). setDecorations() replaces the ranges of the kinds in its mask (a bit per
// DecorationKind, every kind by default), so diagnostics, occurrences and find matches are set apart. error, warning
// and info draw a wavy underline (2 px high, a 4 px period on the row's own grid, so it never crawls as the text
// scrolls) in options().inks; highlight (a word's occurrences) an ink16 background; findMatch an accentDim
// background at half alpha; findCurrent an accentDim background boxed in accent; bracket a 1 px ink52 box.
// Backgrounds go over the selection and under the text, underlines and boxes over the text; an empty range draws one
// space width from its position (a diagnostic at a line's end). Ranges stay in place across edits until the product
// sets them again: they move with the text around them as a caret does, text typed inside one widens it, text typed
// at either end does not, and a range whose text is all removed is dropped (an empty range moves as one point).
// setText() clears them. setLineMarks() replaces the line marks (a line and a severity; the most severe on a line is
// drawn): with markColumn, a column at the gutter's left (round(style.px) wide) shows a disc (error), a square
// (warning) or a ring (info) in options().inks on the line's first row; without it the mark tints the line number;
// without either nothing shows. A mark moves with its line's start. The theme has no danger or warning ink (nine
// inks, two accents), so DecorationInks defaults error and warning to fixed colours and info to the theme's signal.
//
// Code-editor options. indentGuides (tag EDIT_GUIDE): a 1 px ink16 rule at the start of each indent level (tabWidth
// space widths, tabs expanded) of a line's leading blanks, on its first row; a blank line takes the level of the
// lines around it (theirs when they agree, else one more than the shallower; at most 100 lines each way).
// showTrailingBlanks (EDIT_GUIDE): the blanks after a line's last other character in ink32, a 1.5 px dot per space and
// a rule across a tab. matchBrackets: while focused, the bracket pair at the caret (bracketPair(): the character
// before the caret, else the one after it, of ()[]{}) boxed in ink52; nesting is counted only among brackets of the
// bracket's own highlight style (one in a string or a comment does not count against one in code), over at most
// 20000 characters. autoClosePairs (typed keys only, not insert()): ( [ { insert their closer after the caret when
// the next character is a blank, the line's end or one of ;:.,=}])>, and " ' too when the character before is not a
// word character or that quote; an opener or a quote typed over a selection wraps it (the selection stays on the
// text inside); a closer typed just before one the editor inserted steps over it (while the caret stays on that line,
// not past it); Backspace between an empty pair (any of the five) removes both. braceIndent: Return after an opener
// ( [ { (blanks between it and the caret dropped) indents the new line one level (a tab, or tabWidth spaces with
// softTabs) past the line's own, and with that opener's closer next (blanks between dropped) puts the closer on a
// line of its own at the line's indent, the caret on the indented line between; a closer typed on a line that is
// blank before the caret takes the indent of its opener's line when that is shallower, else outdents one level.
// minimap (tag EDIT_MAP): a strip minimapWidth px wide (16 at least, half the box at most) at the box's right, the
// text and the vertical scrollbar to its left behind an ink16 rule: each visual row 2 px high, each column 1 px
// wide (tabs expanded), a bar per run of non-blank characters in its highlight style's ink; it scrolls in proportion
// with the text when its rows do not fit; a slider (ink16, ink32 while dragged) shades the rows in view; a press on
// the slider drags it, a press beside it centres the slider there and drags on.
//
// Geometry for the product's overlays: pointOf() (a position's character cell's top-left, scrolled, in the Panel's
// px; a position at a wrap break starts the row after it), posAt() (the nearest boundary to a point over the rows;
// nullopt over the frame, the gutter, the minimap, the padding above and below the rows, or past the last row),
// visibleLineRange() (the lines with any row in view), wordRangeAt() (the run of identifier characters [A-Za-z0-9_]
// touching a position; empty when there is none), rowHeight().
//
// Edits for language services: each is one undo step that restores the caret and the selection, sanitises its text,
// and is false with no change when read-only or when nothing would change. replaceRange(): the caret after the
// insertion (a completion replaces the word before the caret). applyEdits(): a formatter's or a rename's list, its
// positions in the text as it stands, in any order (sorted by the editor; insertions at one position keep their
// order), refused whole when two overlap; the caret and the anchor move with the text: inside a replaced span to its
// replacement's end, at or after a span's end with the text after it. replaceText(): the whole text, recorded as the
// one replacement between the common prefix and suffix, the caret and the anchor kept at their line and column
// (clamped).

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/panel/Input.h>
#include <funkgui/text/Highlighter.h>
#include <funkgui/text/TextStyle.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace funkgui
{
    class Canvas;
    class HostServices;
    struct Theme;

    // A position in the text: a line and a byte offset into it (one byte per character).
    struct TextPos
    {
        int line = 0;
        int col = 0;

        friend constexpr bool operator==(TextPos a, TextPos b) noexcept { return a.line == b.line && a.col == b.col; }
        friend constexpr bool operator!=(TextPos a, TextPos b) noexcept { return !(a == b); }
        friend constexpr bool operator<(TextPos a, TextPos b) noexcept
        {
            return a.line < b.line || (a.line == b.line && a.col < b.col);
        }
        friend constexpr bool operator<=(TextPos a, TextPos b) noexcept { return !(b < a); }
        friend constexpr bool operator>(TextPos a, TextPos b) noexcept { return b < a; }
        friend constexpr bool operator>=(TextPos a, TextPos b) noexcept { return !(a < b); }
    };

    // A range of the text, [from, to). The editor clamps and orders every range it takes.
    struct TextRange
    {
        TextPos from{};
        TextPos to{};

        constexpr bool empty() const noexcept { return from == to; }
    };

    // The lines from `first` to `last`, both included.
    struct LineRange
    {
        int first = 0;
        int last = 0;
    };

    // One edit of a language service (a formatter, a rename, a quick fix): the text [from, to) becomes `text`.
    struct TextEdit
    {
        TextPos     from{};
        TextPos     to{};
        std::string text{};
    };

    // What a decoration draws (see the header comment).
    enum class DecorationKind : uint8_t { error, warning, info, highlight, findMatch, findCurrent, bracket };

    constexpr uint32_t decorationBit(DecorationKind k) noexcept { return 1u << static_cast<unsigned>(k); }
    inline constexpr uint32_t kDiagnosticDecorations = decorationBit(DecorationKind::error)
                                                     | decorationBit(DecorationKind::warning)
                                                     | decorationBit(DecorationKind::info);
    inline constexpr uint32_t kFindDecorations = decorationBit(DecorationKind::findMatch)
                                               | decorationBit(DecorationKind::findCurrent);
    inline constexpr uint32_t kAllDecorations = 0x7Fu;

    struct Decoration
    {
        TextPos        from{};
        TextPos        to{};
        DecorationKind kind = DecorationKind::error;
    };

    // A mark on a line, drawn in the gutter (see the header comment).
    struct LineMark
    {
        enum class Kind : uint8_t { info, warning, error };   // in order of severity

        int  line = 0;
        Kind kind = Kind::error;
    };

    // The inks of the underlines and the line marks (only HighlightStyle's ink and colour are read).
    struct DecorationInks
    {
        text::HighlightStyle error{ text::HighlightStyle::Ink::custom, { 0xE5, 0x48, 0x4D, 0xFF }, 0.0f, false };
        text::HighlightStyle warning{ text::HighlightStyle::Ink::custom, { 0xE2, 0xA3, 0x36, 0xFF }, 0.0f, false };
        text::HighlightStyle info{ text::HighlightStyle::Ink::signal, {}, 0.0f, false };
    };

    // The font presets: styles of the type scale (one face; see the header comment).
    enum class EditorFont : uint8_t { code, prose, compact, large };

    constexpr TextStyle editorFont(EditorFont f) noexcept
    {
        switch (f)
        {
            case EditorFont::prose:   return { 13.0f, 0.0f, 0.02f, false };
            case EditorFont::compact: return { 10.0f, 0.0f, 0.03f, true };
            case EditorFont::large:   return { 16.0f, 0.0f, 0.00f, true };
            case EditorFont::code:    break;
        }
        return { 12.0f, 0.0f, 0.00f, true };
    }

    struct TextEditorOptions
    {
        TextStyle   style = editorFont(EditorFont::code);
        float       lineSpacing = 1.4f;                  // row height = round(style.px * lineSpacing), at least 1
        int         tabWidth = 4;                        // tab stops every tabWidth space widths (at least 1)
        bool        softTabs = false;                    // Tab inserts spaces to the next stop
        bool        lineNumbers = true;                  // the gutter
        bool        wordWrap = false;                    // rows break at the box's width (after a blank when one fits)
        bool        readOnly = false;
        bool        autoIndent = true;                   // Return copies the line's leading blanks
        bool        highlightLine = true;                // the caret's line banded while focused
        bool        blinkCaret = false;                  // the caret blinks while focused (tick() drives it)
        float       blinkPeriod = 1.0f;                  // seconds, on for the first half
        float       padding = 6.0f;                      // between the frame (and the gutter) and the text
        int         maxUndo = 1000;                      // undo steps kept
        std::string placeholder{};                       // shown in ink32 while the text is empty
        std::string a11yTitle = "Text";                  // the accessibility item's title

        // v0.22.0, the code-editor looks and keys (see the header comment); all off by default
        bool        indentGuides = false;                // a rule at each indent level of the leading blanks
        bool        showTrailingBlanks = false;          // blanks at a line's end drawn as dots and rules
        bool        matchBrackets = false;               // the bracket pair at the caret boxed while focused
        bool        autoClosePairs = false;              // ( [ { " ' typed insert their closer; step over; pair delete
        bool        braceIndent = false;                 // Return after an opener indents; a closer typed outdents
        bool        minimap = false;                     // an overview of the rows at the box's right
        float       minimapWidth = 64.0f;                // px, at least 16, at most half the box
        bool        markColumn = false;                  // a column at the gutter's left for the line marks
        DecorationInks inks{};                           // the underlines' and the line marks' inks
    };

    class TextEditor
    {
    public:
        explicit TextEditor(uint32_t a11yId = 0);
        ~TextEditor();

        TextEditor(const TextEditor&) = delete;
        TextEditor& operator=(const TextEditor&) = delete;

        // ---- configuration -----------------------------------------------------------------------------------------
        void setOptions(const TextEditorOptions&);       // relays out; the text, the caret and the undo steps stay
        const TextEditorOptions& options() const noexcept { return options_; }
        void setFont(EditorFont);                        // options().style = editorFont(f)
        void setStyle(const TextStyle&);                 // any style of the type scale
        void setBounds(const Rect&);                     // the box, frame included; relays out when the width changes
        const Rect& bounds() const noexcept { return bounds_; }
        void attach(HostServices*);                      // the clipboard and the keyboard convention; nullptr = none
        void setHighlighter(std::unique_ptr<text::Highlighter>);   // nullptr: plain text (the default)
        const text::Highlighter* highlighter() const noexcept { return highlighter_.get(); }
        void setPalette(std::vector<text::HighlightStyle>);
        const std::vector<text::HighlightStyle>& palette() const noexcept { return palette_; }
        static std::vector<text::HighlightStyle> defaultPalette();   // KeywordHighlighter's eight styles

        // ---- the text ----------------------------------------------------------------------------------------------
        void setText(std::string_view utf8);             // replaces everything; the caret at the start; undo cleared
        std::string text() const;                        // the lines joined with LF
        int lineCount() const noexcept { return static_cast<int>(lines_.size()); }
        const std::string& line(int i) const;            // "" for an index outside the text
        TextPos endPos() const noexcept;                 // after the last character
        uint32_t revision() const noexcept { return revision_; }   // bumps on every text change
        bool modified() const noexcept { return modified_; }       // since the last clearModified() or setText()
        void clearModified() noexcept { modified_ = false; }
        static std::string sanitise(std::string_view utf8);        // the text rule above, with LF kept

        // ---- the caret and the selection ---------------------------------------------------------------------------
        TextPos caret() const noexcept { return caret_; }
        TextPos anchor() const noexcept { return anchor_; }         // the selection's other end (== caret: none)
        bool    hasSelection() const noexcept { return caret_ != anchor_; }
        TextPos selectionStart() const noexcept;
        TextPos selectionEnd() const noexcept;
        std::string selectedText() const;
        void setCaret(TextPos, bool extend = false);     // extend: the anchor stays, else it follows
        void select(TextPos anchor, TextPos caret);
        void selectAll();
        // Moves the selection to the next match of `needle` (one line, matched within a line) after the selection
        // (forward) or before it, wrapping round when `wrapAround`; false and no change when there is none.
        bool find(std::string_view needle, bool forward = true, bool caseSensitive = true, bool wrapAround = true);

        // ---- decorations (v0.22.0; see the header comment) ----------------------------------------------------------
        // Replaces the decorations of the kinds in `kinds` (decorationBit()s) with those of the list that are of
        // those kinds; the other kinds stay. setDecorations({}, kinds) clears them.
        void setDecorations(std::vector<Decoration>, uint32_t kinds = kAllDecorations);
        const std::vector<Decoration>& decorations() const noexcept { return decorations_; }   // clamped, by `from`
        void setLineMarks(std::vector<LineMark>);        // replaces them all; a line outside the text is dropped
        const std::vector<LineMark>& lineMarks() const noexcept { return lineMarks_; }          // by line
        // The bracket pair at `at` by matchBrackets' rule (whatever the option): the two brackets' positions.
        bool bracketPair(TextPos at, TextPos& open, TextPos& close) const;

        // ---- editing (every call undoable; false and no change when read-only, or when nothing changes) -----------
        bool insert(std::string_view utf8);              // at the caret, replacing the selection; caret after it
        bool replaceRange(TextPos from, TextPos to, std::string_view utf8);   // one step; the caret after it
        bool applyEdits(std::vector<TextEdit>);          // one step; refused whole when two edits overlap
        bool replaceText(std::string_view utf8);         // the whole text as one step; the caret's line and column kept
        bool deleteSelection();
        bool undo();
        bool redo();
        bool canUndo() const noexcept { return !undo_.empty(); }
        bool canRedo() const noexcept { return !redo_.empty(); }
        bool copy();                                     // false without a selection
        bool cut();
        bool paste();

        // ---- focus, frame, scroll ----------------------------------------------------------------------------------
        void setFocused(bool);
        bool focused() const noexcept { return focused_; }
        void tick(float dt);                             // the caret blink
        bool wantsFullRate() const noexcept { return false; }
        void draw(Canvas&, const Theme&, bool focusRing = false) const;
        Point scroll() const noexcept { return { scrollX_, scrollY_ }; }   // px
        void  setScroll(float x, float y);               // clamped to the text's extent
        void  scrollToCaret();
        float contentWidth() const noexcept { return maxLineW_; }
        float contentHeight() const noexcept { return static_cast<float>(rowCount()) * rowH_; }

        // ---- input -------------------------------------------------------------------------------------------------
        bool   contains(Point p) const noexcept { return bounds_.contains(p); }
        void   pointerDown(const PointerEvent&);
        void   pointerDrag(const PointerEvent&);
        void   pointerUp(const PointerEvent&);
        void   doubleClick(const PointerEvent&);
        bool   wheel(const WheelEvent&);
        bool   key(const KeyEvent&);
        Cursor cursorAt(Point) const noexcept;           // normal (there is no text cursor in funkgui::Cursor)

        // ---- accessibility -----------------------------------------------------------------------------------------
        void accessibility(A11yItem&) const;
        uint32_t a11yId() const noexcept { return a11yId_; }

        // ---- layout, for a product that draws around the editor -----------------------------------------------------
        float   rowHeight() const noexcept { return rowH_; }
        float   gutterWidth() const;                     // 0 without line numbers
        Rect    textArea() const;                        // where the rows are drawn, before scrolling
        int     rowCount() const noexcept;               // visual rows (== lineCount() without word wrap)
        int     rowOf(TextPos) const;                    // the visual row a position is on
        TextPos positionAt(Point) const;                 // the nearest character boundary to a point in the box
        Point   caretPoint() const;                      // the caret's top-left, in the Panel's px (scrolled)

        // ---- geometry for overlays (v0.22.0; see the header comment) ------------------------------------------------
        Point     pointOf(TextPos) const;                // a position's character cell's top-left (scrolled)
        std::optional<TextPos> posAt(Point) const;       // the nearest boundary; nullopt outside the rows
        LineRange visibleLineRange() const;              // the lines with any row in view
        TextRange wordRangeAt(TextPos) const;            // the identifier characters touching a position
        Rect      minimapArea() const noexcept;          // where the minimap's rows draw; empty without one

    private:
        enum class Kind : uint8_t { typing, backspace, del, other };
        enum class Drag : uint8_t { none, chars, words, lines, vbar, hbar, minimap };

        // One replacement: the text `removed` at `from` became `inserted`.
        struct Piece
        {
            TextPos     from{};
            std::string removed, inserted;
        };

        // One undo step: its pieces in the order they were applied (undone in reverse); typing, Backspace and Delete
        // steps have one piece, which the next key of the same kind may extend.
        struct Edit
        {
            std::vector<Piece> pieces;
            TextPos     caretBefore{}, anchorBefore{}, caretAfter{}, anchorAfter{};
            Kind        kind = Kind::other;
        };

        // layout
        void    rebuildMetrics();
        void    relayoutAll();
        void    relayoutLine(int i);
        void    reindexRows();
        float   penAfter(const std::string& s, int k, float pen) const noexcept;
        float   xOf(const std::string& s, int from, int col) const noexcept;
        int     colAt(const std::string& s, int from, int to, float x) const noexcept;
        void    rowRange(int row, int& ln, int& from, int& to) const noexcept;
        void    caretRowRange(int& from, int& to) const noexcept;   // the caret's visual row (rowOfCaret)
        int     rowStartOf(TextPos) const noexcept;      // the first column of the row a position is on
        int     rowEndOf(TextPos) const noexcept;        // the last column (the break, or the line's length)
        Rect    innerRect() const noexcept;
        float   rightEdge() const noexcept;              // the rows' right edge: the inner box's, or the minimap's left
        Rect    rowsClip() const noexcept;
        float   markColumnWidth() const noexcept;        // 0 without markColumn
        void    minimapGeometry(float& offset, float& sliderY, float& sliderH) const noexcept;
        bool    vbarShown() const noexcept;
        bool    hbarShown() const noexcept;
        Rect    vbarRect(float& thumbY, float& thumbH) const noexcept;
        Rect    hbarRect(float& thumbX, float& thumbW) const noexcept;
        float   maxScrollX() const noexcept;
        float   maxScrollY() const noexcept;
        void    clampScroll();

        // text
        TextPos clampPos(TextPos) const noexcept;
        TextPos endOf(TextPos at, std::string_view inserted) const noexcept;
        std::string rangeText(TextPos a, TextPos b) const;
        TextPos change(TextPos a, TextPos b, std::string_view text, Kind kind);    // spliceText, recorded
        TextPos spliceText(TextPos a, TextPos b, std::string_view text);           // a <= b, clamped; not recorded
        void    pushStep(Edit&&);
        void    mapThrough(TextPos a, TextPos b, TextPos end);   // decorations, marks and auto-closers after a splice
        void    rehighlightFrom(int firstLine, int lastChanged);
        void    tokenise(int ln) const;                  // spans_ for a line
        uint16_t styleIn(const std::vector<text::Span>&, int col) const noexcept;
        bool    scanBracket(TextPos from, char open, char close, bool forward, int style, TextPos& out) const;
        int     guideLevels(int ln) const;               // indent levels of a line (a blank one: its neighbours')
        int     indentLevelsOf(int ln) const noexcept;   // -1 for a blank line
        std::string indentUnit() const;

        // movement
        TextPos prevChar(TextPos) const noexcept;
        TextPos nextChar(TextPos) const noexcept;
        TextPos prevWord(TextPos) const noexcept;
        TextPos nextWord(TextPos) const noexcept;
        void    wordAt(TextPos, TextPos& start, TextPos& end) const noexcept;
        void    lineAt(TextPos, TextPos& start, TextPos& end) const noexcept;
        TextPos rowMove(int fromRow, int rows, float goalX, bool& up) const noexcept;
        float   goalXOfCaret() const noexcept;
        int     rowOfCaret() const noexcept;             // rowOf(caret_), minus one when the caret sits at a break
        TextPos hitTest(Point, bool& up) const;
        void    moveCaret(TextPos, bool extend, bool keepGoal = false, bool up = false);
        bool    commandKeyIsMeta() const noexcept;
        bool    editable() const noexcept { return !options_.readOnly; }
        bool    typeText(std::string_view, Kind);       // replaces the selection, caret after; false when nothing
        bool    typeChar(char);                          // a typed character: autoClosePairs and braceIndent first
        bool    outdentCloser(char);                     // braceIndent: a closer typed on a blank-indented line
        bool    indentLines(bool outdent);
        bool    typeTab();
        bool    typeReturn();
        bool    eraseBackward(bool word, bool toRowStart);
        bool    eraseForward(bool word, bool toRowEnd);

        // drawing
        void    drawLineNumber(Canvas&, const Theme&, int row, float rowTop) const;
        void    drawLineMark(Canvas&, const Theme&, int row, float rowTop) const;
        void    drawRow(Canvas&, const Theme&, int row, float rowTop, bool pair) const;   // pair: box pairOpen_/Close_
        void    drawMinimap(Canvas&, const Theme&) const;
        const LineMark* markOn(int ln) const noexcept;  // the most severe mark on a line, or nullptr

        TextEditorOptions        options_;
        uint32_t                 a11yId_ = 0;
        Rect                     bounds_{};
        HostServices*            host_ = nullptr;
        std::unique_ptr<text::Highlighter> highlighter_;
        std::vector<text::HighlightStyle>  palette_;

        std::vector<std::string>      lines_;            // at least one
        std::vector<std::vector<int>> breaks_;           // per line: the columns its rows after the first start at
        std::vector<int>              rowOfLine_;        // per line, the index of its first row; [n] = rowCount()
        std::vector<float>            lineW_;            // per line, its width in px (tabs expanded)
        std::vector<uint32_t>         lineState_;        // per line, the highlighter state it starts in
        float                         maxLineW_ = 0.0f;

        TextPos  caret_{}, anchor_{};
        float    goalX_ = -1.0f;                         // the column Up/Down keep, in px; < 0: none
        bool     caretUp_ = false;                       // the caret at a wrap break belongs to the row before it
        float    scrollX_ = 0.0f, scrollY_ = 0.0f;
        bool     focused_ = false;
        float    blinkT_ = 0.0f;
        uint32_t revision_ = 0;
        bool     modified_ = false;

        std::vector<Edit> undo_, redo_;
        bool     recording_ = true;

        std::string clipboard_;                          // the editor's own copy of what it last copied

        std::vector<Decoration> decorations_;            // by from
        std::vector<LineMark>   lineMarks_;              // by line
        std::vector<TextPos>    autoClosers_;            // closers autoClosePairs inserted, still to step over
        uint32_t                styleGen_ = 0;           // bumps with the highlighter (the bracket cache's key)

        Drag     drag_ = Drag::none;
        TextPos  dragStart_{}, dragEnd_{};               // the word or line the drag began on
        float    dragOffset_ = 0.0f;                     // scrollbar drag: the pointer's offset into the thumb

        // the font at options_.style: advances per ASCII character, tracking included
        float    adv_[128] = {};
        float    scale_ = 0.0f, spaceW_ = 0.0f, tabW_ = 0.0f, digitW_ = 0.0f, rowH_ = 1.0f, textTop_ = 0.0f;
        bool     fontOk_ = false;

        mutable std::vector<text::Span> spans_;          // scratch: the spans of the line last tokenised
        mutable int                     spansLine_ = -1;
        mutable uint32_t                spansRev_ = 0;
        mutable std::vector<float>      xs_;             // scratch: the x of each column of a row
        mutable std::string             run_;            // scratch: one run of text
        mutable std::vector<const Decoration*> shown_;   // scratch: the decorations on the lines in view
        mutable std::vector<text::Span> scanSpans_;      // scratch: a line's spans while matching brackets
        mutable bool    pairOk_ = false;                 // the bracket pair at the caret, cached by revision, caret
        mutable bool    pairValid_ = false;              // and style generation
        mutable TextPos pairAt_{}, pairOpen_{}, pairClose_{};
        mutable uint32_t pairRev_ = 0, pairGen_ = 0;
    };
}

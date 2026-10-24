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

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/panel/Input.h>
#include <funkgui/text/Highlighter.h>
#include <funkgui/text/TextStyle.h>

#include <cstdint>
#include <memory>
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

        // ---- editing (every call undoable; false and no change when read-only, or when nothing changes) -----------
        bool insert(std::string_view utf8);              // at the caret, replacing the selection; caret after it
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

    private:
        enum class Kind : uint8_t { typing, backspace, del, other };
        enum class Drag : uint8_t { none, chars, words, lines, vbar, hbar };

        struct Edit
        {
            TextPos     from{};
            std::string removed, inserted;
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
        Rect    rowsClip() const noexcept;
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
        TextPos replaceRange(TextPos a, TextPos b, std::string_view text, Kind kind);
        void    rehighlightFrom(int firstLine, int lastChanged);
        void    tokenise(int ln) const;                  // spans_ for a line

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
        bool    indentLines(bool outdent);
        bool    typeTab();
        bool    typeReturn();
        bool    eraseBackward(bool word, bool toRowStart);
        bool    eraseForward(bool word, bool toRowEnd);

        // drawing
        void    drawLineNumber(Canvas&, const Theme&, int row, float rowTop) const;
        void    drawRow(Canvas&, const Theme&, int row, float rowTop) const;

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
    };
}

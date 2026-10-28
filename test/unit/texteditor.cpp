// FUNKGUI_TEST name=fg.texteditor timeout=300 gpu=0
//
// fg.texteditor: the multi-line editor (widgets/TextEditor.h) and its highlighters (text/Highlighter.h), driven through
// a Panel over HeadlessHost (keys by the host's grammar, clicks, drags, double clicks, the wheel, the clipboard) and
// through the widget's own calls. Spec rows only: the text rule (line breaks, tabs, '?' for what the atlas cannot
// draw); typing, Return with auto-indent, Tab, soft tabs, indent and outdent of a selection; every move of the header
// under both keyboard conventions (meta and not), the goal column, Home's toggle, the page keys; the selection by
// Shift, click, drag, double and triple click, the gutter and Cmd-A, and what a plain arrow does to it; Backspace and
// Delete by character, word and row, joining lines; undo and redo with coalesced typing and deletes, redo cleared by an
// edit, the step cap; copy, cut and paste through the host and the editor's own clipboard; find; word wrap (rows, Up
// and Down across them, End on a wrapped row, hit tests); scrolling (the wheel, scrollToCaret, the clamp, the
// scrollbars, a thumb drag); KeywordHighlighter's spans and states, and the editor re-highlighting after an edit that
// opens or closes a block comment; the palette (inks, weights, underlines); what is drawn (tags, the clip, no missing
// glyph, theme invariance, the caret only while focused, the placeholder, the blink); the a11y item; read-only; the
// font presets. v0.22.0, the code-editor hooks: decorations (set by kind, clamped and sorted, moved with the text and
// dropped with it, kept on their text through indent, outdent and a wrap, cleared by setText; where the underlines,
// backgrounds and boxes are drawn) and line marks (moved with their lines, through indent and its undo too, the mark
// column or the tinted number); bracketPair (before and after the caret, nesting, across lines, unmatched, only its
// own highlight style) and its boxes; autoClosePairs (insert, step over, wrap, pair delete, the quote and word rules,
// undo) and braceIndent (Return between and after an opener, from the line's own indent without autoIndent, a closer
// typed on a blank line); replaceRange, applyEdits and replaceText (one undo step each, the caret mapped or kept,
// overlap refused, decorations moved); pointOf, posAt, visibleLineRange, wordRangeAt; indent guides, trailing blanks
// and the minimap (its geometry, a press and a drag). No JUCE: the test also runs without it and as wasm32.

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/Fingerprint.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Theme.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Panel.h>
#include <funkgui/test/Harness.h>
#include <funkgui/text/FontService.h>
#include <funkgui/text/Highlighter.h>
#include <funkgui/widgets/TextEditor.h>

#include <cmath>
#include <cstdint>
#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace T = funkgui::test;
using funkgui::A11yAction;
using funkgui::A11yItem;
using funkgui::Canvas;
using funkgui::Col;
using funkgui::Decoration;
using DK = funkgui::DecorationKind;
using funkgui::EditorFont;
using funkgui::HeadlessHost;
using funkgui::HostServices;
using funkgui::Key;
using funkgui::KeyEvent;
using funkgui::LineMark;
using funkgui::Mods;
using funkgui::Point;
using funkgui::PointerEvent;
using funkgui::Prim;
using funkgui::PrimList;
using funkgui::Rect;
using funkgui::TextEditor;
using funkgui::TextEditorOptions;
using funkgui::TextPos;
using funkgui::Theme;
using funkgui::WheelEvent;
using funkgui::text::HighlightStyle;
using funkgui::text::KeywordHighlighter;
using funkgui::text::KeywordRules;
using funkgui::text::Span;
namespace tags = funkgui::tags;

namespace
{
    constexpr Rect kBox{ 10.0f, 10.0f, 300.0f, 120.0f };

    class EditorPanel final : public funkgui::Panel
    {
    public:
        EditorPanel() { ed.setBounds(kBox); }

        void attach(HostServices& h) override { ed.attach(&h); }
        int  width() const override { return 320; }
        int  height() const override { return 140; }
        void tick(float dt) override { ed.tick(dt); }
        void draw(Canvas& c, const Theme& th) override { ed.draw(c, th, ed.focused()); }
        bool wantsFullRate() const override { return false; }
        void pointerDown(const PointerEvent& e) override { ed.pointerDown(e); }
        void pointerDrag(const PointerEvent& e) override { ed.pointerDrag(e); }
        void pointerUp(const PointerEvent& e) override { ed.pointerUp(e); }
        void doubleClick(const PointerEvent& e) override { ed.doubleClick(e); }
        bool wheel(const WheelEvent& e) override { return ed.wheel(e); }
        bool key(const KeyEvent& e) override { return ed.key(e); }
        void accessibility(std::vector<A11yItem>& out) const override
        {
            A11yItem it;
            ed.accessibility(it);
            out.push_back(it);
        }
        uint32_t a11yRevision() const override { return 0; }
        void a11yAction(uint32_t, A11yAction, double) override {}
        void closeGestures() override {}

        TextEditor ed{ 7 };
    };

    struct Fixture
    {
        EditorPanel  panel;
        HeadlessHost host;

        explicit Fixture(bool meta = true) : host(panel, 0, 2.0f) { host.setCommandKeyIsMeta(meta); }

        TextEditor& ed() { return panel.ed; }

        // The point a click lands on to put the caret at p: the caret's own point, a little right and down.
        Point pointOf(TextPos p)
        {
            const TextPos c = ed().caret(), a = ed().anchor();
            ed().setCaret(p);
            const Point q = ed().caretPoint();
            ed().select(a, c);
            return { q.x + 1.0f, q.y + ed().rowHeight() * 0.5f };
        }

        void clickAt(TextPos p, Mods m = {})
        {
            const Point q = pointOf(p);
            host.click(q.x, q.y, m);
        }
    };

    std::string pos(TextPos p) { return std::to_string(p.line) + ":" + std::to_string(p.col); }

    bool is(std::string_view a, std::string_view b) { return a == b; }

    uint32_t pack(Col c) noexcept
    {
        return static_cast<uint32_t>(c.r) | (static_cast<uint32_t>(c.g) << 8) | (static_cast<uint32_t>(c.b) << 16)
             | (static_cast<uint32_t>(c.a) << 24);
    }

    bool isTextPrim(const Prim& p) noexcept { return static_cast<int>(p.d2[2] + 0.5f) == 1; }

    int countTag(const PrimList& l, funkgui::Tag t)
    {
        int n = 0;
        for (const Prim& p : l.prims)
            n += p.tag == t ? 1 : 0;
        return n;
    }

    // Text prims of a tag in a colour (and, with weight >= 0, of that SDF weight).
    int countGlyphs(const PrimList& l, funkgui::Tag t, Col colour, float weight = -1.0f)
    {
        int n = 0;
        for (const Prim& p : l.prims)
            if (p.tag == t && isTextPrim(p) && p.c0 == pack(colour)
                && (weight < 0.0f || std::fabs(p.d1[0] - weight) < 1.0e-5f))
                ++n;
        return n;
    }

    bool allInside(const PrimList& l, const Rect& r)
    {
        for (const Prim& p : l.prims)
            if (p.x0 < r.x - 0.01f || p.y0 < r.y - 0.01f || p.x1 > r.right() + 0.01f || p.y1 > r.bottom() + 0.01f)
                return false;
        return true;
    }

    bool sameFingerprint(const funkgui::Fingerprint& a, const funkgui::Fingerprint& b)
    {
        return a.geometry == b.geometry && a.text == b.text && a.statics == b.statics && a.texts == b.texts
            && a.tagCounts == b.tagCounts;
    }

    std::string spansOf(const funkgui::text::Highlighter& h, std::string_view line, uint32_t state,
                        uint32_t* out = nullptr)
    {
        std::vector<Span> spans;
        const uint32_t next = h.line(line, state, spans);
        if (out != nullptr)
            *out = next;
        std::string s;
        for (const Span& sp : spans)
        {
            if (!s.empty())
                s += ' ';
            s += std::to_string(sp.start) + "-" + std::to_string(sp.end) + ":" + std::to_string(sp.style);
        }
        return s;
    }

    // ---- the text rule ----------------------------------------------------------------------------------------------

    void textRule(T::Probe& P)
    {
        TextEditor e;
        P.eq("text.empty_lines", e.lineCount(), 1);
        P.eq("text.empty_text", is(e.text(), ""), 1);
        e.setText("a\r\nb\rc\td\xC3\xA9\n");
        P.eq("text.lines", e.lineCount(), 4);                    // a, b, c<tab>d?, and the empty last line
        P.eq("text.crlf", is(e.line(0), "a") && is(e.line(1), "b"), 1);
        P.eq("text.tab_kept", is(e.line(2), "c\td?"), 1);
        P.eq("text.joined", is(e.text(), "a\nb\nc\td?\n"), 1);
        P.eq("text.line_outside", is(e.line(9), "") && is(e.line(-1), ""), 1);
        P.eq("text.endpos", is(pos(e.endPos()), "3:0"), 1);
        P.eq("text.caret_at_start", is(pos(e.caret()), "0:0") && !e.hasSelection(), 1);
        P.eq("text.not_modified", e.modified(), 0);
        const uint32_t rev = e.revision();
        e.setText("x");
        P.eq("text.revision_bumps", e.revision() > rev, 1);
        P.eq("text.sanitise", is(TextEditor::sanitise("q\r\n\xE2\x98\x83\t"), "q\n?\t"), 1);
        P.eq("text.insert_sanitised", e.insert("\xC3\xA9\r\nz") && is(e.text(), "?\nzx"), 1);
        P.eq("text.insert_caret", is(pos(e.caret()), "1:1"), 1);
        P.eq("text.modified", e.modified(), 1);
        e.clearModified();
        P.eq("text.cleared", e.modified(), 0);
    }

    // ---- typing -----------------------------------------------------------------------------------------------------

    void typing(T::Probe& P)
    {
        Fixture f;
        TextEditor& e = f.ed();
        P.eq("type.unfocused_refuses", f.panel.key(KeyEvent{ Key::character, {}, U'a' }), 0);
        P.eq("type.unfocused_text", is(e.text(), ""), 1);
        f.clickAt({ 0, 0 });
        P.eq("type.click_focuses", e.focused(), 1);
        f.host.keys("H,e,l,l,o,space,W");
        P.eq("type.text", is(e.text(), "Hello W"), 1);
        P.eq("type.caret", is(pos(e.caret()), "0:7"), 1);
        f.host.keys("return,x");
        P.eq("type.return", e.lineCount() == 2 && is(e.line(1), "x"), 1);
        P.eq("type.ctrl_char_refused", f.panel.key(KeyEvent{ Key::character, Mods{ false, false, false, true }, U'q' }),
             0);
        P.eq("type.cmd_return_refused", f.panel.key(KeyEvent{ Key::enter, Mods{ false, true, false, false }, 0 }), 0);
        P.eq("type.escape_refused", f.panel.key(KeyEvent{ Key::escape, {}, 0 }), 0);
        P.eq("type.unknown_chord_refused",
             f.panel.key(KeyEvent{ Key::character, Mods{ false, true, false, false }, U'q' }), 0);
        P.eq("type.after_refusals", is(e.text(), "Hello W\nx"), 1);

        // Auto-indent copies the line's leading blanks; without it Return breaks the line plainly.
        e.setText("    if x:");
        e.setFocused(true);
        e.setCaret(e.endPos());
        f.host.keys("return,y");
        P.eq("type.autoindent", is(e.line(1), "    y"), 1);
        e.setText("\tz");
        e.setCaret({ 0, 1 });                                  // between the tab and the z
        f.host.keys("return");
        P.eq("type.autoindent_up_to_caret", is(e.line(0), "\t") && is(e.line(1), "\tz"), 1);
        TextEditorOptions o = e.options();
        o.autoIndent = false;
        e.setOptions(o);
        e.setText("  a");
        e.setCaret(e.endPos());
        f.host.keys("return");
        P.eq("type.no_autoindent", is(e.line(1), ""), 1);

        // Tabs: a tab by default, spaces to the next stop with softTabs.
        e.setText("ab");
        e.setCaret(e.endPos());
        f.host.keys("tab,c");
        P.eq("type.tab", is(e.text(), "ab\tc"), 1);
        o.softTabs = true;
        e.setOptions(o);
        e.setText("ab");
        e.setCaret(e.endPos());
        f.host.keys("tab,c");
        P.eq("type.soft_tab", is(e.text(), "ab  c"), 1);
        e.setText("");
        f.host.keys("tab");
        P.eq("type.soft_tab_full", is(e.text(), "    "), 1);
        f.host.keys("backspace");
        P.eq("type.soft_backspace_to_stop", is(e.text(), ""), 1);
        e.setText("     ");                                   // five spaces: Backspace takes one, to the stop at 4
        e.setCaret(e.endPos());
        f.host.keys("backspace");
        P.eq("type.soft_backspace_one", is(e.text(), "    "), 1);
        e.setText("  x  ");                                   // not all blanks before the caret: one character
        e.setCaret(e.endPos());
        f.host.keys("backspace");
        P.eq("type.soft_backspace_after_text", is(e.text(), "  x "), 1);
    }

    // ---- movement ---------------------------------------------------------------------------------------------------

    void movement(T::Probe& P)
    {
        Fixture f(true);                                        // macOS convention
        TextEditor& e = f.ed();
        e.setText("one two\n  three four\nfive");
        e.setFocused(true);
        e.setCaret({ 0, 3 });
        f.host.keys("left");
        P.eq("move.left", is(pos(e.caret()), "0:2"), 1);
        f.host.keys("right,right");
        P.eq("move.right", is(pos(e.caret()), "0:4"), 1);
        e.setCaret({ 1, 0 });
        f.host.keys("left");
        P.eq("move.left_joins", is(pos(e.caret()), "0:7"), 1);
        f.host.keys("right");
        P.eq("move.right_joins", is(pos(e.caret()), "1:0"), 1);
        e.setCaret({ 0, 0 });
        f.host.keys("left");
        P.eq("move.left_at_start", is(pos(e.caret()), "0:0"), 1);
        e.setCaret(e.endPos());
        f.host.keys("right");
        P.eq("move.right_at_end", is(pos(e.caret()), "2:4"), 1);

        e.setCaret({ 0, 7 });
        f.host.keys("alt+left");
        P.eq("move.word_left", is(pos(e.caret()), "0:4"), 1);
        f.host.keys("alt+left");
        P.eq("move.word_left_2", is(pos(e.caret()), "0:0"), 1);
        f.host.keys("alt+right");
        P.eq("move.word_right", is(pos(e.caret()), "0:3"), 1);
        f.host.keys("alt+right");
        P.eq("move.word_right_skips_blank", is(pos(e.caret()), "0:7"), 1);
        f.host.keys("alt+right");
        P.eq("move.word_right_joins", is(pos(e.caret()), "1:0"), 1);
        f.host.keys("alt+left");
        P.eq("move.word_left_joins", is(pos(e.caret()), "0:7"), 1);

        e.setCaret({ 1, 5 });
        f.host.keys("cmd+left");
        P.eq("move.cmd_left_row_start", is(pos(e.caret()), "1:0"), 1);
        f.host.keys("cmd+right");
        P.eq("move.cmd_right_row_end", is(pos(e.caret()), "1:12"), 1);
        f.host.keys("home");
        P.eq("move.home_first_nonblank", is(pos(e.caret()), "1:2"), 1);
        f.host.keys("home");
        P.eq("move.home_toggles_to_0", is(pos(e.caret()), "1:0"), 1);
        f.host.keys("home");
        P.eq("move.home_toggles_back", is(pos(e.caret()), "1:2"), 1);
        f.host.keys("end");
        P.eq("move.end", is(pos(e.caret()), "1:12"), 1);
        e.setText("   ");
        f.host.keys("home");
        P.eq("move.home_all_blank", is(pos(e.caret()), "0:0"), 1);

        e.setText("one two\n  three four\nfive");
        e.setCaret({ 0, 7 });
        f.host.keys("down");
        P.eq("move.down_keeps_x", is(pos(e.caret()), "1:7"), 1);
        f.host.keys("down");
        P.eq("move.down_clamps_short_line", is(pos(e.caret()), "2:4"), 1);
        f.host.keys("up");
        P.eq("move.up_restores_goal", is(pos(e.caret()), "1:7"), 1);
        f.host.keys("up");
        P.eq("move.up_goal_2", is(pos(e.caret()), "0:7"), 1);
        f.host.keys("up");
        P.eq("move.up_first_row_to_start", is(pos(e.caret()), "0:0"), 1);
        e.setCaret({ 2, 1 });
        f.host.keys("down");
        P.eq("move.down_last_row_to_end", is(pos(e.caret()), "2:4"), 1);
        e.setCaret({ 1, 6 });
        f.host.keys("left,down");
        P.eq("move.horizontal_resets_goal", is(pos(e.caret()), "2:4"), 1);
        f.host.keys("cmd+up");
        P.eq("move.cmd_up_doc_start", is(pos(e.caret()), "0:0"), 1);
        f.host.keys("cmd+down");
        P.eq("move.cmd_down_doc_end", is(pos(e.caret()), "2:4"), 1);
        f.host.keys("cmd+home");
        P.eq("move.cmd_home", is(pos(e.caret()), "0:0"), 1);
        f.host.keys("cmd+end");
        P.eq("move.cmd_end", is(pos(e.caret()), "2:4"), 1);

        // The page keys move by the rows in view (6 of 17 px in a 106 px text area) and scroll by the same.
        std::string many;
        for (int i = 0; i < 40; ++i)
            many += "line " + std::to_string(i) + "\n";
        e.setText(many);
        f.host.keys("pagedown");
        const int rowsInView = static_cast<int>(std::floor(e.textArea().h / e.rowHeight()));
        P.eq("move.rows_in_view", rowsInView, 6);
        P.eq("move.pagedown", is(pos(e.caret()), "6:0"), 1);
        P.near("move.pagedown_scroll", e.scroll().y, 6.0 * e.rowHeight(), 0.01);
        f.host.keys("pagedown,pageup");
        P.eq("move.pageup", is(pos(e.caret()), "6:0"), 1);
        f.host.keys("pageup,pageup");
        P.eq("move.pageup_clamps", is(pos(e.caret()), "0:0") && e.scroll().y == 0.0f, 1);

        // The other convention: Ctrl (Mods::cmd, and ctrl with it) moves by words; Home and End do the rows.
        Fixture g(false);
        TextEditor& h = g.ed();
        h.setText("one two three");
        h.setFocused(true);
        h.setCaret({ 0, 13 });
        g.host.keys("cmd+left");
        P.eq("move.ctrl_left_word", is(pos(h.caret()), "0:8"), 1);
        g.host.keys("ctrl+cmd+right");
        P.eq("move.ctrl_right_word", is(pos(h.caret()), "0:13"), 1);
        g.host.keys("alt+left");
        P.eq("move.alt_left_word_too", is(pos(h.caret()), "0:8"), 1);
        g.host.keys("home");
        P.eq("move.home_nonmeta", is(pos(h.caret()), "0:0"), 1);
        g.host.keys("end");
        P.eq("move.end_nonmeta", is(pos(h.caret()), "0:13"), 1);
        g.host.keys("cmd+home");
        P.eq("move.ctrl_home_doc", is(pos(h.caret()), "0:0"), 1);
    }

    // ---- the selection ----------------------------------------------------------------------------------------------

    void selection(T::Probe& P)
    {
        Fixture f;
        TextEditor& e = f.ed();
        e.setText("one two\nthree");
        e.setFocused(true);
        f.host.keys("shift+right,shift+right,shift+right");
        P.eq("sel.shift_right", is(e.selectedText(), "one"), 1);
        P.eq("sel.anchor_caret", is(pos(e.anchor()), "0:0") && is(pos(e.caret()), "0:3"), 1);
        f.host.keys("shift+alt+right");
        P.eq("sel.shift_word", is(e.selectedText(), "one two"), 1);
        f.host.keys("shift+down");
        P.eq("sel.shift_down", is(e.selectedText(), "one two\nthree"), 1);
        f.host.keys("left");
        P.eq("sel.left_collapses_to_start", !e.hasSelection() && is(pos(e.caret()), "0:0"), 1);
        f.host.keys("cmd+a");
        P.eq("sel.select_all", is(e.selectedText(), "one two\nthree"), 1);
        f.host.keys("right");
        P.eq("sel.right_collapses_to_end", !e.hasSelection() && is(pos(e.caret()), "1:5"), 1);
        f.host.keys("shift+up");
        P.eq("sel.shift_up", is(e.selectedText(), "wo\nthree"), 1);   // the goal column: 5 of "one two"
        f.host.keys("X");
        P.eq("sel.typing_replaces", is(e.text(), "one tX"), 1);
        P.eq("sel.typing_caret", is(pos(e.caret()), "0:6") && !e.hasSelection(), 1);
        f.host.keys("shift+left,shift+left");
        f.host.keys("backspace");
        P.eq("sel.backspace_removes", is(e.text(), "one "), 1);
        f.host.keys("shift+left,delete");
        P.eq("sel.delete_removes", is(e.text(), "one"), 1);

        // The pointer: a click, Shift-click, a drag, a double click (the word), a triple click and the gutter (the
        // line), and a drag after each extending by the same unit.
        e.setText("alpha beta gamma\ndelta epsilon\nzeta");
        f.clickAt({ 0, 6 });
        P.eq("sel.click", is(pos(e.caret()), "0:6") && !e.hasSelection(), 1);
        f.clickAt({ 1, 5 }, Mods{ true, false, false, false });
        P.eq("sel.shift_click", is(e.selectedText(), "beta gamma\ndelta"), 1);
        const Point a = f.pointOf({ 0, 2 }), b = f.pointOf({ 1, 3 });
        f.host.drag(a.x, a.y, b.x, b.y, 4);
        P.eq("sel.drag", is(e.selectedText(), "pha beta gamma\ndel"), 1);
        P.eq("sel.drag_caret_at_end", is(pos(e.caret()), "1:3"), 1);
        const Point w = f.pointOf({ 0, 7 });
        f.host.doubleClick(w.x, w.y);
        P.eq("sel.double_click_word", is(e.selectedText(), "beta"), 1);
        f.host.press(w.x, w.y);
        f.panel.pointerDown(PointerEvent{ w.x, w.y, {}, 2, false });
        const Point w2 = f.pointOf({ 0, 12 });
        f.host.moveTo(w2.x, w2.y, 2);
        P.eq("sel.word_drag", is(e.selectedText(), "beta gamma"), 1);
        f.host.release();
        const Point back = f.pointOf({ 0, 1 });
        f.panel.pointerDown(PointerEvent{ w.x, w.y, {}, 2, false });
        f.panel.pointerDrag(PointerEvent{ back.x, back.y, {}, 2, false });
        f.panel.pointerUp(PointerEvent{ back.x, back.y, {}, 2, false });
        P.eq("sel.word_drag_back", is(e.selectedText(), "alpha beta") && is(pos(e.caret()), "0:0"), 1);
        f.panel.pointerDown(PointerEvent{ w.x, w.y, {}, 3, false });
        P.eq("sel.triple_click_line", is(e.selectedText(), "alpha beta gamma\n"), 1);
        const Point l2 = f.pointOf({ 1, 2 });
        f.panel.pointerDrag(PointerEvent{ l2.x, l2.y, {}, 3, false });
        f.panel.pointerUp(PointerEvent{ l2.x, l2.y, {}, 3, false });
        P.eq("sel.line_drag", is(e.selectedText(), "alpha beta gamma\ndelta epsilon\n"), 1);
        const Point g = f.pointOf({ 2, 0 });
        f.host.click(e.bounds().x + 4.0f, g.y);                 // the gutter
        P.eq("sel.gutter_click_line", is(e.selectedText(), "zeta"), 1);
        f.host.click(e.bounds().x + 4.0f, f.pointOf({ 1, 0 }).y);
        P.eq("sel.gutter_click_middle_line", is(e.selectedText(), "delta epsilon\n"), 1);
        f.host.click(w.x, w.y, Mods{ false, false, false, true });   // a popup press: nothing changes
        P.eq("sel.popup_press_ignored", is(e.selectedText(), "delta epsilon\n"), 1);
        f.host.click(2.0f, 2.0f);                               // outside the box
        P.eq("sel.outside_press_ignored", is(e.selectedText(), "delta epsilon\n") && e.focused(), 1);

        // The double click on blanks and on punctuation selects that run; on an empty line nothing.
        e.setText("a   b.,;c\n");
        e.doubleClick(PointerEvent{ f.pointOf({ 0, 2 }).x, f.pointOf({ 0, 2 }).y, {}, 2, false });
        P.eq("sel.double_click_blanks", is(e.selectedText(), "   "), 1);
        e.doubleClick(PointerEvent{ f.pointOf({ 0, 6 }).x, f.pointOf({ 0, 6 }).y, {}, 2, false });
        P.eq("sel.double_click_punct", is(e.selectedText(), ".,;"), 1);
        e.doubleClick(PointerEvent{ f.pointOf({ 1, 0 }).x, f.pointOf({ 1, 0 }).y, {}, 2, false });
        P.eq("sel.double_click_empty", !e.hasSelection() && is(pos(e.caret()), "1:0"), 1);

        // Programmatic selection, clamped.
        e.select({ 0, 1 }, { 9, 9 });
        P.eq("sel.select_clamped", is(pos(e.anchor()), "0:1") && is(pos(e.caret()), "1:0"), 1);
        e.setCaret({ 0, 99 });
        P.eq("sel.caret_clamped", is(pos(e.caret()), "0:9"), 1);
        P.eq("sel.start_end", is(pos(e.selectionStart()), "0:9") && is(pos(e.selectionEnd()), "0:9"), 1);
    }

    // ---- deleting ---------------------------------------------------------------------------------------------------

    void deleting(T::Probe& P)
    {
        Fixture f;
        TextEditor& e = f.ed();
        e.setText("one two\nthree");
        e.setFocused(true);
        e.setCaret({ 1, 0 });
        f.host.keys("backspace");
        P.eq("del.backspace_joins", is(e.text(), "one twothree") && is(pos(e.caret()), "0:7"), 1);
        f.host.keys("delete");
        P.eq("del.delete_char", is(e.text(), "one twohree"), 1);
        e.setCaret({ 0, 0 });
        P.eq("del.backspace_at_start_consumed", f.panel.key(KeyEvent{ Key::backspace, {}, 0 }), 1);
        P.eq("del.backspace_at_start_nothing", is(e.text(), "one twohree"), 1);
        e.setCaret(e.endPos());
        P.eq("del.delete_at_end_consumed", f.panel.key(KeyEvent{ Key::del, {}, 0 }), 1);
        P.eq("del.delete_at_end_nothing", is(e.text(), "one twohree"), 1);

        e.setText("one two  three");
        e.setCaret({ 0, 9 });                                  // after "two  "
        f.host.keys("alt+backspace");
        P.eq("del.word_backspace", is(e.text(), "one three"), 1);
        f.host.keys("alt+delete");
        P.eq("del.word_delete", is(e.text(), "one "), 1);
        e.setText("one two three");
        e.setCaret({ 0, 7 });
        f.host.keys("cmd+backspace");
        P.eq("del.cmd_backspace_row_start", is(e.text(), " three"), 1);
        e.setCaret({ 0, 1 });
        f.host.keys("cmd+delete");
        P.eq("del.cmd_delete_row_end", is(e.text(), " "), 1);
        e.setText("a\nb");
        e.setCaret({ 0, 1 });
        f.host.keys("delete");
        P.eq("del.delete_joins", is(e.text(), "ab"), 1);
        e.setText("a\nb");
        e.setCaret({ 1, 0 });
        f.host.keys("cmd+backspace");
        P.eq("del.cmd_backspace_at_row_start_joins", is(e.text(), "ab"), 1);
        e.setText("a\nb");
        e.setCaret({ 0, 1 });
        f.host.keys("cmd+delete");
        P.eq("del.cmd_delete_at_row_end_joins", is(e.text(), "ab"), 1);

        Fixture g(false);
        TextEditor& h = g.ed();
        h.setText("one two");
        h.setFocused(true);
        h.setCaret(h.endPos());
        g.host.keys("ctrl+cmd+backspace");
        P.eq("del.ctrl_backspace_word", is(h.text(), "one "), 1);
    }

    // ---- indent and outdent -----------------------------------------------------------------------------------------

    void indenting(T::Probe& P)
    {
        Fixture f;
        TextEditor& e = f.ed();
        e.setText("a\nb\n\nc");
        e.setFocused(true);
        e.select({ 0, 1 }, { 3, 1 });
        f.host.keys("tab");
        P.eq("indent.tab_indents_lines", is(e.text(), "\ta\n\tb\n\n\tc"), 1);
        P.eq("indent.selection_kept", is(pos(e.anchor()), "0:2") && is(pos(e.caret()), "3:2"), 1);
        f.host.keys("shift+tab");
        P.eq("indent.shift_tab_outdents", is(e.text(), "a\nb\n\nc"), 1);
        P.eq("indent.selection_kept_2", is(pos(e.anchor()), "0:1") && is(pos(e.caret()), "3:1"), 1);
        P.eq("indent.outdent_nothing_false",
             f.panel.key(KeyEvent{ Key::tab, Mods{ true, false, false, false }, 0 }), 0);
        e.select({ 0, 0 }, { 1, 0 });                          // ends at a line's start: that line is not included
        f.host.keys("tab");
        P.eq("indent.excludes_line_at_end", is(e.text(), "\ta\nb\n\nc"), 1);
        e.setCaret({ 1, 1 });
        f.host.keys("shift+tab");
        P.eq("indent.outdent_without_tab_false", is(e.text(), "\ta\nb\n\nc"), 1);
        e.setCaret({ 0, 2 });
        f.host.keys("shift+tab");
        P.eq("indent.outdent_single_line", is(e.text(), "a\nb\n\nc") && is(pos(e.caret()), "0:1"), 1);

        TextEditorOptions o = e.options();
        o.softTabs = true;
        o.tabWidth = 2;
        e.setOptions(o);
        e.setText("  a\n   b\nc");
        e.select({ 0, 0 }, { 2, 1 });
        f.host.keys("shift+tab");
        P.eq("indent.soft_outdent", is(e.text(), "a\n b\nc"), 1);
        f.host.keys("tab");
        P.eq("indent.soft_indent", is(e.text(), "  a\n   b\n  c"), 1);
        e.setCaret({ 2, 0 });
        f.host.keys("tab");
        P.eq("indent.single_line_tab_types", is(e.text(), "  a\n   b\n    c"), 1);
    }

    // ---- undo and redo ----------------------------------------------------------------------------------------------

    void undoRedo(T::Probe& P)
    {
        Fixture f;
        TextEditor& e = f.ed();
        e.setFocused(true);
        P.eq("undo.nothing", !e.canUndo() && !e.undo() && !e.redo(), 1);
        f.host.keys("a,b,c");
        P.eq("undo.coalesced_typing", e.canUndo() && e.undo() && is(e.text(), "") && !e.canUndo(), 1);
        P.eq("undo.caret_restored", is(pos(e.caret()), "0:0"), 1);
        P.eq("undo.redo", e.redo() && is(e.text(), "abc") && is(pos(e.caret()), "0:3"), 1);
        f.host.keys("return,d");
        P.eq("undo.return_new_step", e.undo() && is(e.text(), "abc\n"), 1);
        P.eq("undo.return_step_2", e.undo() && is(e.text(), "abc"), 1);
        P.eq("undo.redo_twice", e.redo() && e.redo() && is(e.text(), "abc\nd"), 1);
        f.host.keys("backspace,backspace,backspace");           // d, the line break, c: the break is its own step
        P.eq("undo.backspaces_text", is(e.text(), "ab"), 1);
        P.eq("undo.backspace_step_c", e.undo() && is(e.text(), "abc"), 1);
        P.eq("undo.backspace_step_break", e.undo() && is(e.text(), "abc\n"), 1);
        P.eq("undo.backspace_step_d", e.undo() && is(e.text(), "abc\nd"), 1);
        e.setCaret({ 1, 1 });
        f.host.keys("backspace");
        e.setCaret({ 0, 3 });
        f.host.keys("backspace,backspace");
        P.eq("undo.coalesced_backspaces", is(e.text(), "a\n") && e.undo() && is(e.text(), "abc\n"), 1);
        P.eq("undo.coalesced_backspaces_2", e.undo() && is(e.text(), "abc\nd"), 1);
        e.setCaret({ 0, 0 });
        f.host.keys("delete,delete");
        P.eq("undo.coalesced_deletes", is(e.text(), "c\nd") && e.undo() && is(e.text(), "abc\nd"), 1);
        f.host.keys("x");
        P.eq("undo.edit_clears_redo", !e.canRedo(), 1);
        f.host.keys("cmd+z");
        P.eq("undo.cmd_z", is(e.text(), "abc\nd"), 1);
        f.host.keys("cmd+shift+z");
        P.eq("undo.cmd_shift_z", is(e.text(), "xabc\nd"), 1);
        f.host.keys("cmd+z,cmd+y");
        P.eq("undo.cmd_y", is(e.text(), "xabc\nd"), 1);
        f.host.keys("cmd+a,Q");
        P.eq("undo.replace_selection_step", is(e.text(), "Q") && e.undo() && is(e.text(), "xabc\nd"), 1);
        P.eq("undo.replace_selection_restored", is(e.selectedText(), "xabc\nd"), 1);
        f.host.keys("left,a,b");
        e.setCaret({ 0, 0 });
        f.host.keys("c");
        P.eq("undo.typing_elsewhere_new_step", e.undo() && is(e.text(), "abxabc\nd"), 1);
        P.eq("undo.typing_elsewhere_step_2", e.undo() && is(e.text(), "xabc\nd"), 1);
        e.insert("paste");
        f.host.keys("z");
        P.eq("undo.paste_then_typing",
             e.undo() && is(e.text(), "pastexabc\nd") && e.undo() && is(e.text(), "xabc\nd"), 1);
        e.select({ 0, 0 }, { 1, 1 });
        f.host.keys("tab");
        P.eq("undo.indent_one_step", e.undo() && is(e.text(), "xabc\nd") && is(e.selectedText(), "xabc\nd"), 1);
        e.setText("fresh");
        P.eq("undo.settext_clears", !e.canUndo() && !e.canRedo(), 1);

        TextEditorOptions o = e.options();
        o.maxUndo = 2;
        e.setOptions(o);
        e.setText("");
        f.host.keys("a,return,b,return,c");
        P.eq("undo.cap", e.undo() && e.undo() && !e.undo() && is(e.text(), "a\nb"), 1);
        o.maxUndo = 0;
        e.setOptions(o);
        f.host.keys("z");
        P.eq("undo.cap_zero", !e.canUndo(), 1);
    }

    // ---- the clipboard ----------------------------------------------------------------------------------------------

    void clipboard(T::Probe& P)
    {
        Fixture f;
        TextEditor& e = f.ed();
        e.setText("one two\nthree");
        e.setFocused(true);
        P.eq("clip.copy_nothing", !e.copy() && f.host.log.copies == 0, 1);
        e.select({ 0, 4 }, { 1, 2 });
        f.host.keys("cmd+c");
        P.eq("clip.copy_host", f.host.log.copies == 1 && is(f.host.log.lastCopy, "two\nth"), 1);
        P.eq("clip.copy_keeps_text", is(e.text(), "one two\nthree") && e.hasSelection(), 1);
        f.host.keys("cmd+x");
        P.eq("clip.cut", f.host.log.copies == 2 && is(e.text(), "one ree") && is(pos(e.caret()), "0:4"), 1);
        f.host.keys("cmd+v");
        P.eq("clip.paste", is(e.text(), "one two\nthree") && is(pos(e.caret()), "1:2"), 1);
        f.host.setClipboard("X\r\nY\xC3\xA9");
        f.host.keys("cmd+a,cmd+v");
        P.eq("clip.paste_sanitised_replaces", is(e.text(), "X\nY?"), 1);
        f.host.setClipboard("");                                 // the host reads empty: the editor's own copy
        P.eq("clip.paste_own_when_host_empty", e.paste() && is(e.text(), "X\nY?two\nth"), 1);
        e.selectAll();
        P.eq("clip.cut_undo", e.cut() && is(e.text(), "") && e.undo() && is(e.text(), "X\nY?two\nth"), 1);
        P.eq("clip.cut_copied", is(f.host.log.lastCopy, "X\nY?two\nth"), 1);

        TextEditor own;                                         // no host: its own clipboard serves paste
        own.setText("ab");
        own.selectAll();
        P.eq("clip.own_copy", own.copy(), 1);
        own.setCaret(own.endPos());
        P.eq("clip.own_paste", own.paste() && is(own.text(), "abab"), 1);
        TextEditor none;
        P.eq("clip.own_paste_nothing", !none.paste(), 1);
    }

    // ---- find -------------------------------------------------------------------------------------------------------

    void finding(T::Probe& P)
    {
        TextEditor e;
        e.setText("cat dog\nCat bird cat\ndog");
        P.eq("find.first", e.find("cat") && is(pos(e.selectionStart()), "0:0") && is(e.selectedText(), "cat"), 1);
        P.eq("find.next_skips_current", e.find("cat") && is(pos(e.selectionStart()), "1:9"), 1);
        P.eq("find.wraps", e.find("cat") && is(pos(e.selectionStart()), "0:0"), 1);
        P.eq("find.case_insensitive", e.find("cat", true, false) && is(pos(e.selectionStart()), "1:0"), 1);
        P.eq("find.backward", e.find("cat", false) && is(pos(e.selectionStart()), "0:0"), 1);
        P.eq("find.backward_wraps", e.find("cat", false) && is(pos(e.selectionStart()), "1:9"), 1);
        e.setCaret({ 2, 0 });
        P.eq("find.no_wrap_false", !e.find("cat", true, true, false) && is(pos(e.caret()), "2:0"), 1);
        P.eq("find.missing", !e.find("zebra"), 1);
        P.eq("find.empty", !e.find(""), 1);
        P.eq("find.multiline_refused", !e.find("dog\nCat"), 1);
        e.setCaret({ 0, 0 });
        P.eq("find.backward_no_wrap", !e.find("dog", false, true, false), 1);
        P.eq("find.last_line", e.find("dog") && is(pos(e.selectionStart()), "0:4"), 1);
        P.eq("find.last_line_2", e.find("dog") && is(pos(e.selectionStart()), "2:0"), 1);
    }

    // ---- word wrap --------------------------------------------------------------------------------------------------

    void wrapping(T::Probe& P)
    {
        Fixture f;
        TextEditor& e = f.ed();
        TextEditorOptions o = e.options();
        o.wordWrap = true;
        o.lineNumbers = false;
        e.setOptions(o);
        e.setBounds({ 10.0f, 10.0f, 120.0f, 120.0f });          // 106 px of text: 19 characters of 5.45 px
        const float cw = e.textArea().w;
        P.near("wrap.text_width", cw, 106.0, 0.01);
        e.setText("the quick brown fox jumps over\nab");
        P.eq("wrap.rows", e.rowCount(), 3);                      // "the quick brown ", "fox jumps over", "ab"
        P.eq("wrap.row_of", e.rowOf({ 0, 15 }) == 0 && e.rowOf({ 0, 16 }) == 1 && e.rowOf({ 1, 0 }) == 2, 1);
        P.eq("wrap.no_hbar", e.contentWidth() <= cw, 1);
        e.setFocused(true);
        e.setCaret({ 0, 4 });
        f.host.keys("down");
        P.eq("wrap.down_within_line", is(pos(e.caret()), "0:20"), 1);
        f.host.keys("down");
        P.eq("wrap.down_to_next_line", is(pos(e.caret()), "1:2"), 1);
        f.host.keys("up,up");
        P.eq("wrap.up_keeps_goal", is(pos(e.caret()), "0:4"), 1);
        f.host.keys("end");
        P.eq("wrap.end_of_row", is(pos(e.caret()), "0:16"), 1);
        const Point cp = e.caretPoint();
        P.near("wrap.end_caret_on_first_row", cp.y, e.textArea().y, 0.01);
        f.host.keys("home");
        P.eq("wrap.home_of_row", is(pos(e.caret()), "0:0"), 1);
        f.host.keys("down,home");
        P.eq("wrap.home_second_row", is(pos(e.caret()), "0:16"), 1);
        P.near("wrap.home_caret_on_second_row", e.caretPoint().y, e.textArea().y + e.rowHeight(), 0.01);
        f.host.keys("cmd+right");
        P.eq("wrap.cmd_right_row_end", is(pos(e.caret()), "0:30"), 1);
        const Point q = f.pointOf({ 0, 20 });
        P.eq("wrap.hit_test", is(pos(e.positionAt(q)), "0:20"), 1);
        f.host.click(e.textArea().right() - 1.0f, e.textArea().y + 2.0f);   // past the first row's end
        P.eq("wrap.click_past_row_end", is(pos(e.caret()), "0:16"), 1);
        P.near("wrap.click_past_row_end_stays", e.caretPoint().y, e.textArea().y, 0.01);
        e.setText("abcdefghijklmnopqrstuvwxyz0123456789");
        P.eq("wrap.breaks_long_word", e.rowCount(), 2);
        e.setText("a b");
        P.eq("wrap.short_line_one_row", e.rowCount(), 1);
        e.setBounds({ 10.0f, 10.0f, 300.0f, 120.0f });
        e.setText("the quick brown fox jumps over\nab");
        P.eq("wrap.wider_box_fewer_rows", e.rowCount(), 2);
        o.wordWrap = false;
        e.setOptions(o);
        e.setBounds({ 10.0f, 10.0f, 120.0f, 120.0f });
        P.eq("wrap.off_rows_are_lines", e.rowCount(), 2);
        P.eq("wrap.off_hbar", e.contentWidth() > e.textArea().w, 1);
    }

    // ---- scrolling --------------------------------------------------------------------------------------------------

    void scrolling(T::Probe& P)
    {
        Fixture f;
        TextEditor& e = f.ed();
        std::string many;
        for (int i = 0; i < 50; ++i)
            many += "line " + std::to_string(i)
                  + (i == 20 ? " is a very long line that runs past the box's right edge" : "") + "\n";
        e.setText(many);
        const float rowH = e.rowHeight();
        P.near("scroll.row_height", rowH, 17.0, 0.0);
        P.near("scroll.content_height", e.contentHeight(), 51.0 * 17.0, 0.01);
        P.eq("scroll.at_top", e.scroll().y == 0.0f && e.scroll().x == 0.0f, 1);
        const float x = kBox.centreX(), y = kBox.centreY();
        P.eq("scroll.wheel_consumed",
             f.panel.wheel(WheelEvent{ x, y, 0.0f, -50.0f / 256.0f, false, false, false, {} }), 1);
        P.near("scroll.wheel_notch_three_rows", e.scroll().y, 3.0 * rowH, 0.01);
        f.panel.wheel(WheelEvent{ x, y, 0.0f, 0.5f, true, false, false, {} });   // smooth up: 256 px, clamped at 0
        P.eq("scroll.wheel_clamps_top", e.scroll().y == 0.0f, 1);
        f.panel.wheel(WheelEvent{ x, y, 0.0f, -100.0f, false, false, false, {} });
        P.near("scroll.wheel_clamps_bottom", e.scroll().y, e.contentHeight() - e.textArea().h, 0.01);
        P.eq("scroll.wheel_outside_false",
             f.panel.wheel(WheelEvent{ 2.0f, 2.0f, 0.0f, 1.0f, false, false, false, {} }), 0);
        f.panel.wheel(WheelEvent{ x, y, -50.0f / 256.0f, 0.0f, false, false, false, {} });
        P.near("scroll.wheel_dx", e.scroll().x, 3.0 * rowH, 0.01);
        e.setScroll(-5.0f, 1.0e9f);
        P.eq("scroll.set_clamped", e.scroll().x == 0.0f && e.scroll().y == e.contentHeight() - e.textArea().h, 1);
        e.setScroll(0.0f, 0.0f);
        e.setFocused(true);
        e.setCaret({ 30, 0 });
        P.near("scroll.caret_into_view", e.scroll().y, 31.0 * rowH - e.textArea().h, 0.01);
        e.setCaret({ 0, 0 });
        P.eq("scroll.caret_back_to_top", e.scroll().y == 0.0f, 1);
        e.setCaret({ 20, 60 });
        P.eq("scroll.caret_scrolls_x", e.scroll().x > 0.0f, 1);
        const Point cp = e.caretPoint();
        P.eq("scroll.caret_point_in_view", cp.x >= e.textArea().x && cp.x + 1.5f <= e.textArea().right() + 0.01f, 1);
        e.setCaret({ 20, 0 });
        P.eq("scroll.caret_col0_resets_x", e.scroll().x == 0.0f, 1);

        // What is drawn: the rows in view only, within the box, and both scrollbars; a thumb drag moves the view.
        e.setScroll(0.0f, 0.0f);
        e.setFocused(false);
        const PrimList& l = f.host.draw();
        P.eq("scroll.clipped", allInside(l, kBox.expanded(2.0f)), 1);
        P.eq("scroll.only_visible_rows",
             countTag(l, tags::editGutter) <= 8 * 2 && countTag(l, tags::editGutter) >= 7, 1);
        P.eq("scroll.bars_drawn", countTag(l, tags::editFrame), 4);   // the frame, the gutter rule, two thumbs
        P.eq("scroll.no_missing_glyphs", l.missingGlyphs, 0);
        e.setText("short");
        P.eq("scroll.no_bars_when_fits", countTag(f.host.draw(), tags::editFrame), 2);
        e.setText(many);
        const Rect inner = kBox.reduced(1.0f);
        const float barX = inner.right() - 4.0f;               // the vertical bar's centre
        f.host.press(barX, e.textArea().y + 2.0f);              // on the thumb at the top
        f.host.moveTo(barX, e.textArea().y + 52.0f, 2);
        P.eq("scroll.thumb_drag", e.scroll().y > 0.0f && e.scroll().y < e.contentHeight(), 1);
        const float dragged = e.scroll().y;
        f.host.release();
        f.host.click(barX, e.textArea().bottom() - 3.0f);       // on the track: the thumb jumps there
        P.eq("scroll.track_click_jumps", e.scroll().y > dragged, 1);
    }

    // ---- highlighting -----------------------------------------------------------------------------------------------

    void highlighting(T::Probe& P)
    {
        const KeywordHighlighter c(KeywordRules::cLike());
        uint32_t st = 0;
        P.eq("hl.types_keywords", is(spansOf(c, "int x = if(y);", 0, &st), "0-3:2 6-7:6 8-10:1"), 1);
        P.eq("hl.state_stays_0", st, 0);
        P.eq("hl.line_comment", is(spansOf(c, "x // y = 1", 0), "2-10:3"), 1);
        P.eq("hl.string_escape", is(spansOf(c, "f(\"a\\\"b\", 'c')", 0), "2-8:4 10-13:4"), 1);
        P.eq("hl.string_unterminated", is(spansOf(c, "s = \"abc", 0), "2-3:6 4-8:4"), 1);
        P.eq("hl.numbers", is(spansOf(c, "0x1F + 2.5e3 .5", 0), "0-4:5 5-6:6 7-12:5 13-15:5"), 1);
        P.eq("hl.ident_not_number", is(spansOf(c, "x2 a.b", 0), ""), 1);
        P.eq("hl.operators_merge", is(spansOf(c, "a -> b", 0), "2-4:6"), 1);
        P.eq("hl.preprocessor", is(spansOf(c, "  #include <x> // y", 0), "2-19:7"), 1);
        P.eq("hl.hash_after_code_plain", is(spansOf(c, "x # y", 0), ""), 1);
        constexpr uint32_t kIn = KeywordHighlighter::kInBlockComment;
        P.eq("hl.block_open", is(spansOf(c, "a /* b", 0, &st), "2-6:3") && st == kIn, 1);
        P.eq("hl.block_continues", is(spansOf(c, "if x", st, &st), "0-4:3") && st == kIn, 1);
        P.eq("hl.block_closes", is(spansOf(c, "b */ if", st, &st), "0-4:3 5-7:1") && st == 0, 1);
        P.eq("hl.block_one_line", is(spansOf(c, "/* a */ x /* b", 0, &st), "0-7:3 10-14:3") && st == 1, 1);
        P.eq("hl.block_close_only_text", is(spansOf(c, "*/", 0, &st), "0-2:6") && st == 0, 1);
        P.eq("hl.empty_line", is(spansOf(c, "", 1, &st), "") && st == 1, 1);
        P.eq("hl.whole_words", is(spansOf(c, "iffy ifs", 0), ""), 1);
        P.eq("hl.tabs_blank", is(spansOf(c, "\tif\t", 0), "1-3:1"), 1);

        const KeywordHighlighter s(KeywordRules::scriptLike());
        P.eq("hl.script_comment", is(spansOf(s, "x = 1 # y", 0), "2-3:6 4-5:5 6-9:3"), 1);
        P.eq("hl.script_no_block", is(spansOf(s, "/* a", 0, &st), "0-2:6") && st == 0, 1);
        P.eq("hl.script_keywords", is(spansOf(s, "def f(): return None", 0), "0-3:1 7-8:6 9-15:1 16-20:2"), 1);

        KeywordRules r;
        r.keywords = { "SELECT", "from" };
        r.caseSensitive = false;
        r.lineComment = "--";
        r.quotes = "'";
        r.operators = "";
        r.numbers = false;
        const KeywordHighlighter sql(r);
        P.eq("hl.case_insensitive", is(spansOf(sql, "select * FROM t -- 1", 0), "0-6:1 9-13:1 16-20:3"), 1);
        P.eq("hl.no_numbers_no_ops", is(spansOf(sql, "1 + 2 \"q\"", 0), ""), 1);
        P.eq("hl.quote_set", is(spansOf(sql, "'a' \"b\"", 0), "0-3:4"), 1);

        // In the editor: the palette's inks and weights on the glyphs, and the states kept across edits.
        Fixture f;
        TextEditor& e = f.ed();
        e.setHighlighter(std::make_unique<KeywordHighlighter>(KeywordRules::cLike()));
        e.setPalette(TextEditor::defaultPalette());
        const Theme th = Theme::graphite();
        const float w0 = e.options().style.weight;
        e.setText("if x");
        const PrimList& l0 = f.host.draw();
        P.eq("hl.keyword_glyphs_accent", countGlyphs(l0, tags::editText, th.accent, w0 + 0.03f), 2);
        P.eq("hl.plain_glyph_ink100", countGlyphs(l0, tags::editText, th.ink100, w0), 1);
        e.setText("/*\nreturn\n*/ x");
        const PrimList& l1 = f.host.draw();
        P.eq("hl.comment_glyphs_ink32", countGlyphs(l1, tags::editText, th.ink32), 2 + 6 + 2);
        P.eq("hl.no_keyword_in_comment", countGlyphs(l1, tags::editText, th.accent), 0);
        e.setCaret({ 0, 2 });
        e.setFocused(true);
        f.host.keys("backspace,backspace");                    // the comment never opened
        const PrimList& l2 = f.host.draw();
        P.eq("hl.reopened_keyword", countGlyphs(l2, tags::editText, th.accent), 6);
        P.eq("hl.close_without_open_ops", countGlyphs(l2, tags::editText, th.ink52), 2);
        e.setCaret({ 0, 0 });
        f.host.keys("/,*");
        const PrimList& l3 = f.host.draw();
        P.eq("hl.reclosed_comment",
             countGlyphs(l3, tags::editText, th.ink32) == 10 && countGlyphs(l3, tags::editText, th.accent) == 0, 1);
        e.setText("a\nb\nc\nreturn");
        e.setCaret({ 1, 1 });
        f.host.keys("return");                                 // a line inserted: the states shift with the lines
        const PrimList& l4 = f.host.draw();
        P.eq("hl.insert_line_keeps_states", countGlyphs(l4, tags::editText, th.accent), 6);
        e.setText("/* a\nb\nc */\nreturn");
        e.select({ 0, 0 }, { 2, 4 });
        f.host.keys("delete");                                 // lines removed: the keyword after them is plain again
        const PrimList& l5 = f.host.draw();
        P.eq("hl.remove_lines_keeps_states", countGlyphs(l5, tags::editText, th.accent), 6);
        P.eq("hl.remove_lines_text", is(e.text(), "\nreturn"), 1);

        // The palette: a custom colour with an underline, and an index past the palette drawn plain.
        std::vector<HighlightStyle> pal(2);
        pal[1].ink = HighlightStyle::Ink::custom;
        pal[1].colour = Col{ 1, 2, 3 };
        pal[1].underline = true;
        e.setPalette(pal);
        e.setText("if int");
        const PrimList& l6 = f.host.draw();
        P.eq("hl.custom_colour", countGlyphs(l6, tags::editText, Col{ 1, 2, 3 }), 2);
        P.eq("hl.past_palette_plain", countGlyphs(l6, tags::editText, th.ink100), 3);
        int underlines = 0;
        for (const Prim& p : l6.prims)
            underlines += p.tag == tags::editText && !isTextPrim(p) ? 1 : 0;
        P.eq("hl.underline", underlines, 1);
        e.setHighlighter(nullptr);
        const PrimList& l7 = f.host.draw();
        P.eq("hl.none_all_plain", countGlyphs(l7, tags::editText, th.ink100), 5);
        P.eq("hl.default_palette_size", static_cast<int>(TextEditor::defaultPalette().size()),
             static_cast<int>(KeywordHighlighter::count));
    }

    // ---- what is drawn ----------------------------------------------------------------------------------------------

    void drawing(T::Probe& P)
    {
        Fixture f;
        TextEditor& e = f.ed();
        e.setText("ab\ncd");
        const PrimList& l0 = f.host.draw();
        P.eq("draw.no_caret_unfocused", countTag(l0, tags::editCaret), 0);
        P.eq("draw.no_band_unfocused", countTag(l0, tags::editSelect), 0);
        P.eq("draw.gutter_numbers", countTag(l0, tags::editGutter), 2);
        P.eq("draw.text_glyphs", countTag(l0, tags::editText), 4);
        P.eq("draw.frame_and_rule", countTag(l0, tags::editFrame), 2);
        P.eq("draw.no_missing_glyphs", l0.missingGlyphs, 0);
        P.eq("draw.inside_box", allInside(l0, kBox.expanded(2.0f)), 1);
        const funkgui::Fingerprint fp0 = funkgui::fingerprint(l0);
        EditorPanel other;
        other.ed.setText("ab\ncd");
        funkgui::HeadlessHost paper(other, 1);
        const funkgui::Fingerprint fp1 = funkgui::fingerprint(paper.draw());
        P.eq("draw.theme_invariant", sameFingerprint(fp0, fp1), 1);

        e.setFocused(true);
        e.setCaret({ 1, 1 });
        const PrimList& l1 = f.host.draw();
        P.eq("draw.caret_focused", countTag(l1, tags::editCaret), 1);
        P.eq("draw.band_focused", countTag(l1, tags::editSelect), 1);
        const Point cp = e.caretPoint();
        P.near("draw.caret_x", cp.x, e.textArea().x + 21.8182 * 0.25, 0.01);
        P.near("draw.caret_y", cp.y, e.textArea().y + e.rowHeight(), 0.01);
        P.eq("draw.focus_ring", countTag(l1, tags::focusRing), 4);
        e.select({ 0, 1 }, { 1, 1 });
        const PrimList& l2 = f.host.draw();
        P.eq("draw.selection_rows", countTag(l2, tags::editSelect), 1 + 2);   // the band and one rect per row
        e.setText("");
        TextEditorOptions o = e.options();
        o.placeholder = "Type here";
        e.setOptions(o);
        const PrimList& l3 = f.host.draw();
        P.eq("draw.placeholder", countTag(l3, tags::editText), 8);
        e.setText("x");
        P.eq("draw.placeholder_gone", countTag(f.host.draw(), tags::editText), 1);
        o.lineNumbers = false;
        e.setOptions(o);
        P.eq("draw.no_gutter", countTag(f.host.draw(), tags::editGutter) == 0 && e.gutterWidth() == 0.0f, 1);
        P.near("draw.text_area_no_gutter", e.textArea().x, kBox.x + 1.0 + 6.0, 0.001);
        o.highlightLine = false;
        e.setOptions(o);
        P.eq("draw.no_band_option", countTag(f.host.draw(), tags::editSelect), 0);

        // The blink: on for the first half of the period, advanced by tick() alone, reset by a move.
        o.blinkCaret = true;
        o.blinkPeriod = 1.0f;
        e.setOptions(o);
        P.eq("draw.blink_on", countTag(f.host.draw(), tags::editCaret), 1);
        f.host.tick(36);                                        // 0.6 s
        P.eq("draw.blink_off", countTag(f.host.draw(), tags::editCaret), 0);
        f.host.tick(30);                                        // 1.1 s: wrapped
        P.eq("draw.blink_on_again", countTag(f.host.draw(), tags::editCaret), 1);
        f.host.tick(36);
        e.setCaret({ 0, 0 });
        P.eq("draw.blink_reset_by_move", countTag(f.host.draw(), tags::editCaret), 1);
        P.eq("draw.no_full_rate", e.wantsFullRate(), 0);
        e.setBounds({ 0.0f, 0.0f, 0.0f, 0.0f });
        P.eq("draw.empty_box_nothing", f.host.draw().prims.empty(), 1);
        e.setBounds({ 0.0f, 0.0f, 10.0f, 10.0f });
        P.eq("draw.tiny_box_frame_only", countTag(f.host.draw(), tags::editText), 0);

        // Scrolled sideways, what scrolls is cut at the gutter's edge: nothing runs over the line numbers.
        Fixture g;
        TextEditor& h = g.ed();
        h.setText("a line long enough to scroll the box sideways when the caret goes to its far end\nb");
        h.setFocused(true);
        h.select({ 0, 0 }, { 0, 82 });
        const PrimList& l4 = g.host.draw();
        const float gutterEdge = kBox.x + 1.0f + h.gutterWidth();
        bool clear = true;
        for (const Prim& p : l4.prims)
            if ((p.tag == tags::editText || p.tag == tags::editSelect || p.tag == tags::editCaret)
                && p.x0 < gutterEdge - 0.5f)
                clear = false;
        P.eq("draw.hscroll_scrolled", h.scroll().x > 0.0f, 1);
        P.eq("draw.hscroll_clear_of_gutter", clear, 1);
        P.eq("draw.hscroll_numbers_kept", countTag(l4, tags::editGutter), 2);

        // A tick of any length keeps the blink inside its period (1e9 s is a whole number of periods: caret on), and
        // a tick that is not finite is ignored.
        TextEditorOptions bo = h.options();
        bo.blinkCaret = true;
        h.setOptions(bo);
        h.tick(1.0e9f);
        h.tick(std::numeric_limits<float>::infinity());
        P.eq("draw.blink_long_tick", countTag(g.host.draw(), tags::editCaret), 1);
    }

    // ---- v0.22.0: the code-editor hooks -----------------------------------------------------------------------------

    int kindOf(const Prim& p) noexcept { return static_cast<int>(p.d2[2] + 0.5f); }   // 0 rrect, 1 text, 2 segment

    // An rrect's own box, the AA apron (1.5 px, no softness) taken off.
    Rect boxOf(const Prim& p) noexcept { return { p.x0 + 1.5f, p.y0 + 1.5f, p.x1 - p.x0 - 3.0f, p.y1 - p.y0 - 3.0f }; }

    std::vector<Prim> withTag(const PrimList& l, funkgui::Tag t)
    {
        std::vector<Prim> out;
        for (const Prim& p : l.prims)
            if (p.tag == t)
                out.push_back(p);
        return out;
    }

    std::string decs(const TextEditor& e)
    {
        std::string s;
        for (const Decoration& d : e.decorations())
        {
            if (!s.empty())
                s += ' ';
            s += std::to_string(static_cast<int>(d.kind)) + "@" + pos(d.from) + "-" + pos(d.to);
        }
        return s;
    }

    std::string marks(const TextEditor& e)
    {
        std::string s;
        for (const LineMark& m : e.lineMarks())
        {
            if (!s.empty())
                s += ' ';
            s += std::to_string(m.line) + ":" + std::to_string(static_cast<int>(m.kind));
        }
        return s;
    }

    // Types a string as keys arrive: a character each, a space as Key::space.
    void typeKeys(Fixture& f, std::string_view s)
    {
        for (const char ch : s)
            f.panel.key(ch == ' ' ? KeyEvent{ Key::space, {}, U' ' }
                                  : KeyEvent{ Key::character, {}, static_cast<char32_t>(ch) });
    }

    void setOption(TextEditor& e, bool TextEditorOptions::*field, bool on = true)
    {
        TextEditorOptions o = e.options();
        o.*field = on;
        e.setOptions(o);
    }

    void decorations(T::Probe& P)
    {
        Fixture f;
        TextEditor& e = f.ed();
        e.setText("alpha beta gamma\ndelta\nzeta");
        e.setDecorations({ { { 0, 6 }, { 0, 10 }, DK::error }, { { 1, 99 }, { 1, 0 }, DK::warning },
                           { { 0, 0 }, { 0, 5 }, DK::highlight } });
        P.eq("deco.set_sorted_clamped", is(decs(e), "3@0:0-0:5 0@0:6-0:10 1@1:0-1:5"), 1);
        e.setDecorations({ { { 2, 0 }, { 2, 4 }, DK::info }, { { 0, 0 }, { 0, 1 }, DK::findMatch } },
                         funkgui::kDiagnosticDecorations);
        P.eq("deco.mask_replaces_its_kinds", is(decs(e), "3@0:0-0:5 2@2:0-2:4"), 1);
        e.setDecorations({}, funkgui::decorationBit(DK::highlight));
        P.eq("deco.mask_clears", is(decs(e), "2@2:0-2:4"), 1);

        // They move with the text: typed inside widens, at either end does not; removed whole they go.
        e.setDecorations({ { { 0, 6 }, { 0, 10 }, DK::error } });
        e.setCaret({ 0, 8 });
        e.insert("X");
        P.eq("deco.typed_inside_widens", is(decs(e), "0@0:6-0:11"), 1);
        e.setCaret({ 0, 6 });
        e.insert("Y");
        P.eq("deco.typed_at_start_moves", is(decs(e), "0@0:7-0:12"), 1);
        e.setCaret({ 0, 12 });
        e.insert("Z");
        P.eq("deco.typed_at_end_stays", is(decs(e), "0@0:7-0:12"), 1);
        e.setCaret({ 0, 0 });
        e.insert("top\n");
        P.eq("deco.line_above_moves", is(decs(e), "0@1:7-1:12"), 1);
        e.undo();
        e.undo();
        e.undo();
        e.undo();
        P.eq("deco.undo_moves_back", is(decs(e), "0@0:6-0:10") && is(e.line(0), "alpha beta gamma"), 1);
        e.select({ 0, 5 }, { 0, 11 });
        e.deleteSelection();
        P.eq("deco.removed_text_drops", e.decorations().empty(), 1);
        e.setDecorations({ { { 1, 5 }, { 1, 5 }, DK::error } });
        e.setCaret({ 1, 0 });
        e.insert("ab");
        P.eq("deco.empty_moves_as_point", is(decs(e), "0@1:7-1:7"), 1);
        e.setText("x");
        P.eq("deco.settext_clears", e.decorations().empty(), 1);

        // Line marks: kept by line, a line outside the text dropped; they move with their line's start.
        e.setText("a\nb\nc");
        e.setLineMarks({ { 1, LineMark::Kind::warning }, { 9, LineMark::Kind::error }, { 1, LineMark::Kind::error },
                         { 0, LineMark::Kind::info } });
        P.eq("mark.set", is(marks(e), "0:0 1:1 1:2"), 1);
        e.setCaret({ 0, 0 });
        e.insert("z\n");
        P.eq("mark.moves_with_line", is(marks(e), "1:0 2:1 2:2"), 1);
        const Col errInk = e.options().inks.error.resolve(Theme::graphite());
        const Col infoInk = e.options().inks.info.resolve(Theme::graphite());
        const PrimList& l0 = f.host.draw();
        P.eq("mark.tints_number_most_severe", countGlyphs(l0, tags::editGutter, errInk), 1);   // "3"
        P.eq("mark.tints_number_info", countGlyphs(l0, tags::editGutter, infoInk), 1);         // "2"
        P.eq("mark.default_info_is_signal", pack(infoInk) == pack(Theme::graphite().signal), 1);
        const float gutterBefore = e.gutterWidth();
        setOption(e, &TextEditorOptions::markColumn);
        P.near("mark.column_widens_gutter", e.gutterWidth() - gutterBefore, 12.0, 0.0);
        const PrimList& l1 = f.host.draw();
        const std::vector<Prim> m1 = withTag(l1, tags::editMark);
        P.eq("mark.column_draws_marks", static_cast<int>(m1.size()), 2);
        P.eq("mark.column_left_of_numbers", m1.size() == 2 && m1[0].x1 <= kBox.x + 1.0f + 12.0f + 1.6f, 1);
        P.eq("mark.column_numbers_plain", countGlyphs(l1, tags::editGutter, errInk), 0);
        f.host.click(kBox.x + 6.0f, f.pointOf({ 2, 0 }).y);    // the mark column is the gutter: the line
        P.eq("mark.column_press_selects_line", is(e.selectedText(), "b\n"), 1);
        setOption(e, &TextEditorOptions::lineNumbers, false);
        P.near("mark.column_without_numbers", e.gutterWidth(), 12.0, 0.0);

        // What is drawn: an underline under its range near the row's foot, backgrounds over their ranges, the
        // current match boxed, one space width for an empty range, nothing for a range out of view.
        Fixture g;
        TextEditor& h = g.ed();
        h.setText("alpha beta gamma\ndelta");
        h.setDecorations({ { { 0, 6 }, { 0, 10 }, DK::error } });
        const std::vector<Prim> sq = withTag(g.host.draw(), tags::editMark);
        const float xa = h.pointOf({ 0, 6 }).x, xb = h.pointOf({ 0, 10 }).x;
        const float foot = h.pointOf({ 0, 0 }).y + h.rowHeight();
        bool under = !sq.empty();
        for (const Prim& p : sq)
        {
            const float cx = (p.x0 + p.x1) * 0.5f, cy = (p.y0 + p.y1) * 0.5f;
            const float sx0 = cx + p.d1[0], sy0 = cy + p.d1[1], sx1 = cx + p.d1[2], sy1 = cy + p.d1[3];
            under = under && kindOf(p) == 2 && sx0 >= xa - 0.01f && sx1 <= xb + 0.01f
                 && std::fmin(sy0, sy1) >= foot - 3.01f && std::fmax(sy0, sy1) <= foot - 0.99f;
        }
        // One segment per 2 px half period the range touches, counted on the row's own grid.
        const auto pieces = [&](float a, float b) {
            const float x0 = h.textArea().x;
            return static_cast<int>(std::ceil((b - x0) / 2.0f)) - static_cast<int>(std::floor((a - x0) / 2.0f));
        };
        P.eq("deco.squiggle_segments", static_cast<int>(sq.size()), pieces(xa, xb));
        P.eq("deco.squiggle_under_range", under, 1);
        h.setDecorations({ { { 1, 0 }, { 1, 5 }, DK::highlight }, { { 0, 0 }, { 0, 5 }, DK::findCurrent },
                           { { 0, 11 }, { 0, 16 }, DK::findMatch }, { { 1, 5 }, { 1, 5 }, DK::warning } });
        const std::vector<Prim> bg = withTag(g.host.draw(), tags::editMark);
        int boxes = 0, bordered = 0, segs = 0;
        bool occurrence = false;
        for (const Prim& p : bg)
        {
            boxes += kindOf(p) == 0 ? 1 : 0;
            bordered += kindOf(p) == 0 && p.d2[0] > 0.0f ? 1 : 0;
            segs += kindOf(p) == 2 ? 1 : 0;
            const Rect r = boxOf(p);
            occurrence = occurrence
                      || (kindOf(p) == 0 && std::fabs(r.x - h.pointOf({ 1, 0 }).x) < 0.01f
                          && std::fabs(r.right() - h.pointOf({ 1, 5 }).x) < 0.01f);
        }
        P.eq("deco.backgrounds", boxes, 3);
        P.eq("deco.current_match_boxed", bordered, 1);
        P.eq("deco.occurrence_over_range", occurrence, 1);
        const float space = h.pointOf({ 1, 1 }).x - h.pointOf({ 1, 0 }).x;
        P.eq("deco.empty_range_space_width", segs, pieces(h.pointOf({ 1, 5 }).x, h.pointOf({ 1, 5 }).x + space));
        std::string many;
        for (int i = 0; i < 40; ++i)
            many += "line " + std::to_string(i) + "\n";
        h.setText(many);
        h.setDecorations({ { { 30, 0 }, { 30, 4 }, DK::error } });
        P.eq("deco.out_of_view_not_drawn", countTag(g.host.draw(), tags::editMark), 0);
        h.setScroll(0.0f, 28.0f * h.rowHeight());
        P.eq("deco.scrolled_into_view", countTag(g.host.draw(), tags::editMark) > 0, 1);
    }

    // Indent and outdent change each line on its own: decorations stay on their text and marks on their lines, through
    // undo and redo too.
    void decorationsThroughIndent(T::Probe& P)
    {
        Fixture f;
        TextEditor& e = f.ed();
        e.setFocused(true);
        e.setText("    foo(bar)");
        e.setDecorations({ { { 0, 8 }, { 0, 11 }, DK::error } });
        e.setCaret({ 0, 4 });
        f.host.keys("shift+tab");
        P.eq("deco.outdent_keeps", is(e.text(), "foo(bar)") && is(decs(e), "0@0:4-0:7"), 1);
        P.eq("deco.outdent_undo", e.undo() && is(e.text(), "    foo(bar)") && is(decs(e), "0@0:8-0:11"), 1);
        P.eq("deco.outdent_redo", e.redo() && is(e.text(), "foo(bar)") && is(decs(e), "0@0:4-0:7"), 1);

        e.setText("a\nb\nc\nd\ne");
        e.setLineMarks({ { 2, LineMark::Kind::error }, { 3, LineMark::Kind::warning } });
        e.setDecorations({ { { 3, 0 }, { 3, 1 }, DK::warning } });
        e.select({ 2, 0 }, { 4, 1 });
        f.host.keys("tab");
        P.eq("mark.indent_keeps_lines", is(e.text(), "a\nb\n\tc\n\td\n\te") && is(marks(e), "2:2 3:1"), 1);
        P.eq("deco.indent_keeps", is(decs(e), "1@3:1-3:2"), 1);
        P.eq("mark.indent_undo_keeps_lines",
             e.undo() && is(e.text(), "a\nb\nc\nd\ne") && is(marks(e), "2:2 3:1") && is(decs(e), "1@3:0-3:1"), 1);
        P.eq("mark.indent_redo_keeps_lines", e.redo() && is(marks(e), "2:2 3:1") && is(decs(e), "1@3:1-3:2"), 1);
    }

    void brackets(T::Probe& P)
    {
        TextEditor e;
        e.setText("a(b[c]d)e\n{\n  x\n}\n(()");
        TextPos o, c;
        const auto pairAt = [&](TextPos at) {
            return e.bracketPair(at, o, c) ? pos(o) + "-" + pos(c) : std::string("none");
        };
        P.eq("bracket.before_caret", is(pairAt({ 0, 2 }), "0:1-0:7"), 1);
        P.eq("bracket.after_caret", is(pairAt({ 0, 1 }), "0:1-0:7"), 1);
        P.eq("bracket.nested", is(pairAt({ 0, 4 }), "0:3-0:5"), 1);
        P.eq("bracket.closer_before", is(pairAt({ 0, 6 }), "0:3-0:5"), 1);
        P.eq("bracket.closer_outer", is(pairAt({ 0, 8 }), "0:1-0:7"), 1);
        P.eq("bracket.none", is(pairAt({ 0, 9 }), "none"), 1);
        P.eq("bracket.across_lines", is(pairAt({ 1, 1 }), "1:0-3:0"), 1);
        P.eq("bracket.unmatched", is(pairAt({ 4, 0 }), "none"), 1);
        e.setText("f(\")\")");
        P.eq("bracket.plain_counts_all", is(pairAt({ 0, 2 }), "0:1-0:3"), 1);
        e.setHighlighter(std::make_unique<KeywordHighlighter>(KeywordRules::cLike()));
        P.eq("bracket.styled_skips_string", is(pairAt({ 0, 2 }), "0:1-0:5"), 1);

        // Drawn while focused with matchBrackets: two boxes, at the two brackets.
        Fixture f;
        TextEditor& h = f.ed();
        h.setText("f(a[1])");
        h.setCaret({ 0, 2 });
        setOption(h, &TextEditorOptions::matchBrackets);
        P.eq("bracket.unfocused_not_drawn", countTag(f.host.draw(), tags::editMark), 0);
        h.setFocused(true);
        const std::vector<Prim> b = withTag(f.host.draw(), tags::editMark);
        P.eq("bracket.boxes", static_cast<int>(b.size()), 2);
        P.eq("bracket.box_at_open", b.size() == 2 && std::fabs(boxOf(b[0]).x - h.pointOf({ 0, 1 }).x) < 0.01f, 1);
        P.eq("bracket.box_at_close", b.size() == 2 && std::fabs(boxOf(b[1]).x - h.pointOf({ 0, 6 }).x) < 0.01f, 1);
        h.setCaret({ 0, 0 });
        P.eq("bracket.moves_with_caret", countTag(f.host.draw(), tags::editMark), 0);
        setOption(h, &TextEditorOptions::matchBrackets, false);
        h.setCaret({ 0, 2 });
        P.eq("bracket.option_off", countTag(f.host.draw(), tags::editMark), 0);
    }

    void closingPairs(T::Probe& P)
    {
        Fixture f;
        TextEditor& e = f.ed();
        e.setFocused(true);
        typeKeys(f, "f(");
        P.eq("close.off_by_default", is(e.text(), "f("), 1);
        setOption(e, &TextEditorOptions::autoClosePairs);
        e.setText("");
        typeKeys(f, "f(");
        P.eq("close.paren", is(e.text(), "f()") && is(pos(e.caret()), "0:2"), 1);
        typeKeys(f, "x");
        P.eq("close.typed_inside", is(e.text(), "f(x)"), 1);
        typeKeys(f, ")");
        P.eq("close.steps_over", is(e.text(), "f(x)") && is(pos(e.caret()), "0:4"), 1);
        typeKeys(f, ")");
        P.eq("close.only_its_own", is(e.text(), "f(x))"), 1);
        e.setText("abc");
        typeKeys(f, "(");
        P.eq("close.not_before_word", is(e.text(), "(abc"), 1);
        e.setText("a;");
        e.setCaret({ 0, 1 });
        typeKeys(f, "[");
        P.eq("close.before_punctuation", is(e.text(), "a[];"), 1);
        e.setText("");
        typeKeys(f, "\"");
        P.eq("close.quote", is(e.text(), "\"\"") && is(pos(e.caret()), "0:1"), 1);
        typeKeys(f, "\"");
        P.eq("close.quote_steps_over", is(e.text(), "\"\"") && is(pos(e.caret()), "0:2"), 1);
        e.setText("don");
        e.setCaret(e.endPos());
        typeKeys(f, "'");
        P.eq("close.quote_not_after_word", is(e.text(), "don'"), 1);
        e.setText("abc");
        e.select({ 0, 0 }, { 0, 3 });
        typeKeys(f, "[");
        P.eq("close.wraps_selection", is(e.text(), "[abc]") && is(e.selectedText(), "abc"), 1);
        e.setText("foo");
        e.setDecorations({ { { 0, 0 }, { 0, 3 }, DK::error } });
        e.select({ 0, 0 }, { 0, 3 });
        typeKeys(f, "(");
        P.eq("close.wrap_keeps_decoration",
             is(e.text(), "(foo)") && is(e.selectedText(), "foo") && is(decs(e), "0@0:1-0:4"), 1);
        P.eq("close.wrap_one_step", e.undo() && is(e.text(), "foo") && is(decs(e), "0@0:0-0:3") && !e.canUndo(), 1);
        e.setText("");
        typeKeys(f, "{");
        f.host.keys("backspace");
        P.eq("close.backspace_pair", is(e.text(), ""), 1);
        e.setText("()");
        e.setCaret({ 0, 1 });
        f.host.keys("backspace");
        P.eq("close.backspace_any_pair", is(e.text(), ""), 1);
        e.setText("()");
        e.setCaret({ 0, 1 });
        typeKeys(f, ")");
        P.eq("close.not_inserted_not_stepped", is(e.text(), "())"), 1);
        e.setText("\n");
        typeKeys(f, "(");
        f.host.keys("down,up");
        e.setCaret({ 0, 1 });
        typeKeys(f, ")");
        P.eq("close.forgotten_off_line", is(e.text(), "())\n"), 1);
        e.setText("");
        typeKeys(f, "(x");
        f.host.keys("cmd+z");
        P.eq("close.undo_typed", is(e.text(), "()"), 1);
        f.host.keys("cmd+z");
        P.eq("close.undo_pair", is(e.text(), ""), 1);
    }

    void braces(T::Probe& P)
    {
        Fixture f;
        TextEditor& e = f.ed();
        e.setFocused(true);
        setOption(e, &TextEditorOptions::braceIndent);
        e.setText("f() {}");
        e.setCaret({ 0, 5 });
        f.host.keys("return");
        P.eq("brace.between", is(e.text(), "f() {\n\t\n}") && is(pos(e.caret()), "1:1"), 1);
        f.host.keys("cmd+z");
        P.eq("brace.one_step", is(e.text(), "f() {}") && is(pos(e.caret()), "0:5"), 1);
        TextEditorOptions o = e.options();
        o.softTabs = true;
        e.setOptions(o);
        e.setText("    if (x) {}");
        e.setCaret({ 0, 12 });
        f.host.keys("return");
        P.eq("brace.indented_soft", is(e.text(), "    if (x) {\n        \n    }") && is(pos(e.caret()), "1:8"), 1);
        o.softTabs = false;
        e.setOptions(o);
        e.setText("{   ");
        e.setCaret(e.endPos());
        f.host.keys("return");
        P.eq("brace.after_opener", is(e.text(), "{\n\t") && is(pos(e.caret()), "1:1"), 1);
        e.setText("x");
        e.setCaret(e.endPos());
        f.host.keys("return");
        P.eq("brace.plain_return", is(e.text(), "x\n"), 1);
        e.setText("\tf() {\n\t\t");
        e.setCaret({ 1, 2 });
        typeKeys(f, "}");
        P.eq("brace.closer_takes_opener_indent", is(e.line(1), "\t}") && is(pos(e.caret()), "1:2"), 1);
        e.setText("\t\t");
        e.setCaret(e.endPos());
        typeKeys(f, "]");
        P.eq("brace.closer_outdents", is(e.text(), "\t]"), 1);
        e.setText("        ");
        e.setCaret(e.endPos());
        typeKeys(f, ")");
        P.eq("brace.closer_outdents_spaces", is(e.text(), "    )"), 1);
        e.setText("\tx");
        e.setCaret(e.endPos());
        typeKeys(f, "}");
        P.eq("brace.closer_after_text", is(e.text(), "\tx}"), 1);
        setOption(e, &TextEditorOptions::autoIndent, false);   // braceIndent still starts from the line's own indent
        e.setText("\tif (x) {}");
        e.setCaret({ 0, 9 });
        f.host.keys("return");
        P.eq("brace.own_indent_without_autoindent",
             is(e.text(), "\tif (x) {\n\t\t\n\t}") && is(pos(e.caret()), "1:2"), 1);
        e.setText("\tx");
        e.setCaret(e.endPos());
        f.host.keys("return");
        P.eq("brace.plain_return_without_autoindent", is(e.text(), "\tx\n"), 1);
        setOption(e, &TextEditorOptions::autoIndent);
        setOption(e, &TextEditorOptions::autoClosePairs);
        e.setText("");
        typeKeys(f, "{");
        f.host.keys("return");
        P.eq("brace.with_autoclose", is(e.text(), "{\n\t\n}") && is(pos(e.caret()), "1:1"), 1);
        typeKeys(f, "x;");
        f.host.keys("down");
        P.eq("brace.closer_below", is(pos(e.caret()), "2:1"), 1);
    }

    void serviceEdits(T::Probe& P)
    {
        TextEditor e;
        e.setText("pri x");
        e.setCaret({ 0, 3 });
        P.eq("svc.replace", e.replaceRange({ 0, 0 }, { 0, 3 }, "print") && is(e.text(), "print x"), 1);
        P.eq("svc.replace_caret_after", is(pos(e.caret()), "0:5"), 1);
        P.eq("svc.replace_undo", e.undo() && is(e.text(), "pri x") && is(pos(e.caret()), "0:3"), 1);
        P.eq("svc.replace_redo", e.redo() && is(e.text(), "print x") && is(pos(e.caret()), "0:5"), 1);
        P.eq("svc.replace_noop", e.replaceRange({ 0, 0 }, { 0, 5 }, "print"), 0);
        e.select({ 0, 6 }, { 0, 7 });
        e.replaceRange({ 0, 0 }, { 0, 0 }, "\xC3\xA9");
        P.eq("svc.replace_sanitised", is(e.text(), "?print x"), 1);
        P.eq("svc.replace_undo_selection",
             e.undo() && is(pos(e.anchor()), "0:6") && is(pos(e.caret()), "0:7"), 1);

        e.setText("a=1\nb=2\nc=3");
        e.setCaret({ 2, 3 });
        const bool applied = e.applyEdits({ { { 1, 1 }, { 1, 2 }, " = " }, { { 0, 1 }, { 0, 2 }, " = " },
                                            { { 2, 1 }, { 2, 2 }, " = " } });
        P.eq("svc.edits", applied && is(e.text(), "a = 1\nb = 2\nc = 3"), 1);
        P.eq("svc.edits_caret_mapped", is(pos(e.caret()), "2:5"), 1);
        P.eq("svc.edits_one_step", e.undo() && is(e.text(), "a=1\nb=2\nc=3") && is(pos(e.caret()), "2:3")
                                       && !e.canUndo(), 1);
        P.eq("svc.edits_redo", e.redo() && is(e.text(), "a = 1\nb = 2\nc = 3") && is(pos(e.caret()), "2:5"), 1);
        P.eq("svc.edits_overlap_refused",
             !e.applyEdits({ { { 0, 0 }, { 0, 2 }, "x" }, { { 0, 1 }, { 0, 3 }, "y" } })
                 && is(e.text(), "a = 1\nb = 2\nc = 3"), 1);
        P.eq("svc.edits_noop_refused", e.applyEdits({ { { 0, 0 }, { 0, 1 }, "a" } }), 0);
        e.setText("x;y");
        e.applyEdits({ { { 0, 0 }, { 0, 0 }, "A" }, { { 0, 2 }, { 0, 2 }, "\n" }, { { 0, 0 }, { 0, 0 }, "B" } });
        P.eq("svc.edits_insert_order_lines", is(e.text(), "ABx;\ny") && e.lineCount() == 2, 1);
        e.setText("foo bar baz");
        e.setDecorations({ { { 0, 8 }, { 0, 11 }, DK::error } });
        e.applyEdits({ { { 0, 4 }, { 0, 7 }, "barbar" }, { { 0, 0 }, { 0, 3 }, "f" } });
        P.eq("svc.edits_move_decorations", is(e.text(), "f barbar baz") && is(decs(e), "0@0:9-0:12"), 1);

        e.setText("one\ntwo\nthree");
        e.setCaret({ 2, 4 });
        P.eq("svc.text", e.replaceText("one\n2\nthree") && is(e.text(), "one\n2\nthree"), 1);
        P.eq("svc.text_caret_kept", is(pos(e.caret()), "2:4"), 1);
        P.eq("svc.text_undo", e.undo() && is(e.text(), "one\ntwo\nthree") && is(pos(e.caret()), "2:4"), 1);
        P.eq("svc.text_same_refused", e.replaceText("one\ntwo\nthree"), 0);
        e.setText("abcdef");
        e.setCaret({ 0, 6 });
        e.replaceText("ab");
        P.eq("svc.text_caret_clamped", is(pos(e.caret()), "0:2"), 1);
        e.setText("aaa bbb ccc");
        e.setDecorations({ { { 0, 8 }, { 0, 11 }, DK::warning } });
        e.replaceText("aaa XX ccc");
        P.eq("svc.text_keeps_decorations", is(decs(e), "1@0:7-0:10"), 1);

        TextEditorOptions o = e.options();
        o.readOnly = true;
        e.setOptions(o);
        P.eq("svc.read_only", !e.replaceRange({ 0, 0 }, { 0, 1 }, "z") && !e.applyEdits({ { { 0, 0 }, { 0, 0 }, "z" } })
                                  && !e.replaceText("z") && is(e.text(), "aaa XX ccc"), 1);
    }

    void overlayGeometry(T::Probe& P)
    {
        Fixture f;
        TextEditor& e = f.ed();
        e.setText("one two\nthree\n");
        e.setCaret({ 1, 2 });
        const Point cp = e.caretPoint(), p12 = e.pointOf({ 1, 2 });
        P.eq("geo.point_of_caret", p12.x == cp.x && p12.y == cp.y, 1);
        P.near("geo.point_of_x", e.pointOf({ 0, 3 }).x - e.textArea().x, 3.0 * 21.8182 * 0.25, 0.01);
        P.near("geo.point_of_y", e.pointOf({ 1, 0 }).y - e.textArea().y, e.rowHeight(), 0.0);
        const auto at = [&](TextPos p) {
            const Point q = e.pointOf(p);
            const std::optional<TextPos> r = e.posAt({ q.x + 1.0f, q.y + e.rowHeight() * 0.5f });
            return r ? pos(*r) : std::string("none");
        };
        P.eq("geo.pos_at_round_trip", is(at({ 0, 4 }), "0:4") && is(at({ 1, 5 }), "1:5"), 1);
        const float midY = e.pointOf({ 0, 0 }).y + e.rowHeight() * 0.5f;
        const std::optional<TextPos> lineEnd = e.posAt({ e.textArea().right() - 2.0f, midY });
        P.eq("geo.pos_at_line_end", lineEnd.has_value() && *lineEnd == TextPos{ 0, 7 }, 1);
        P.eq("geo.pos_at_gutter", e.posAt({ kBox.x + 4.0f, midY }).has_value(), 0);
        P.eq("geo.pos_at_padding", e.posAt({ e.textArea().x + 4.0f, e.textArea().y - 2.0f }).has_value(), 0);
        P.eq("geo.pos_at_below_rows", e.posAt({ e.textArea().x + 4.0f, e.textArea().y + 3.5f * e.rowHeight() })
                                          .has_value(), 0);
        P.eq("geo.pos_at_outside", e.posAt({ 2.0f, 2.0f }).has_value(), 0);

        std::string many;
        for (int i = 0; i < 40; ++i)
            many += "line " + std::to_string(i) + "\n";
        e.setText(many);
        P.eq("geo.visible_lines", e.visibleLineRange().first == 0 && e.visibleLineRange().last == 6, 1);
        e.setScroll(0.0f, 2.0f * e.rowHeight());
        P.eq("geo.visible_lines_scrolled", e.visibleLineRange().first == 2 && e.visibleLineRange().last == 8, 1);
        P.near("geo.point_of_scrolled", e.pointOf({ 2, 0 }).y, e.textArea().y, 0.0);

        e.setText("foo_bar1 baz.qux a + b");
        const auto word = [&](int col) {
            const funkgui::TextRange r = e.wordRangeAt({ 0, col });
            return std::to_string(r.from.col) + "-" + std::to_string(r.to.col);
        };
        P.eq("geo.word_inside", is(word(3), "0-8"), 1);
        P.eq("geo.word_touching_left", is(word(8), "0-8"), 1);
        P.eq("geo.word_touching_right", is(word(9), "9-12"), 1);
        P.eq("geo.word_before_dot", is(word(12), "9-12") && is(word(13), "13-16"), 1);
        P.eq("geo.word_none", is(word(19), "19-19") && e.wordRangeAt({ 0, 19 }).empty(), 1);

        TextEditorOptions o = e.options();
        o.wordWrap = true;
        e.setOptions(o);
        e.setText("word word word word word word word word word word word word word word word word word");
        int brk = 0;
        while (brk < 80 && e.rowOf({ 0, brk }) == 0)
            ++brk;
        P.eq("geo.wrapped", e.rowCount() > 1 && brk < 80, 1);
        P.eq("geo.point_of_break_starts_next_row",
             e.pointOf({ 0, brk }).x == e.textArea().x && e.pointOf({ 0, brk }).y == e.textArea().y + e.rowHeight(), 1);
    }

    void codeLooks(T::Probe& P)
    {
        Fixture f;
        TextEditor& e = f.ed();
        e.setText("{\n\tif (x) {\n\t\ty();\n\n\t}\n}");
        P.eq("guide.off_by_default", countTag(f.host.draw(), tags::editGuide), 0);
        setOption(e, &TextEditorOptions::indentGuides);
        const std::vector<Prim> g = withTag(f.host.draw(), tags::editGuide);
        P.eq("guide.levels", static_cast<int>(g.size()), 0 + 1 + 2 + 2 + 1 + 0);   // the blank line: as its block
        float maxX = 0.0f;
        for (const Prim& p : g)
            maxX = std::max(maxX, boxOf(p).x);
        P.near("guide.at_tab_stops", maxX, e.textArea().x + 4.0 * 21.8182 * 0.25, 0.51);

        setOption(e, &TextEditorOptions::indentGuides, false);
        setOption(e, &TextEditorOptions::showTrailingBlanks);
        e.setText("a  \tb\na  \t\n   ");
        const std::vector<Prim> t = withTag(f.host.draw(), tags::editGuide);
        int dots = 0;
        for (const Prim& p : t)
            dots += boxOf(p).h > 1.0f ? 1 : 0;                  // a 1.5 px dot, or a device px rule across a tab
        P.eq("blanks.trailing_only", static_cast<int>(t.size()), 3 + 3);
        P.eq("blanks.dots_and_rule", dots, 5);

        // The minimap: the text area gives way; a bar per run of non-blank characters; the slider; a press beside
        // the slider centres it there, a drag moves it.
        setOption(e, &TextEditorOptions::showTrailingBlanks, false);
        e.setText("ab cd\n\tx");
        const float taW = e.textArea().w;
        P.eq("map.off_by_default", countTag(f.host.draw(), tags::editMap) == 0 && e.minimapArea().isEmpty(), 1);
        setOption(e, &TextEditorOptions::minimap);
        const Rect m = e.minimapArea();
        P.near("map.width", m.w, 64.0, 0.001);
        P.near("map.text_gives_way", taW - e.textArea().w, 64.0, 0.001);
        const std::vector<Prim> mp = withTag(f.host.draw(), tags::editMap);
        P.eq("map.prims", static_cast<int>(mp.size()), 1 + 3 + 1);   // the rule, three bars, the slider
        P.eq("map.tab_expanded", mp.size() == 5 && std::fabs(boxOf(mp[3]).x - (m.x + 4.0f + 4.0f)) < 0.01f
                                     && std::fabs(boxOf(mp[3]).y - (m.y + 2.0f + 0.25f)) < 0.01f, 1);
        P.eq("map.pos_at_none", e.posAt({ m.x + 10.0f, m.y + 2.0f }).has_value(), 0);
        TextEditorOptions o = e.options();
        o.minimapWidth = 1.0f;
        e.setOptions(o);
        P.near("map.width_at_least_16", e.minimapArea().w, 16.0, 0.001);
        o.minimapWidth = 1000.0f;
        e.setOptions(o);
        P.near("map.width_at_most_half", e.minimapArea().w, (kBox.w - 2.0f) * 0.5f, 0.001);
        o.minimapWidth = 64.0f;
        e.setOptions(o);

        std::string many;
        for (int i = 0; i < 40; ++i)
            many += "line " + std::to_string(i) + "\n";
        e.setText(many);
        const Rect mm = e.minimapArea();
        const float shown = std::min(mm.h, static_cast<float>(e.rowCount()) * 2.0f);
        const float sliderH = std::min(shown, e.textArea().h / e.rowHeight() * 2.0f);
        const float maxScroll = e.contentHeight() - e.textArea().h;
        const float py = mm.y + 50.0f;
        f.host.press(mm.x + 20.0f, py);
        P.near("map.press_centres_slider", e.scroll().y, (py - sliderH * 0.5f - mm.y) / (shown - sliderH) * maxScroll,
               0.01);
        P.eq("map.press_keeps_caret", is(pos(e.caret()), "0:0"), 1);
        f.host.moveTo(mm.x + 20.0f, py + 10.0f, 2);
        P.near("map.drag", e.scroll().y, (py + 10.0f - sliderH * 0.5f - mm.y) / (shown - sliderH) * maxScroll, 0.01);
        f.host.release();
        P.eq("map.clipped", allInside(f.host.draw(), kBox.expanded(2.0f)), 1);
    }

    // ---- accessibility, read-only, fonts ----------------------------------------------------------------------------

    void misc(T::Probe& P)
    {
        Fixture f;
        TextEditor& e = f.ed();
        e.setText("ab\ncd");
        std::vector<A11yItem> items = f.host.accessibility();
        P.eq("a11y.one_item", static_cast<int>(items.size()), 1);
        const A11yItem& it = items[0];
        P.eq("a11y.role", static_cast<int>(it.role), 15);
        P.eq("a11y.id", static_cast<int>(it.id), 7);
        P.eq("a11y.title", is(it.title, "Text"), 1);
        P.eq("a11y.value", is(it.value, "ab\ncd"), 1);
        P.eq("a11y.description", is(it.description, "line 1, column 1"), 1);
        P.eq("a11y.bounds", it.bounds.x == kBox.x && it.bounds.w == kBox.w, 1);
        P.eq("a11y.not_readonly", it.readOnly, 0);
        e.select({ 0, 1 }, { 1, 1 });
        items = f.host.accessibility();
        P.eq("a11y.selection", is(items[0].description, "line 2, column 2, 3 selected"), 1);
        P.eq("a11y.line", is(funkgui::a11yDumpLine(items[0]).substr(0, 2), "15"), 1);

        TextEditorOptions o = e.options();
        o.readOnly = true;
        o.a11yTitle = "Notes";
        e.setOptions(o);
        e.setFocused(true);
        e.setCaret({ 0, 0 });
        P.eq("ro.type_refused", f.panel.key(KeyEvent{ Key::character, {}, U'z' }), 0);
        P.eq("ro.return_refused", f.panel.key(KeyEvent{ Key::enter, {}, 0 }), 0);
        P.eq("ro.backspace_refused", f.panel.key(KeyEvent{ Key::backspace, {}, 0 }), 0);
        P.eq("ro.tab_refused", f.panel.key(KeyEvent{ Key::tab, {}, 0 }), 0);
        P.eq("ro.paste_refused", !e.paste() && !e.insert("q") && !e.deleteSelection(), 1);
        P.eq("ro.text_unchanged", is(e.text(), "ab\ncd"), 1);
        P.eq("ro.moves", f.panel.key(KeyEvent{ Key::right, {}, 0 }) && is(pos(e.caret()), "0:1"), 1);
        f.host.keys("shift+right,cmd+c");
        P.eq("ro.copies", is(f.host.log.lastCopy, "b"), 1);
        P.eq("ro.cut_refused", !e.cut() && is(e.text(), "ab\ncd"), 1);
        items = f.host.accessibility();
        P.eq("ro.a11y", items[0].readOnly && is(items[0].title, "Notes"), 1);
        P.eq("ro.undo_refused", !e.undo(), 1);

        // The font presets and a custom style: px and the row height; the metrics follow.
        P.eq("font.code", funkgui::editorFont(EditorFont::code).px == 12.0f
                              && funkgui::editorFont(EditorFont::code).tabular, 1);
        P.eq("font.prose", funkgui::editorFont(EditorFont::prose).px == 13.0f
                               && !funkgui::editorFont(EditorFont::prose).tabular, 1);
        P.eq("font.compact", funkgui::editorFont(EditorFont::compact).px == 10.0f, 1);
        P.eq("font.large", funkgui::editorFont(EditorFont::large).px == 16.0f, 1);
        P.eq("font.default_is_code", e.options().style.px == 12.0f, 1);
        P.near("font.row_height_code", e.rowHeight(), 17.0, 0.0);
        e.setFont(EditorFont::large);
        P.near("font.row_height_large", e.rowHeight(), 22.0, 0.0);
        P.eq("font.style_set", e.options().style.px == 16.0f, 1);
        e.setFont(EditorFont::compact);
        P.near("font.row_height_compact", e.rowHeight(), 14.0, 0.0);
        e.setStyle(funkgui::TextStyle{ 20.0f, 1.0f, 0.0f, false });
        P.near("font.row_height_custom", e.rowHeight(), 28.0, 0.0);
        e.setText("ab");
        e.setCaret({ 0, 2 });
        P.near("font.tracking_in_advance", e.caretPoint().x - e.textArea().x, 2.0 * (21.8182 * 20.0 / 48.0 + 1.0),
               0.01);
        o = e.options();
        o.lineSpacing = 1.0f;
        e.setOptions(o);
        P.near("font.line_spacing", e.rowHeight(), 20.0, 0.0);
        o.lineSpacing = 0.0f;                                   // not > 0: 1.0
        o.tabWidth = 0;                                         // at least 1
        e.setOptions(o);
        P.eq("font.options_sanitised", e.options().lineSpacing == 1.0f && e.options().tabWidth == 1, 1);
        P.eq("font.cursor_normal", static_cast<int>(e.cursorAt({ 20.0f, 20.0f })), 0);
        P.eq("font.contains", e.contains({ 20.0f, 20.0f }) && !e.contains({ 2.0f, 2.0f }), 1);
        P.eq("tags.numbers", tags::editFrame == 20 && tags::editGutter == 21 && tags::editText == 22
                                 && tags::editSelect == 23 && tags::editCaret == 24, 1);
        P.eq("tags.names", is(funkgui::tagName(tags::editCaret), "EDIT_CARET")
                               && funkgui::tagFromName("EDIT_TEXT") == tags::editText, 1);
        P.eq("tags.numbers_v22", tags::editMark == 25 && tags::editGuide == 26 && tags::editMap == 27, 1);
        P.eq("tags.names_v22", is(funkgui::tagName(tags::editMark), "EDIT_MARK")
                                   && is(funkgui::tagName(tags::editGuide), "EDIT_GUIDE")
                                   && funkgui::tagFromName("EDIT_MAP") == tags::editMap, 1);
    }
}

int main(int argc, char** argv)
{
    const funkgui::HeadlessGuiScope gui;
    const T::ScopedFtz ftz;
    T::Probe P("fg.texteditor", "", argc, argv);
    funkgui::FontService::get().atlas();
    if (!P.eq("font.ok", funkgui::FontService::get().ok(), 1))
        return P.finish();
    textRule(P);
    typing(P);
    movement(P);
    selection(P);
    deleting(P);
    indenting(P);
    undoRedo(P);
    clipboard(P);
    finding(P);
    wrapping(P);
    scrolling(P);
    highlighting(P);
    drawing(P);
    misc(P);
    decorations(P);
    decorationsThroughIndent(P);
    brackets(P);
    closingPairs(P);
    braces(P);
    serviceEdits(P);
    overlayGeometry(P);
    codeLooks(P);
    return P.finish();
}

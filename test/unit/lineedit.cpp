// FUNKGUI_TEST name=fg.lineedit timeout=120 gpu=0
//
// fg.lineedit: typed text and discrete input (02 §5.5, §5.8, §5.9, §8.9). Spec rows only.
// - lineedit.*, printable.*: text::LineEdit (HR PresetPanel.cpp editKey and the selected pre-fill) and text::printable.
// - menulook.*: MenuLook's popup-menu colours from the theme tokens, setTheme(), and the bundled face at 14 px.
// - cells.*, prefcells.*, themecells.*: SegmentedSelector over ParamCells / PrefCells / ThemeCells, and latch.*:
//   LatchToggle over ParamToggle — the input contracts behind the gallery pictures (select on down, tap semantics, the
//   host menu never writing, disabled cells and latches refusing, keys, a11y items and actions, hover settling), against
//   a recording port and host. (The card's test list has no unit test of its own for the cell and latch widgets; their
//   rows live here, beside the other discrete-input rules, and fg.gallery.{segmented,latch,theme} show them.)
// The preference rows run against a fresh scratch store (<ENV_PREFIX>PREFS_DIR, removed at the end), never the real one.

#include <funkgui/core/Config.h>
#include <funkgui/core/Env.h>
#include <funkgui/core/Theme.h>
#include <funkgui/juce/MenuLook.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/params/GestureController.h>
#include <funkgui/params/ParamPort.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/test/Harness.h>
#include <funkgui/text/LineEdit.h>
#include <funkgui/widgets/LatchToggle.h>
#include <funkgui/widgets/SegmentedSelector.h>
#include <funkgui/widgets/ThemeCells.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace T = funkgui::test;
using funkgui::Key;
using funkgui::KeyEvent;
using funkgui::Mods;
using funkgui::text::LineEdit;

namespace
{
    // ---- LineEdit helpers -------------------------------------------------------------------------------------------

    KeyEvent k(Key key, Mods m = {})
    {
        KeyEvent e;
        e.key = key;
        e.mods = m;
        if (key == Key::space)
            e.ch = U' ';
        return e;
    }

    KeyEvent ch(char32_t c, Mods m = {})
    {
        KeyEvent e;
        e.key = Key::character;
        e.ch = c;
        e.mods = m;
        return e;
    }

    Mods cmd() { Mods m; m.cmd = true; return m; }
    Mods alt() { Mods m; m.alt = true; return m; }
    Mods ctrl() { Mods m; m.ctrl = true; return m; }

    void type(LineEdit& e, const char* s, int maxLength = 40)
    {
        for (const char* p = s; *p != 0; ++p)
            e.key(*p == ' ' ? k(Key::space) : ch(static_cast<char32_t>(*p)), maxLength);
    }

    LineEdit edit(const char* text, int caret)
    {
        LineEdit e;
        e.buffer = text;
        e.caret = caret;
        return e;
    }

    // ---- A recording port and host ----------------------------------------------------------------------------------

    class Port final : public funkgui::ParamPort
    {
    public:
        explicit Port(float v = 0.0f) : v_(v) {}
        float value01() const override { return v_; }
        float default01() const override { return 0.0f; }
        int   numSteps() const override { return 3; }
        void  beginGesture() override { ++begins; }
        void  setValue01(float v) override { ++sets; v_ = v; }
        void  endGesture() override { ++ends; }
        const char* id() const override { return "port"; }
        void* native() const override { return nullptr; }

        int writes() const { return sets; }
        bool balanced() const { return begins == ends && begins == sets; }   // every write a begin/set/end triple

        int begins = 0, sets = 0, ends = 0;

    private:
        float v_;
    };

    class Host final : public funkgui::HostServices
    {
    public:
        void   setUnboundedDrag(bool) override {}
        void   showParamMenu(funkgui::ParamPort&, float, float) override { ++menus; }
        void   nudgeFullRate() override {}
        double nowSeconds() const override { return 0.0; }
        void   beginBatch() override {}
        void   endBatch() override {}
        int menus = 0;
    };

    funkgui::PointerEvent pt(float x, float y, bool popup = false)
    {
        funkgui::PointerEvent e;
        e.x = x;
        e.y = y;
        e.popup = popup;
        return e;
    }

    // Three cells; `disabled` (when >= 0) refuses with a reason.
    class Cells final : public funkgui::ParamCells
    {
    public:
        Cells(funkgui::ParamPort& p, std::vector<funkgui::CellText> t, int disabled = -1)
            : ParamCells(p, std::move(t)), disabled_(disabled) {}
        bool enabled(int i) const override { return i != disabled_; }
        const char* help(int i) const override { return i == disabled_ ? "NEEDS HQ" : ParamCells::help(i); }

    private:
        int disabled_;
    };

    class Toggle final : public funkgui::ParamToggle
    {
    public:
        Toggle(funkgui::ParamPort& p, bool enabled) : ParamToggle(p), enabled_(enabled) {}
        bool enabled() const override { return enabled_; }
        const char* reason() const override { return "NO SIDECHAIN BUS CONNECTED"; }

    private:
        bool enabled_;
    };

    const funkgui::A11yItem* item(const std::vector<funkgui::A11yItem>& v, uint32_t id)
    {
        for (const auto& i : v)
            if (i.id == id)
                return &i;
        return nullptr;
    }

    int64_t rgb(juce::Colour c) { return static_cast<int64_t>(c.getARGB()); }
    int64_t rgb(funkgui::Col c) { return static_cast<int64_t>(juce::Colour(c.r, c.g, c.b, c.a).getARGB()); }
}

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;      // MenuLook's typeface, UiPreferences' PropertiesFile
    T::Probe P("fg.lineedit", "", argc, argv);

    // ---- LineEdit: the selected pre-fill (HR: Save As on "Keep" then "Temp" saved "KeepTemp") -------------------------
    {
        LineEdit e;
        e.set("Drum Bus 02", 40);
        P.eq("lineedit.set_selects", e.buffer == "Drum Bus 02" && e.caret == 11 && e.allSelected, 1);
        e.set("", 40);
        P.eq("lineedit.set_empty_not_selected", e.buffer.empty() && e.caret == 0 && !e.allSelected, 1);
        e.set("Caf\xC3\xA9 \xE2\x98\x83 Bus", 40);
        P.eq("lineedit.set_printable", e.buffer == "Caf? ? Bus", 1);
        e.set("0123456789", 4);
        P.eq("lineedit.set_cut_to_max", e.buffer == "0123" && e.caret == 4, 1);

        e.set("Keep", 40);
        P.eq("lineedit.selected_char_replaces", e.key(ch(U'T'), 40) && e.buffer == "T" && e.caret == 1
                                                    && !e.allSelected, 1);
        type(e, "emp");
        P.eq("lineedit.selected_then_typing", e.buffer == "Temp" && e.caret == 4, 1);
        e.set("Keep", 40);
        P.eq("lineedit.selected_backspace_clears", e.key(k(Key::backspace), 40) && e.buffer.empty() && e.caret == 0, 1);
        e.set("Keep", 40);
        P.eq("lineedit.selected_delete_clears", e.key(k(Key::del), 40) && e.buffer.empty(), 1);
        e.set("Keep", 40);
        P.eq("lineedit.selected_space_replaces", e.key(k(Key::space), 40) && e.buffer == " ", 1);
        e.set("Keep", 40);
        P.eq("lineedit.selected_left_to_start", e.key(k(Key::left), 40) && e.buffer == "Keep" && e.caret == 0
                                                    && !e.allSelected, 1);
        e.set("Keep", 40);
        P.eq("lineedit.selected_right_to_end", e.key(k(Key::right), 40) && e.caret == 4 && !e.allSelected, 1);
        e.set("Keep", 40);
        P.eq("lineedit.return_left_to_product", !e.key(k(Key::enter), 40) && e.allSelected && e.buffer == "Keep", 1);
        P.eq("lineedit.escape_left_to_product", !e.key(k(Key::escape), 40) && e.allSelected, 1);
        P.eq("lineedit.selected_cmd_char_keeps_text", !e.key(ch(U'a', cmd()), 40) && e.buffer == "Keep"
                                                          && !e.allSelected, 1);
        e.set("Keep", 40);
        P.eq("lineedit.selected_tab_drops_selection", !e.key(k(Key::tab), 40) && !e.allSelected && e.buffer == "Keep", 1);
    }

    // ---- LineEdit: HR editKey ---------------------------------------------------------------------------------------
    {
        LineEdit e;
        type(e, "ac");
        e.key(k(Key::left), 40);
        e.key(ch(U'b'), 40);
        P.eq("lineedit.insert_at_caret", e.buffer == "abc" && e.caret == 2, 1);
        P.eq("lineedit.space_inserts", e.key(k(Key::space), 40) && e.buffer == "ab c", 1);

        LineEdit full = edit("abcd", 4);
        P.eq("lineedit.full_refuses", !full.key(ch(U'e'), 4) && full.buffer == "abcd", 1);
        P.eq("lineedit.zero_max_refuses", !full.key(ch(U'e'), 0), 1);
        P.eq("lineedit.cmd_char_refused", !e.key(ch(U'z', cmd()), 40) && e.buffer == "ab c", 1);
        P.eq("lineedit.ctrl_char_refused", !e.key(ch(U'z', ctrl()), 40), 1);
        P.eq("lineedit.non_ascii_refused", !e.key(ch(U'é'), 40) && !e.key(ch(U'\t'), 40), 1);

        LineEdit b = edit("abc", 0);
        P.eq("lineedit.backspace_at_start", b.key(k(Key::backspace), 40) && b.buffer == "abc" && b.caret == 0, 1);
        b.caret = 2;
        P.eq("lineedit.backspace", b.key(k(Key::backspace), 40) && b.buffer == "ac" && b.caret == 1, 1);
        LineEdit w = edit("ab cd", 5);
        P.eq("lineedit.alt_backspace_word", w.key(k(Key::backspace, alt()), 40) && w.buffer == "ab " && w.caret == 3, 1);
        P.eq("lineedit.alt_backspace_over_spaces", w.key(k(Key::backspace, alt()), 40) && w.buffer.empty(), 1);
        LineEdit l = edit("hello world", 5);
        P.eq("lineedit.cmd_backspace_line", l.key(k(Key::backspace, cmd()), 40) && l.buffer == " world" && l.caret == 0, 1);
        LineEdit d = edit("abc", 1);
        P.eq("lineedit.delete_forward", d.key(k(Key::del), 40) && d.buffer == "ac" && d.caret == 1, 1);
        d.caret = 2;
        P.eq("lineedit.delete_at_end", d.key(k(Key::del), 40) && d.buffer == "ac", 1);

        LineEdit m = edit("abcd", 2);
        m.key(k(Key::left), 40);
        P.eq("lineedit.left", m.caret, 1);
        m.key(k(Key::right), 40);
        m.key(k(Key::right), 40);
        P.eq("lineedit.right", m.caret, 3);
        m.key(k(Key::left, cmd()), 40);
        P.eq("lineedit.cmd_left", m.caret, 0);
        m.key(k(Key::left), 40);
        P.eq("lineedit.left_stops", m.caret, 0);
        m.key(k(Key::right, cmd()), 40);
        P.eq("lineedit.cmd_right", m.caret, 4);
        m.key(k(Key::right), 40);
        P.eq("lineedit.right_stops", m.caret, 4);
        m.key(k(Key::home), 40);
        P.eq("lineedit.home", m.caret, 0);
        m.key(k(Key::end), 40);
        P.eq("lineedit.end", m.caret, 4);
        P.eq("lineedit.unhandled_keys", !m.key(k(Key::up), 40) && !m.key(k(Key::down), 40)
                                            && !m.key(k(Key::pageUp), 40) && !m.key(k(Key::pageDown), 40)
                                            && !m.key(k(Key::tab), 40), 1);

        LineEdit c = edit("abc", 99);
        c.key(k(Key::left), 40);
        P.eq("lineedit.caret_clamped_high", c.caret, 2);
        c.caret = -5;
        c.key(ch(U'x'), 40);
        P.eq("lineedit.caret_clamped_low", c.buffer == "xabc" && c.caret == 1, 1);

        // A caller that put UTF-8 in the buffer directly: key() never cuts a character in half.
        LineEdit u = edit("a\xC3\xA9" "b", 3);
        P.eq("lineedit.utf8_backspace_whole", u.key(k(Key::backspace), 40) && u.buffer == "ab" && u.caret == 1, 1);
        LineEdit u2 = edit("a\xC3\xA9" "b", 2);          // inside the é: moved back onto its first byte
        u2.key(k(Key::del), 40);
        P.eq("lineedit.utf8_caret_on_boundary", u2.buffer == "ab" && u2.caret == 1, 1);
        LineEdit u3 = edit("\xC3\xA9\xC3\xA9", 4);
        P.eq("lineedit.utf8_max_counts_characters", !u3.key(ch(U'x'), 2) && u3.key(ch(U'x'), 3), 1);
    }

    // ---- printable ------------------------------------------------------------------------------------------------------
    P.eq("printable.ascii_kept", funkgui::text::printable(" Drum~Bus_02! ") == " Drum~Bus_02! ", 1);
    P.eq("printable.codepoints_marked", funkgui::text::printable("Caf\xC3\xA9 \xE2\x98\x83") == "Caf? ?", 1);
    P.eq("printable.controls_marked", funkgui::text::printable("a\tb\x7F") == "a?b?", 1);
    P.eq("printable.malformed_marked", funkgui::text::printable("a\xFF" "b\xC3") == "a?b?", 1);
    P.eq("printable.empty", funkgui::text::printable("").empty(), 1);
    P.eq("printable.null", funkgui::text::printable(static_cast<const char*>(nullptr)).empty(), 1);
    P.eq("printable.std_string", funkgui::text::printable(std::string("a\xC3\xA9")) == "a?", 1);
    P.eq("printable.juce_string", funkgui::text::printable(juce::String(juce::CharPointer_UTF8("Caf\xC3\xA9\t.")))
                                      == juce::String("Caf?" "?."), 1);

    // ---- MenuLook ---------------------------------------------------------------------------------------------------------
    {
        const funkgui::Theme g = funkgui::Theme::graphite(), p = funkgui::Theme::paper();
        funkgui::MenuLook look(g);
        P.eq("menulook.background_ground", rgb(look.findColour(juce::PopupMenu::backgroundColourId)), rgb(g.ground));
        P.eq("menulook.text_ink100", rgb(look.findColour(juce::PopupMenu::textColourId)), rgb(g.ink100));
        P.eq("menulook.header_ink52", rgb(look.findColour(juce::PopupMenu::headerTextColourId)), rgb(g.ink52));
        P.eq("menulook.highlight_ink16", rgb(look.findColour(juce::PopupMenu::highlightedBackgroundColourId)),
             rgb(g.ink16));
        P.eq("menulook.highlight_text_ink100", rgb(look.findColour(juce::PopupMenu::highlightedTextColourId)),
             rgb(g.ink100));
        look.setTheme(p);
        P.eq("menulook.set_theme", rgb(look.findColour(juce::PopupMenu::backgroundColourId)) == rgb(p.ground)
                                       && rgb(look.findColour(juce::PopupMenu::textColourId)) == rgb(p.ink100), 1);
        const juce::Font f = look.getPopupMenuFont();
        std::printf("menulook.font  '%s' '%s' %.1f\n", f.getTypefaceName().toRawUTF8(), f.getTypefaceStyle().toRawUTF8(),
                    static_cast<double>(f.getHeight()));
        P.eq("menulook.font_14px", f.getHeight() == funkgui::MenuLook::kMenuPx && funkgui::MenuLook::kMenuPx == 14.0f, 1);
        // The embedded face: a typeface made from the bundled bytes (its name table is subset away, so the name is
        // empty), and monospaced, which the system fallback face is not.
        const float narrow = juce::GlyphArrangement::getStringWidth(f, "iiii");
        const float wide = juce::GlyphArrangement::getStringWidth(f, "MMMM");
        P.eq("menulook.font_bundled_face", f.getTypefacePtr() != nullptr && narrow > 0.0f && narrow == wide, 1);
    }

    // ---- SegmentedSelector over ParamCells ------------------------------------------------------------------------------
    {
        Host host;
        funkgui::GestureController g(host);
        Port port(0.5f);
        Cells model(port, { { "ECO", "Eco", "no oversampling" }, { "STD", "Std", nullptr }, { "HQ", nullptr, nullptr } },
                    -1);
        const std::vector<funkgui::Rect> rects = { { 0, 0, 32, 16 }, { 36, 0, 32, 16 }, { 72, 0, 26, 16 } };
        funkgui::SegmentedSelector sel(model, rects, funkgui::CellStyle::text, "QUALITY", { -60, 4 }, 100);

        P.eq("cells.param_active_nearest", model.active(), 1);
        P.eq("cells.param_host01", model.host01(0) == 0.0f && model.host01(1) == 0.5f && model.host01(2) == 1.0f, 1);
        P.eq("cells.cell_at", sel.cellAt({ 40, 8 }) == 1 && sel.cellAt({ 70, 8 }) == -1 && sel.contains({ 80, 1 }), 1);
        sel.pointerDown(pt(40, 8), g);
        P.eq("cells.select_active_writes_nothing", port.begins, 0);
        sel.pointerDown(pt(80, 8), g);
        P.eq("cells.select_on_down_one_tap", port.writes() == 1 && port.balanced() && port.value01() == 1.0f
                                                 && model.active() == 2, 1);
        sel.pointerDown(pt(10, 8, true), g);
        P.eq("cells.popup_menu_no_write", host.menus == 1 && port.writes() == 1, 1);
        sel.pointerDown(pt(200, 8), g);
        P.eq("cells.outside_nothing", port.writes() == 1 && host.menus == 1, 1);

        P.eq("cells.key_left", sel.key(k(Key::left), g) && model.active() == 1, 1);
        P.eq("cells.key_home", sel.key(k(Key::home), g) && model.active() == 0, 1);
        const int before = port.writes();
        P.eq("cells.key_left_at_start_consumed", sel.key(k(Key::left), g) && port.writes() == before, 1);
        P.eq("cells.key_up_is_next", sel.key(k(Key::up), g) && model.active() == 1, 1);
        P.eq("cells.key_down_is_previous", sel.key(k(Key::down), g) && model.active() == 0, 1);
        P.eq("cells.key_end", sel.key(k(Key::end), g) && model.active() == 2, 1);
        const int w = port.writes();
        P.eq("cells.key_return_space_consumed_no_write", sel.key(k(Key::enter), g) && sel.key(k(Key::space), g)
                                                             && port.writes() == w, 1);
        P.eq("cells.key_others_not_consumed", !sel.key(k(Key::tab), g) && !sel.key(k(Key::pageUp), g)
                                                  && !sel.key(ch(U'a'), g), 1);
        P.eq("cells.every_write_a_triple", port.balanced(), 1);

        std::vector<funkgui::A11yItem> items;
        sel.accessibility(items);
        const funkgui::A11yItem* grp = item(items, 100);
        const funkgui::A11yItem* c0 = item(items, 101);
        const funkgui::A11yItem* c2 = item(items, 103);
        P.eq("cells.a11y_count", static_cast<int64_t>(items.size()), 4);
        P.eq("cells.a11y_group", grp != nullptr && grp->role == funkgui::A11yRole::radioGroup && grp->title == "QUALITY"
                                     && grp->value == "HQ" && grp->bounds.x == 0.0f && grp->bounds.w == 98.0f, 1);
        P.eq("cells.a11y_buttons", c0 != nullptr && c2 != nullptr && c0->role == funkgui::A11yRole::radioButton
                                       && c0->parent == 100 && c0->title == "Eco" && c0->help == "no oversampling"
                                       && !c0->checked && c2->checked && c2->title == "HQ" && c0->checkable, 1);
        sel.setSpokenTitle("Quality");
        items.clear();
        sel.accessibility(items);
        P.eq("cells.a11y_spoken_title", items.front().title == "Quality", 1);

        P.eq("cells.a11y_press_cell", sel.a11yAction(102, funkgui::A11yAction::press, 0, g) && model.active() == 1, 1);
        P.eq("cells.a11y_group_set_value", sel.a11yAction(100, funkgui::A11yAction::setValue, 0.2, g)
                                               && model.active() == 0, 1);
        P.eq("cells.a11y_group_increment", sel.a11yAction(100, funkgui::A11yAction::increment, 0, g)
                                               && model.active() == 1, 1);
        const int m = host.menus;
        P.eq("cells.a11y_show_menu", sel.a11yAction(101, funkgui::A11yAction::showMenu, 0, g) && host.menus == m + 1, 1);
        P.eq("cells.a11y_foreign_id", !sel.a11yAction(99, funkgui::A11yAction::press, 0, g)
                                          && !sel.a11yAction(104, funkgui::A11yAction::press, 0, g), 1);

        P.eq("cells.cursor", sel.cursorAt({ 40, 8 }) == funkgui::Cursor::pointingHand
                                 && sel.cursorAt({ 70, 8 }) == funkgui::Cursor::normal, 1);
        P.eq("cells.settled_at_rest", sel.settled(), 1);
        sel.tick(1.0f / 60.0f, { 5, 8 });
        P.eq("cells.hover_unsettles", sel.settled(), 0);
        int n = 0;
        while (!sel.settled() && n < 600)
        {
            sel.tick(1.0f / 60.0f, { 5, 8 });
            ++n;
        }
        P.in("cells.hover_settles_ticks", n, 1, 60);
        P.eq("cells.bounds_union", sel.bounds().x == 0.0f && sel.bounds().right() == 98.0f && sel.bounds().h == 16.0f, 1);
    }

    // Disabled cells: refused on click, skipped by keys, disabled in a11y.
    {
        Host host;
        funkgui::GestureController g(host);
        Port port(0.0f);
        Cells model(port, { { "OFF" }, { "5 MS" }, { "20 MS" } }, 2);
        funkgui::SegmentedSelector sel(model, { { 0, 0, 32, 16 }, { 36, 0, 38, 16 }, { 78, 0, 44, 16 } },
                                       funkgui::CellStyle::boxed, nullptr, {}, 200);
        sel.pointerDown(pt(90, 8), g);
        P.eq("cells.disabled_click_refused", port.begins == 0 && model.active() == 0, 1);
        P.eq("cells.disabled_cursor_normal", sel.cursorAt({ 90, 8 }) == funkgui::Cursor::normal, 1);
        sel.key(k(Key::end), g);
        P.eq("cells.end_skips_disabled", model.active(), 1);
        sel.key(k(Key::right), g);
        P.eq("cells.right_stops_before_disabled", model.active() == 1 && port.writes() == 1, 1);
        P.eq("cells.a11y_disabled_press_refused", sel.a11yAction(203, funkgui::A11yAction::press, 0, g)
                                                      && model.active() == 1, 1);
        std::vector<funkgui::A11yItem> items;
        sel.accessibility(items);
        const funkgui::A11yItem* c2 = item(items, 203);
        P.eq("cells.a11y_disabled", c2 != nullptr && !c2->enabled && c2->help == "NEEDS HQ" && items.front().title.empty(),
             1);
        sel.tick(1.0f / 60.0f, { 90, 8 });
        P.eq("cells.disabled_no_hover", sel.settled(), 1);
        Port nan(std::numeric_limits<float>::quiet_NaN());
        Cells nanModel(nan, { { "A" }, { "B" } });
        P.eq("cells.nan_value_is_first", nanModel.active(), 0);
    }

    // ---- Preferences: PrefCells and ThemeCells, against a fresh scratch store ---------------------------------------
    {
        const char* caller = funkgui::env("PREFS_DIR");
        const juce::File parent = caller != nullptr ? juce::File::getCurrentWorkingDirectory().getChildFile(caller)
                                                    : juce::File::getSpecialLocation(juce::File::tempDirectory);
        parent.createDirectory();
        const juce::File scratch = parent.getNonexistentChildFile("fg-lineedit-prefs", "", false);
        scratch.createDirectory();
        T::setEnv(FUNKGUI_ENV_PREFIX "PREFS_DIR", scratch.getFullPathName().toRawUTF8());
        funkgui::envReload();
        auto& prefs = funkgui::UiPreferences::get();
        P.eq("prefcells.scratch_store", prefs.file().isAChildOf(scratch), 1);

        Host host;
        funkgui::GestureController g(host);
        funkgui::PrefCells scale("meterScaleDb", { 12, 24, 48, 72 },
                                 { { "12" }, { "24" }, { "48" }, { "72", "72 decibels" } }, 48);
        P.eq("prefcells.missing_is_fallback", scale.active() == 2 && scale.value() == 48, 1);
        P.eq("prefcells.texts", std::string(scale.label(3)) == "72" && std::string(scale.spoken(3)) == "72 decibels"
                                    && std::string(scale.spoken(0)) == "12" && scale.port() == nullptr, 1);
        const uint32_t r0 = prefs.revision();
        scale.select(1, g);
        P.eq("prefcells.select_writes_pref", scale.active() == 1 && prefs.getInt("meterScaleDb", 0, 0, 100) == 24
                                                 && prefs.revision() == r0 + 1, 1);
        scale.select(1, g);
        P.eq("prefcells.select_same_no_write", prefs.revision(), r0 + 1);
        prefs.setInt("meterScaleDb", 30);
        P.eq("prefcells.foreign_value_is_fallback", scale.active(), 2);
        funkgui::PrefCells span("historySpanTenths", { 25, 50, 100, 200 }, { { "2.5" }, { "5" }, { "10" }, { "20" } },
                                7);
        P.eq("prefcells.fallback_outside_is_first", span.active(), 0);
        funkgui::PrefCells ragged("x", { 1, 2, 3 }, { { "A" }, { "B" } }, 1);
        P.eq("prefcells.paired_by_index", ragged.count(), 2);

        funkgui::SegmentedSelector sel(scale, { { 0, 0, 20, 16 }, { 24, 0, 20, 16 }, { 48, 0, 20, 16 }, { 72, 0, 20, 16 } },
                                       funkgui::CellStyle::text, "SCALE", {}, 300);
        sel.pointerDown(pt(80, 8), g);
        P.eq("prefcells.click_writes_pref", prefs.getInt("meterScaleDb", 0, 0, 100), 72);
        sel.pointerDown(pt(80, 8, true), g);
        P.eq("prefcells.popup_no_menu_no_port", host.menus, 0);

        prefs.setTheme(0);
        funkgui::ThemeCells theme({ { 0, 0, 72, 16 }, { 76, 0, 54, 16 } }, nullptr, {}, 400);
        std::vector<funkgui::A11yItem> items;
        theme.selector().accessibility(items);
        P.eq("themecells.a11y", items.size() == 3 && items[0].title == "Theme" && items[0].value == "Graphite theme"
                                    && items[1].title == "Graphite theme" && items[2].title == "Paper theme"
                                    && items[1].checked && !items[2].checked, 1);
        const uint32_t r1 = prefs.revision();
        theme.selector().pointerDown(pt(90, 8), g);
        P.eq("themecells.click_sets_theme", prefs.theme() == 1 && prefs.revision() == r1 + 1, 1);
        theme.selector().pointerDown(pt(90, 8), g);
        P.eq("themecells.click_same_no_write", prefs.revision(), r1 + 1);
        theme.selector().key(k(Key::home), g);
        P.eq("themecells.key_home", prefs.theme(), 0);
        scratch.deleteRecursively();
    }

    // ---- LatchToggle over ParamToggle -------------------------------------------------------------------------------------
    {
        Host host;
        funkgui::GestureController g(host);
        Port port(0.0f);
        Toggle model(port, true);
        funkgui::LatchToggle latch(model, { 10, 10, 88, 36 }, "DELTA", 500);

        latch.pointerDown(pt(20, 20), g);
        P.eq("latch.down_arms_no_write", latch.armed() && port.begins == 0, 1);
        latch.pointerUp(pt(20, 20), g);
        P.eq("latch.up_inside_commits_one_tap", !latch.armed() && port.writes() == 1 && port.balanced() && model.on(), 1);

        latch.pointerDown(pt(20, 20), g);
        latch.pointerDrag(pt(200, 20));
        P.eq("latch.drag_off_disarms", latch.armed(), 0);
        latch.pointerDrag(pt(20, 20));
        latch.pointerUp(pt(20, 20), g);
        P.eq("latch.drag_off_cancels", port.writes() == 1 && model.on(), 1);
        latch.pointerDown(pt(20, 20), g);
        latch.pointerUp(pt(200, 20), g);
        P.eq("latch.up_outside_cancels", port.writes() == 1 && !latch.armed(), 1);
        latch.pointerDown(pt(200, 20), g);
        P.eq("latch.down_outside_not_armed", latch.armed(), 0);
        latch.pointerDown(pt(20, 20, true), g);
        latch.pointerUp(pt(20, 20), g);
        P.eq("latch.popup_menu_no_arm_no_write", host.menus == 1 && port.writes() == 1 && !latch.armed(), 1);

        P.eq("latch.key_return_toggles", latch.key(k(Key::enter), g) && !model.on() && port.writes() == 2, 1);
        P.eq("latch.key_space_toggles", latch.key(k(Key::space), g) && model.on() && port.writes() == 3, 1);
        P.eq("latch.key_others_not_consumed", !latch.key(k(Key::tab), g) && !latch.key(k(Key::left), g), 1);
        P.eq("latch.a11y_toggle", latch.a11yAction(500, funkgui::A11yAction::toggle, g) && !model.on(), 1);
        P.eq("latch.a11y_press", latch.a11yAction(500, funkgui::A11yAction::press, g) && model.on(), 1);
        P.eq("latch.a11y_foreign_id", !latch.a11yAction(501, funkgui::A11yAction::press, g) && model.on(), 1);
        P.eq("latch.a11y_show_menu", latch.a11yAction(500, funkgui::A11yAction::showMenu, g) && host.menus == 2, 1);
        P.eq("latch.every_write_a_triple", port.balanced(), 1);

        std::vector<funkgui::A11yItem> items;
        latch.accessibility(items);
        P.eq("latch.a11y_item", items.size() == 1 && items[0].role == funkgui::A11yRole::toggleButton
                                    && items[0].title == "DELTA" && items[0].checkable && items[0].checked
                                    && items[0].enabled && items[0].help.empty() && latch.reason() == nullptr, 1);
        P.eq("latch.cursor", latch.cursorAt({ 20, 20 }) == funkgui::Cursor::pointingHand
                                 && latch.cursorAt({ 200, 20 }) == funkgui::Cursor::normal, 1);
        latch.tick(1.0f / 60.0f, { 20, 20 });
        P.eq("latch.hover_unsettles", latch.settled(), 0);
        int n = 0;
        while (!latch.settled() && n < 600)
        {
            latch.tick(1.0f / 60.0f, { 20, 20 });
            ++n;
        }
        P.in("latch.hover_settles_ticks", n, 1, 60);
    }

    // A disabled latch refuses everything but the host menu.
    {
        Host host;
        funkgui::GestureController g(host);
        Port port(0.0f);
        Toggle model(port, false);
        funkgui::LatchToggle latch(model, { 0, 0, 88, 36 }, "LISTEN", 600);
        latch.pointerDown(pt(10, 10), g);
        latch.pointerUp(pt(10, 10), g);
        P.eq("latch.disabled_click_refused", !latch.armed() && port.begins == 0, 1);
        P.eq("latch.disabled_key_consumed_no_write", latch.key(k(Key::enter), g) && port.begins == 0, 1);
        P.eq("latch.disabled_a11y_refused", latch.a11yAction(600, funkgui::A11yAction::press, g) && port.begins == 0, 1);
        latch.pointerDown(pt(10, 10, true), g);
        P.eq("latch.disabled_menu_still_works", host.menus, 1);
        std::vector<funkgui::A11yItem> items;
        latch.accessibility(items);
        P.eq("latch.disabled_a11y_reason", items.size() == 1 && !items[0].enabled
                                               && items[0].help == "NO SIDECHAIN BUS CONNECTED"
                                               && std::string(latch.reason()) == "NO SIDECHAIN BUS CONNECTED", 1);
        P.eq("latch.disabled_cursor_normal", latch.cursorAt({ 10, 10 }) == funkgui::Cursor::normal, 1);
        latch.tick(1.0f / 60.0f, { 10, 10 });
        P.eq("latch.disabled_no_hover", latch.settled(), 1);
    }

    return P.finish();
}

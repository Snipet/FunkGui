// FUNKGUI_TEST name=fg.desktop.menu timeout=120 gpu=1 links=desktop
//
// fg.desktop.menu (v0.28.0): desktop/MenuOverlay.h, the menu DesktopHost draws with the Canvas for
// HostServices::showMenu, driven with no window: a Panel that owns one and hands it its events runs in a HeadlessHost
// (which sends the pointer, the wheel and the keys, and records the frame), and the frame is read as a fingerprint
// and through the software rasteriser. (It links FunkGui::desktop, where the overlay lives; it opens no window and
// needs no display.)
//   request.*   a request that fails validMenu() is refused and its callback dropped unrun; a second show() replaces
//               the menu showing, whose callback does not run
//   layout.*    the rows' kinds and heights (MenuLook's metrics), which rows can be highlighted, the panel's size,
//               minWidth
//   place.*     under the anchor with the gap; over it when there is more room there; moved inside the bounds; a
//               menu taller than the bounds scrolls; selectedId is highlighted and in view; setBounds places again
//   pointer.*   the row under the pointer is highlighted unless it is disabled, a separator or a header; a submenu
//               opens beside its row and closes when another row is highlighted; a release on an item chooses it; a
//               press outside dismisses with 0; a release outside does nothing; a disabled item is not chosen
//   keys.*      Down and Up wrap over the rows that can be highlighted; Home, End; Right opens a submenu with its first
//               row highlighted, Left closes it; Return chooses; Space chooses; Escape dismisses with 0; typing moves
//               to the next row that starts with the prefix, the same letter again to the next, a pause starts over
//   callback.*  runs once, after the menu closed (it may show another); never after dismiss()
//   wheel.*     scrolls a tall menu, and the row under the pointer changes with it
//   draw.*      nothing while no menu shows; the panel's surface, the highlighted row's pill and the Panel's own
//               drawing outside the menu, as pixels of the software rasteriser, in both themes; the revision moves
//               with every change of what is drawn and not otherwise
//   frame.*     golden rows: the fingerprint of a frame with a menu, a submenu and every kind of row, at dpi 1 and 2
// The frames are also written as PNGs into the test's sandbox (menu-<theme>.png), for a person to look at.

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/Fingerprint.h>
#include <funkgui/canvas/SoftRaster.h>
#include <funkgui/core/Theme.h>
#include <funkgui/desktop/MenuOverlay.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Panel.h>
#include <funkgui/test/Harness.h>
#include <funkgui/text/FontService.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace T = funkgui::test;
using funkgui::Col;
using funkgui::MenuItem;
using funkgui::MenuRequest;
using funkgui::Rect;
using funkgui::desktop::MenuOverlay;

namespace
{
    constexpr int kW = 480, kH = 320;

    // A Panel with a menu of its own: what DesktopHost does with one, without the window.
    class MenuPanel final : public funkgui::Panel
    {
    public:
        MenuOverlay menu;
        funkgui::HostServices* host = nullptr;
        int moves = 0;                               // pointer moves the menu did not take

        void attach(funkgui::HostServices& s) override { host = &s; }
        int  width() const override { return kW; }
        int  height() const override { return kH; }
        void tick(float) override {}
        void draw(funkgui::Canvas& c, const funkgui::Theme& th) override
        {
            c.rrect(16.0f, 16.0f, 120.0f, 28.0f, 4.0f, th.ink16);       // something of the Panel's own under the menu
            menu.draw(c);
        }
        bool wantsFullRate() const override { return false; }
        void accessibility(std::vector<funkgui::A11yItem>&) const override {}
        uint32_t a11yRevision() const override { return 0; }
        void a11yAction(uint32_t, funkgui::A11yAction, double) override {}
        void closeGestures() override {}

        void pointerMove(const funkgui::PointerEvent& e) override
        {
            if (!menu.pointerMove(e.x, e.y))
                ++moves;
        }
        void pointerDown(const funkgui::PointerEvent& e) override { menu.pointerDown(e.x, e.y); }
        void pointerUp(const funkgui::PointerEvent& e) override { menu.pointerUp(e.x, e.y); }
        bool wheel(const funkgui::WheelEvent& e) override { return menu.wheel(e.x, e.y, e.dy); }
        bool key(const funkgui::KeyEvent& e) override { return menu.key(e, host != nullptr ? host->nowSeconds() : 0.0); }
    };

    // Every kind of row: a header, items (one ticked, one a radio's dot, one with a shortcut, one disabled), a
    // separator, a submenu and a disabled submenu.
    MenuRequest sample(const funkgui::Theme& theme)
    {
        MenuRequest r;
        r.theme = theme;
        r.anchor = { 40.0f, 40.0f, 100.0f, 20.0f };
        r.items.push_back({ .label = "Preset", .header = true });
        r.items.push_back({ .id = 1, .label = "Alpha", .shortcut = "Cmd+A" });
        r.items.push_back({ .id = 2, .label = "Beta", .checked = true });
        r.items.push_back({ .id = 3, .label = "Bravo", .checked = true, .radio = true });
        r.items.push_back({ .id = 4, .label = "Gamma", .enabled = false });
        r.items.push_back({ .separator = true });
        MenuItem more{ .label = "More" };
        more.submenu.push_back({ .id = 10, .label = "One" });
        more.submenu.push_back({ .id = 11, .label = "Two", .enabled = false });
        more.submenu.push_back({ .id = 12, .label = "Three" });
        r.items.push_back(more);
        MenuItem none{ .label = "Nothing", .enabled = false };
        none.submenu.push_back({ .id = 20, .label = "Never" });
        r.items.push_back(none);
        r.items.push_back({ .id = 5, .label = "Delta" });
        return r;
    }

    // Row indexes of sample()'s top level.
    enum : int { kHeaderRow = 0, kAlpha, kBeta, kBravo, kGamma, kRule, kMore, kNothing, kDelta, kRows };

    struct Answer
    {
        int calls = 0, id = -1;
        bool showingWhenCalled = false;
    };

    funkgui::MenuCallback into(Answer& a, const MenuOverlay& menu)
    {
        return [&a, &menu](int id)
        {
            ++a.calls;
            a.id = id;
            a.showingWhenCalled = menu.showing();
        };
    }

    funkgui::Point centre(const Rect& r) { return { r.centreX(), r.centreY() }; }

    bool near(float a, float b) { return std::fabs(a - b) < 0.01f; }

    const uint8_t* px(const funkgui::Image& img, float dpi, float lx, float ly)
    {
        const auto x = static_cast<std::size_t>(lx * dpi), y = static_cast<std::size_t>(ly * dpi);
        return &img.rgba[(y * static_cast<std::size_t>(img.w) + x) * 4u];
    }

    bool isCol(const uint8_t* p, Col c) { return std::abs(p[0] - c.r) <= 1 && std::abs(p[1] - c.g) <= 1 && std::abs(p[2] - c.b) <= 1; }
}

int main(int argc, char** argv)
{
    T::Probe P("fg.desktop.menu", "", argc, argv);
    const funkgui::Theme graphite = funkgui::Theme::byIndex(0);

    // ---- requests ---------------------------------------------------------------------------------------------------
    {
        MenuOverlay menu;
        auto alive = std::make_shared<int>(0);
        int ran = 0;
        MenuRequest empty;
        P.eq("request.empty_refused", menu.show(empty, [alive, &ran](int) { ++ran; }, kW, kH), 0);
        P.eq("request.refused_callback_dropped", alive.use_count() == 1 && ran == 0 && !menu.showing(), 1);
        MenuRequest rules;
        rules.items.push_back({ .separator = true });
        rules.items.push_back({ .label = "Title", .header = true });
        P.eq("request.nothing_to_choose_refused", menu.show(rules, [](int) {}, kW, kH), 0);
        MenuRequest noId;
        noId.items.push_back({ .id = 0, .label = "No id" });
        P.eq("request.item_without_id_refused", menu.show(noId, [](int) {}, kW, kH), 0);

        Answer first, second;
        P.eq("request.shown", menu.show(sample(graphite), into(first, menu), kW, kH) && menu.showing(), 1);
        P.eq("request.second_replaces", menu.show(sample(graphite), into(second, menu), kW, kH), 1);
        menu.key({ funkgui::Key::escape, {}, 0 }, 0.0);
        P.eq("request.replaced_callback_never_runs", first.calls == 0 && second.calls == 1 && second.id == 0, 1);
        Answer third;
        menu.show(sample(graphite), into(third, menu), kW, kH);
        P.eq("request.refused_second_still_replaces", !menu.show(empty, [](int) {}, kW, kH) && !menu.showing()
                                                          && third.calls == 0, 1);
    }

    // ---- layout and placement ---------------------------------------------------------------------------------------
    {
        MenuOverlay menu;
        Answer a;
        MenuRequest r = sample(graphite);
        menu.show(r, into(a, menu), kW, kH);
        const MenuOverlay::Level& top = menu.levels().front();
        P.eq("layout.one_level", static_cast<int64_t>(menu.levels().size()), 1);
        P.eq("layout.rows", static_cast<int64_t>(top.rows.size()), kRows);
        bool kinds = top.rows.size() == kRows;
        for (int i = 0; kinds && i < kRows; ++i)
        {
            const MenuOverlay::Row& row = top.rows[static_cast<std::size_t>(i)];
            const MenuOverlay::RowKind want = i == kHeaderRow ? MenuOverlay::RowKind::header
                                            : i == kRule ? MenuOverlay::RowKind::separator
                                                         : MenuOverlay::RowKind::item;
            const float h = i == kRule ? MenuOverlay::kSeparatorPx : MenuOverlay::kRowPx;
            const bool selectable = i == kAlpha || i == kBeta || i == kBravo || i == kMore || i == kDelta;
            kinds = row.kind == want && near(row.h, h) && row.selectable == selectable && row.item == i
                 && row.submenu == (i == kMore || i == kNothing);
        }
        P.eq("layout.kinds_heights_selectable", kinds, 1);
        const float content = 8.0f * MenuOverlay::kRowPx + MenuOverlay::kSeparatorPx;
        P.near("layout.content_height", top.contentH, content, 0.01);
        P.near("layout.panel_height", top.panel.h, content + 2.0f * MenuOverlay::kPadPx, 0.01);
        P.ge("layout.panel_at_least_min", top.panel.w, MenuOverlay::kMinPanelPx);
        P.eq("layout.panel_width_whole", near(top.panel.w, std::floor(top.panel.w)), 1);
        std::printf("NOTE     the sample's panel is %.0f x %.0f px\n", static_cast<double>(top.panel.w),
                    static_cast<double>(top.panel.h));

        P.near("place.left_on_anchor", top.panel.x, 40.0, 0.01);
        P.near("place.under_anchor", top.panel.y, 60.0 + MenuOverlay::kGapPx, 0.01);
        P.eq("place.nothing_highlighted", top.highlighted, -1);
        const Rect alpha = menu.rowRect(0, kAlpha);
        P.eq("place.row_rect", near(alpha.x, top.panel.x + MenuOverlay::kInsetPx)
                                   && near(alpha.y, top.panel.y + MenuOverlay::kPadPx + MenuOverlay::kHeaderPx)
                                   && near(alpha.w, top.panel.w - 2.0f * MenuOverlay::kInsetPx)
                                   && near(alpha.h, MenuOverlay::kRowPx), 1);
        P.eq("place.row_rect_of_none", menu.rowRect(0, 99).isEmpty() && menu.rowRect(3, 0).isEmpty(), 1);

        // No room under the anchor, more over it: the panel's bottom is the gap over the anchor.
        r.anchor = { 40.0f, 280.0f, 100.0f, 20.0f };
        menu.show(r, into(a, menu), kW, kH);
        P.near("place.over_anchor", menu.levels().front().panel.bottom(), 280.0 - MenuOverlay::kGapPx, 0.01);
        // An anchor at the right edge: moved left until it is inside.
        r.anchor = { 470.0f, 40.0f, 8.0f, 20.0f };
        menu.show(r, into(a, menu), kW, kH);
        P.near("place.moved_inside", menu.levels().front().panel.right(), kW - MenuOverlay::kEdgePx, 0.01);
        // minWidth and selectedId.
        r.anchor = { 40.0f, 40.0f, 100.0f, 20.0f };
        r.minWidth = 300.0f;
        r.selectedId = 5;
        menu.show(r, into(a, menu), kW, kH);
        P.near("layout.min_width", menu.levels().front().panel.w, 300.0, 0.01);
        P.eq("place.selected_highlighted", menu.levels().front().highlighted, kDelta);
        r.selectedId = 4;                            // a disabled item is shown, not highlighted
        menu.show(r, into(a, menu), kW, kH);
        P.eq("place.disabled_selected_not_highlighted", menu.levels().front().highlighted, -1);

        // Taller than the bounds: as tall as they allow, and it scrolls; the selected row is in view.
        MenuRequest tall;
        tall.theme = graphite;
        tall.anchor = { 10.0f, 10.0f, 50.0f, 10.0f };
        for (int i = 1; i <= 40; ++i)
            tall.items.push_back({ .id = i, .label = "Row " + std::to_string(i) });
        tall.selectedId = 30;
        menu.show(tall, into(a, menu), kW, kH);
        const MenuOverlay::Level& t = menu.levels().front();
        P.near("place.tall_height", t.panel.h, kH - 2.0f * MenuOverlay::kEdgePx, 0.01);
        P.near("place.tall_top", t.panel.y, MenuOverlay::kEdgePx, 0.01);
        P.eq("place.tall_scrolled_to_selected", t.scroll > 0.0f && t.highlighted == 29, 1);
        const Rect sel = menu.rowRect(0, 29);
        P.eq("place.selected_in_view", sel.y >= t.panel.y + MenuOverlay::kPadPx - 0.01f
                                           && sel.bottom() <= t.panel.bottom() - MenuOverlay::kPadPx + 0.01f, 1);
        // The wheel: a notch towards the user scrolls three rows on; the row under the pointer follows.
        const float before = t.scroll;
        const uint32_t rev = menu.revision();
        menu.pointerMove(60.0f, 100.0f);
        const int under = t.highlighted;
        menu.wheel(60.0f, 100.0f, -50.0f / 256.0f);
        P.near("wheel.three_rows", t.scroll - before, 3.0f * MenuOverlay::kRowPx, 0.01);
        P.eq("wheel.row_under_pointer_follows", t.highlighted, under + 3);
        P.eq("wheel.revision_moved", menu.revision() != rev, 1);
        menu.wheel(60.0f, 100.0f, 1000.0f);
        P.near("wheel.stops_at_the_top", t.scroll, 0.0, 0.001);
        // The bounds grow: placed again, and no scroll is needed.
        menu.setBounds(kW, 2000.0f);
        P.near("place.bounds_grew", menu.levels().front().panel.h, 40.0f * MenuOverlay::kRowPx + 2.0f * MenuOverlay::kPadPx,
               0.01);
    }

    // ---- the pointer, through a HeadlessHost ------------------------------------------------------------------------
    {
        MenuPanel panel;
        funkgui::HeadlessHost host(panel, 0, 1.0f);
        Answer a;
        host.move(300.0f, 300.0f);
        P.eq("pointer.panels_while_no_menu", panel.moves, 1);
        panel.menu.show(sample(graphite), into(a, panel.menu), kW, kH);
        const auto& levels = panel.menu.levels();
        const auto at = [&](int level, int row) { return centre(panel.menu.rowRect(level, row)); };

        host.move(at(0, kAlpha).x, at(0, kAlpha).y);
        P.eq("pointer.menus_while_showing", panel.moves, 1);
        P.eq("pointer.highlights", levels[0].highlighted, kAlpha);
        host.move(at(0, kGamma).x, at(0, kGamma).y);
        P.eq("pointer.disabled_not_highlighted", levels[0].highlighted, -1);
        host.move(at(0, kRule).x, at(0, kRule).y);
        P.eq("pointer.separator_not_highlighted", levels[0].highlighted, -1);
        host.move(at(0, kHeaderRow).x, at(0, kHeaderRow).y);
        P.eq("pointer.header_not_highlighted", levels[0].highlighted, -1);
        host.move(at(0, kNothing).x, at(0, kNothing).y);
        P.eq("pointer.disabled_submenu_opens_nothing", levels.size() == 1 && levels[0].highlighted == -1, 1);

        host.move(at(0, kMore).x, at(0, kMore).y);
        P.eq("pointer.submenu_opens", levels.size() == 2 && levels[0].open == kMore && levels[0].highlighted == kMore, 1);
        if (levels.size() == 2)
        {
            P.near("pointer.submenu_beside", levels[1].panel.x, levels[0].panel.right() - MenuOverlay::kOverlapPx, 0.01);
            P.near("pointer.submenu_at_its_row", levels[1].panel.y, panel.menu.rowRect(0, kMore).y - MenuOverlay::kPadPx,
                   0.01);
            host.move(at(1, 2).x, at(1, 2).y);
            P.eq("pointer.submenu_row", levels[1].highlighted == 2 && levels[0].highlighted == kMore, 1);
            host.move(at(1, 1).x, at(1, 1).y);
            P.eq("pointer.submenu_disabled_row", levels[1].highlighted, -1);
        }
        host.move(at(0, kDelta).x, at(0, kDelta).y);
        P.eq("pointer.other_row_closes_submenu", levels.size() == 1 && levels[0].open == -1, 1);

        // A click on a disabled item and one on the header choose nothing; one on an item chooses it.
        host.click(at(0, kGamma).x, at(0, kGamma).y);
        host.click(at(0, kHeaderRow).x, at(0, kHeaderRow).y);
        P.eq("pointer.disabled_not_chosen", a.calls == 0 && panel.menu.showing(), 1);
        host.click(at(0, kBeta).x, at(0, kBeta).y);
        P.eq("pointer.release_chooses", a.calls == 1 && a.id == 2, 1);
        P.eq("callback.after_the_menu_closed", !a.showingWhenCalled && !panel.menu.showing(), 1);

        // In a submenu.
        a = {};
        panel.menu.show(sample(graphite), into(a, panel.menu), kW, kH);
        host.move(at(0, kMore).x, at(0, kMore).y);
        host.click(at(0, kMore).x, at(0, kMore).y);              // a click on the submenu's row keeps it open
        P.eq("pointer.submenu_row_click_keeps_open", a.calls == 0 && levels.size() == 2, 1);
        host.click(at(1, 2).x, at(1, 2).y);
        P.eq("pointer.submenu_item_chosen", a.calls == 1 && a.id == 12, 1);

        // Outside: a release does nothing (the click that opened the menu), a press dismisses with 0.
        a = {};
        panel.menu.show(sample(graphite), into(a, panel.menu), kW, kH);
        panel.pointerUp({ 400.0f, 300.0f });
        P.eq("pointer.release_outside_nothing", a.calls == 0 && panel.menu.showing(), 1);
        panel.pointerDown({ 400.0f, 300.0f });
        P.eq("pointer.press_outside_dismisses", a.calls == 1 && a.id == 0 && !panel.menu.showing(), 1);

        // dismiss(): the callback never runs.
        a = {};
        panel.menu.show(sample(graphite), into(a, panel.menu), kW, kH);
        panel.menu.dismiss();
        host.click(at(0, kBeta).x, at(0, kBeta).y);
        P.eq("callback.never_after_dismiss", a.calls == 0 && !panel.menu.showing(), 1);

        // A callback may show another menu.
        Answer inner;
        int outer = 0;
        panel.menu.show(sample(graphite), [&](int id)
        {
            outer = id;
            panel.menu.show(sample(graphite), into(inner, panel.menu), kW, kH);
        }, kW, kH);
        host.click(at(0, kAlpha).x, at(0, kAlpha).y);
        P.eq("callback.may_show_another", outer == 1 && panel.menu.showing() && inner.calls == 0, 1);
        panel.menu.dismiss();
    }

    // ---- the keys ---------------------------------------------------------------------------------------------------
    {
        MenuPanel panel;
        funkgui::HeadlessHost host(panel, 0, 1.0f);
        Answer a;
        const auto& levels = panel.menu.levels();
        panel.menu.show(sample(graphite), into(a, panel.menu), kW, kH);
        host.keys("down");
        P.eq("keys.down_first", levels[0].highlighted, kAlpha);
        host.keys("down,down,down");
        P.eq("keys.down_skips_disabled_and_rule", levels[0].highlighted, kMore);
        host.keys("down,down");
        P.eq("keys.down_wraps", levels[0].highlighted, kAlpha);
        host.keys("up");
        P.eq("keys.up_wraps", levels[0].highlighted, kDelta);
        host.keys("home");
        P.eq("keys.home", levels[0].highlighted, kAlpha);
        host.keys("end");
        P.eq("keys.end", levels[0].highlighted, kDelta);
        host.keys("up,right");
        P.eq("keys.right_opens", levels.size() == 2 && levels[1].highlighted == 0, 1);
        host.keys("down");
        P.eq("keys.submenu_down_skips_disabled", levels.size() == 2 && levels[1].highlighted == 2, 1);
        host.keys("left");
        P.eq("keys.left_closes", levels.size() == 1 && levels[0].highlighted == kMore, 1);
        host.keys("return");
        P.eq("keys.return_opens_submenu", levels.size() == 2 && a.calls == 0, 1);
        host.keys("return");
        P.eq("keys.return_chooses", a.calls == 1 && a.id == 10 && !panel.menu.showing(), 1);

        a = {};
        panel.menu.show(sample(graphite), into(a, panel.menu), kW, kH);
        host.keys("down,down,space");
        P.eq("keys.space_chooses", a.calls == 1 && a.id == 2, 1);
        a = {};
        panel.menu.show(sample(graphite), into(a, panel.menu), kW, kH);
        host.keys("down,escape");
        P.eq("keys.escape_dismisses", a.calls == 1 && a.id == 0 && !panel.menu.showing(), 1);
        a = {};
        panel.menu.show(sample(graphite), into(a, panel.menu), kW, kH);
        host.keys("return");
        P.eq("keys.return_on_nothing", a.calls == 0 && panel.menu.showing(), 1);

        // Type-ahead: b -> Beta; b again -> Bravo; b again wraps to Beta; a pause, then "br" -> Bravo; d -> Delta;
        // g (Gamma is disabled) moves nowhere.
        host.keys("b");
        P.eq("keys.type_b", levels[0].highlighted, kBeta);
        host.keys("b");
        P.eq("keys.type_b_again", levels[0].highlighted, kBravo);
        host.keys("b");
        P.eq("keys.type_b_wraps", levels[0].highlighted, kBeta);
        host.tick(90);                               // a second and a half of the host's clock
        host.keys("b,r");
        P.eq("keys.type_prefix", levels[0].highlighted, kBravo);
        host.tick(90);
        host.keys("shift+D");
        P.eq("keys.type_case_aside", levels[0].highlighted, kDelta);
        host.tick(90);
        host.keys("g");
        P.eq("keys.type_disabled_no_move", levels[0].highlighted, kDelta);
        host.tick(90);
        host.keys("cmd+a");
        P.eq("keys.chord_is_no_text", levels[0].highlighted, kDelta);
        panel.menu.dismiss();
    }

    // ---- drawing ----------------------------------------------------------------------------------------------------
    for (int theme = 0; theme < funkgui::Theme::kCount; ++theme)
    {
        const funkgui::Theme th = funkgui::Theme::byIndex(theme);
        const std::string t = "theme" + std::to_string(theme);
        for (const float dpi : { 1.0f, 2.0f })
        {
            MenuPanel panel;
            funkgui::HeadlessHost host(panel, theme, dpi);
            Answer a;
            const std::size_t bare = host.draw().prims.size();
            panel.menu.show(sample(th), into(a, panel.menu), kW, kH);
            const funkgui::Point more = centre(panel.menu.rowRect(0, kMore));
            host.move(more.x, more.y);
            const funkgui::Point three = centre(panel.menu.rowRect(1, 2));
            host.move(three.x, three.y);
            const uint32_t rev = panel.menu.revision();
            host.move(three.x + 1.0f, three.y);      // the same row: nothing changes
            const funkgui::PrimList& list = host.draw();
            const std::string d = dpi > 1.5f ? ".dpi2" : ".dpi1";
            if (dpi < 1.5f)
            {
                P.eq("draw." + t + ".more_than_the_panel", list.prims.size() > bare, 1);
                P.eq("draw." + t + ".no_glyph_missing", static_cast<int64_t>(list.missingGlyphs), 0);
                P.eq("draw." + t + ".revision_still", panel.menu.revision() == rev, 1);
            }
            funkgui::addMetrics(P, "frame." + t + d, funkgui::fingerprint(list));

            const funkgui::Image img = funkgui::rasterise(list, funkgui::FontService::get().atlas(), 1);
            const Rect top = panel.menu.levels()[0].panel, sub = panel.menu.levels()[1].panel;
            // The surface: MenuLook's mix of the theme's tokens (src/juce/MenuLook.cpp paletteFor).
            const bool dark = theme == 0;
            const Col surface = dark ? funkgui::mix(th.ground, th.ink100, 0.05f)
                                     : funkgui::mix(th.ground, Col{ 255, 255, 255, 255 }, 0.72f);
            const Col hover = dark ? funkgui::mix(th.ground, th.ink100, 0.14f) : funkgui::mix(surface, th.ink100, 0.08f);
            const Rect delta = panel.menu.rowRect(0, kDelta), moreRow = panel.menu.rowRect(0, kMore),
                       threeRow = panel.menu.rowRect(1, 2);
            P.eq("draw." + t + d + ".surface", isCol(px(img, dpi, delta.x + 3.0f, delta.y + 3.0f), surface), 1);
            P.eq("draw." + t + d + ".submenu_surface", isCol(px(img, dpi, sub.x + 12.0f, sub.bottom() - 3.0f), surface), 1);
            P.eq("draw." + t + d + ".open_row_pill", isCol(px(img, dpi, moreRow.x + 6.0f, moreRow.y + 4.0f), hover), 1);
            P.eq("draw." + t + d + ".highlighted_pill", isCol(px(img, dpi, threeRow.x + 6.0f, threeRow.y + 4.0f), hover), 1);
            P.eq("draw." + t + d + ".ground_far_away", isCol(px(img, dpi, 440.0f, 300.0f), th.ground), 1);
            P.eq("draw." + t + d + ".panels_own_drawing", isCol(px(img, dpi, 20.0f, 20.0f), th.ink16), 1);
            P.eq("draw." + t + d + ".inside_bounds", top.x >= MenuOverlay::kEdgePx && sub.right() <= kW - MenuOverlay::kEdgePx
                                                         && top.bottom() <= kH - MenuOverlay::kEdgePx, 1);
            if (dpi > 1.5f)
                host.writePng(("menu-" + t + ".png").c_str(), 2);

            panel.menu.dismiss();
            P.eq("draw." + t + d + ".nothing_after_dismiss", host.draw().prims.size() == bare, 1);
        }
    }
    return P.finish();
}

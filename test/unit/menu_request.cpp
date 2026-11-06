// FUNKGUI_TEST name=fg.menu.request timeout=120 gpu=0
//
// fg.menu.request: the 0.25.0 fields of a menu request (panel/HostServices.h: a section header, a radio item, a
// shortcut, a submenu; the selected item and the minimum width) as a JUCE-free build sees them.
// - fields.*: a request written before them means what it did (the first five members' initialisers are unchanged,
//   and every new field defaults to what was shown before).
// - valid.*: validMenu(), the one rule of every host: at least one item that can be chosen and an id on every item
//   that could be; separators, headers and submenu rows are never chosen and their ids are not read; a submenu with
//   nothing to choose refuses the whole request; a disabled item still needs its id.
// - headless.*: HeadlessHost applies it (a refused request drops its callback unrun), keeps the new fields in
//   log.lastMenu, and chooses into submenus as a user would: by id or label, depth first, never a header or a
//   submenu's own row, and not through a disabled submenu row.
// EditorHost's side is fg.menulook (the JUCE menu the request becomes) and fg.host.services.editor (its refusals).
// Spec rows only.

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Theme.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Panel.h>
#include <funkgui/test/Harness.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace T = funkgui::test;
using funkgui::MenuItem;
using funkgui::MenuRequest;

namespace
{
    class QuietPanel final : public funkgui::Panel
    {
    public:
        funkgui::HostServices* host = nullptr;

        void attach(funkgui::HostServices& h) override { host = &h; }
        int  width() const override { return 160; }
        int  height() const override { return 90; }
        void tick(float) override {}
        void draw(funkgui::Canvas&, const funkgui::Theme&) override {}
        bool wantsFullRate() const override { return false; }
        void accessibility(std::vector<funkgui::A11yItem>&) const override {}
        uint32_t a11yRevision() const override { return 0; }
        void a11yAction(uint32_t, funkgui::A11yAction, double) override {}
        void closeGestures() override {}
    };

    struct Call
    {
        int runs = 0, id = -1;
        std::weak_ptr<int> alive;

        funkgui::MenuCallback menu()
        {
            auto token = std::make_shared<int>(0);
            alive = token;
            runs = 0;
            id = -1;
            return [this, token](int chosen) {
                ++runs;
                id = chosen;
            };
        }

        bool dropped() const { return runs == 0 && alive.expired(); }
    };

    MenuItem header(const char* label)
    {
        MenuItem m;
        m.label = label;
        m.header = true;
        return m;
    }

    MenuItem sub(const char* label, std::vector<MenuItem> items, bool enabled = true)
    {
        MenuItem m;
        m.label = label;
        m.enabled = enabled;
        m.submenu = std::move(items);
        return m;
    }

    // Every kind of row: a header, plain items with shortcuts, radio items, a submenu with a submenu, a disabled
    // submenu, a separator.
    MenuRequest fullMenu()
    {
        MenuRequest m;
        m.items.push_back(header("Factory"));
        MenuItem save{ 1, "Save" };
        save.shortcut = "Cmd+S";
        m.items.push_back(save);
        MenuItem small{ 2, "Small" }, large{ 3, "Large", true, true };
        small.radio = large.radio = true;
        m.items.push_back(small);
        m.items.push_back(large);
        m.items.push_back({ .separator = true });
        m.items.push_back(sub("Recent", { { 10, "Bass" }, sub("Older", { { 11, "Pad" } }) }));
        m.items.push_back(sub("Locked", { { 20, "Hidden" } }, false));
        m.items.push_back({ 30, "Twin" });
        m.items.push_back(sub("Twins", { { 31, "Twin" } }));
        m.selectedId = 3;
        m.minWidth = 140.0f;
        return m;
    }
}

int main(int argc, char** argv)
{
    const funkgui::HeadlessGuiScope gui;
    T::Probe P("fg.menu.request", "", argc, argv);

    // ---- the fields' defaults -----------------------------------------------------------------------------------------
    {
        const MenuItem a{ 1, "Save" };
        P.eq("fields.positional_first_five", a.id == 1 && a.label == "Save" && a.enabled && !a.checked && !a.separator,
             1);
        P.eq("fields.new_ones_default_off", !a.header && !a.radio && a.shortcut.empty() && a.submenu.empty(), 1);
        const MenuItem s{ .separator = true };
        P.eq("fields.designated_separator", s.separator && s.id == 0, 1);
        const MenuRequest r;
        P.eq("fields.request_defaults", r.selectedId == 0 && r.minWidth == 0.0f, 1);
    }

    // ---- validMenu --------------------------------------------------------------------------------------------------
    {
        P.eq("valid.full_menu", funkgui::validMenu(fullMenu()), 1);
        P.eq("valid.empty_refused", funkgui::validMenu(MenuRequest{}), 0);
        MenuRequest headers;
        headers.items = { header("A"), { .separator = true }, header("B") };
        P.eq("valid.headers_and_separators_alone_refused", funkgui::validMenu(headers), 0);
        MenuRequest headerNoId;
        headerNoId.items = { header("A"), { 1, "One" } };
        P.eq("valid.header_id_not_read", funkgui::validMenu(headerNoId), 1);
        MenuRequest subOnly;
        subOnly.items = { sub("More", { { 5, "Five" } }) };
        P.eq("valid.submenu_choice_counts", funkgui::validMenu(subOnly), 1);
        MenuRequest emptySub;
        emptySub.items = { { 1, "One" }, sub("More", { { .separator = true } }) };
        P.eq("valid.submenu_without_choice_refused", funkgui::validMenu(emptySub), 0);
        MenuRequest badDeep;
        badDeep.items = { { 1, "One" }, sub("More", { sub("Deeper", { { 0, "No id" } }) }) };
        P.eq("valid.deep_item_without_id_refused", funkgui::validMenu(badDeep), 0);
        MenuRequest disabledNoId;
        disabledNoId.items = { { 1, "One" }, { 0, "Off", false } };
        P.eq("valid.disabled_item_needs_its_id", funkgui::validMenu(disabledNoId), 0);
        MenuRequest disabledOnly;
        disabledOnly.items = { { 4, "Off", false } };
        P.eq("valid.disabled_item_alone_shown", funkgui::validMenu(disabledOnly), 1);
    }

    // ---- HeadlessHost ---------------------------------------------------------------------------------------------------
    {
        QuietPanel panel;
        funkgui::HeadlessHost host(panel);
        funkgui::HostServices& h = host;

        Call refused;
        MenuRequest headers;
        headers.items = { header("Only a header") };
        const bool headersTaken = h.showMenu(headers, refused.menu());
        P.eq("headless.headers_alone_refused", !headersTaken && refused.dropped(), 1);

        Call c;
        P.eq("headless.full_menu_taken", h.showMenu(fullMenu(), c.menu()), 1);
        const MenuRequest* shown = host.pendingMenu();
        const bool kept = shown != nullptr && shown->items.size() == fullMenu().items.size()
                          && shown->items[0].header && shown->items[1].shortcut == "Cmd+S" && shown->items[3].radio
                          && shown->items[5].submenu.size() == 2 && shown->items[5].submenu[1].submenu.size() == 1
                          && shown->selectedId == 3 && shown->minWidth == 140.0f;
        P.eq("headless.fields_logged", kept, 1);
        P.eq("headless.header_not_chosen_by_label", host.chooseMenuItem("Factory"), 0);
        P.eq("headless.submenu_row_not_chosen", host.chooseMenuItem("Recent"), 0);
        P.eq("headless.through_disabled_row_refused_by_id", host.chooseMenuItem(20), 0);
        P.eq("headless.through_disabled_row_refused_by_label", host.chooseMenuItem("Hidden"), 0);
        P.eq("headless.still_pending", host.pendingMenu() != nullptr && c.runs == 0, 1);
        P.eq("headless.deep_item_by_id", host.chooseMenuItem(11) && c.runs == 1 && c.id == 11, 1);

        P.eq("headless.again_taken", h.showMenu(fullMenu(), c.menu()), 1);
        P.eq("headless.submenu_item_by_label", host.chooseMenuItem("Bass") && c.runs == 1 && c.id == 10, 1);
        P.eq("headless.again_taken_2", h.showMenu(fullMenu(), c.menu()), 1);
        P.eq("headless.label_first_in_menu_order", host.chooseMenuItem("Twin") && c.id == 30, 1);
        P.eq("headless.again_taken_3", h.showMenu(fullMenu(), c.menu()), 1);
        P.eq("headless.radio_by_id", host.chooseMenuItem(2) && c.id == 2, 1);
        P.eq("headless.again_taken_4", h.showMenu(fullMenu(), c.menu()), 1);
        P.eq("headless.missing_id_refused", host.chooseMenuItem(99), 0);
        P.eq("headless.cancel", host.cancelMenu() && c.runs == 1 && c.id == 0, 1);
    }
    return P.finish();
}

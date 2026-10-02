// FUNKGUI_TEST name=fg.host.services timeout=120 gpu=0
//
// fg.host.services: the services of panel/HostServices.h (Web Sprint B, v0.12.0; FCompressor ADR-93) as a JUCE-free
// build sees them: a popup menu, a file chooser and the clipboard as plain calls, and commandKeyIsMeta().
// - A host written before them (the pure virtuals only) still compiles, reports no service and refuses every call:
//   the callback it is handed is destroyed unrun.
// - HeadlessHost reports all three, counts every call in `log` and keeps the last request of each kind there (the
//   items, the anchor and the theme; the mode, title, pattern and suggested name; the copied text), and holds a menu
//   or a chooser pending until the test answers it (chooseMenuItem by id or by label, cancelMenu, returnFiles,
//   cancelFiles), which is when the callback runs. commandKeyIsMeta() is the platform's until it is set.
// - Every rule of the header for the callbacks: never from inside the call that took it (direct and through a Panel's
//   input handler), at most once, not after dismissMenus(), a second request replaces the first of its kind (whose
//   callback is destroyed unrun), a callback may ask again, and the destructor drops what is pending before the Panel
//   hears closeGestures(), so nothing runs after the host has let go of the Panel or has gone.
// - Mode::save's extension rule (a single "*.ext" pattern: appended, or put in place of another, without case).
// - A Panel that calls no service leaves the new log fields untouched and nothing pending.
// EditorHost's side needs a window: the gallery app's "services" section (test/gallery/ServicesGallery.cpp), by hand.
// Spec rows only.

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/core/Theme.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Input.h>
#include <funkgui/panel/Panel.h>
#include <funkgui/test/Harness.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace T = funkgui::test;
using funkgui::FileRequest;
using funkgui::HeadlessHost;
using funkgui::HostServices;
using funkgui::MenuItem;
using funkgui::MenuRequest;

namespace
{
    // A host as every host was before v0.12.0: the pure virtuals and nothing else.
    class PlainHost final : public HostServices
    {
    public:
        void   setUnboundedDrag(bool) override {}
        void   showParamMenu(funkgui::ParamPort&, float, float) override {}
        void   nudgeFullRate() override {}
        double nowSeconds() const override { return 0.0; }
        void   beginBatch() override {}
        void   endBatch() override {}
    };

    constexpr funkgui::Rect kCell{ 20.0f, 30.0f, 64.0f, 16.0f };

    MenuRequest sampleMenu()
    {
        MenuRequest m;
        m.items.push_back({ 1, "Save" });
        m.items.push_back({ 2, "Save As\xE2\x80\xA6", true, true });           // UTF-8 kept as is; ticked
        m.items.push_back({ .separator = true });
        m.items.push_back({ 3, "Delete", false });                             // disabled
        m.anchor = kCell;
        m.theme = funkgui::Theme::paper();
        return m;
    }

    // What a callback did, and whether it still exists: the token is captured by value, so it is gone exactly when
    // the callback has been destroyed. menu() and files() make a fresh callback and forget the last one's run.
    struct Call
    {
        int  runs = 0;
        int  id = -1;
        std::vector<std::string> paths;
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

        funkgui::FilesCallback files()
        {
            auto token = std::make_shared<int>(0);
            alive = token;
            runs = 0;
            paths.clear();
            return [this, token](const std::vector<std::string>& chosen) {
                ++runs;
                paths = chosen;
            };
        }

        bool dropped() const { return runs == 0 && alive.expired(); }      // destroyed without being called
        bool pending() const { return runs == 0 && !alive.expired(); }     // held, not called yet
    };

    // A Panel that asks for a menu on a popup click and for a chooser on Return, as a product view would, and notes
    // what the host's callbacks looked like when the host let go of it.
    class ServicePanel final : public funkgui::Panel
    {
    public:
        HostServices* host = nullptr;
        Call  menu, files;
        int   closes = 0;
        bool  shown = false, chooserOpened = false;
        bool  menuDroppedAtClose = false, filesDroppedAtClose = false;

        void attach(HostServices& h) override { host = &h; }
        int  width() const override { return 160; }
        int  height() const override { return 90; }
        void tick(float) override {}
        void draw(funkgui::Canvas& c, const funkgui::Theme& th) override
        {
            c.rrect(kCell.x, kCell.y, kCell.w, kCell.h, 2.0f, th.ink52);
        }
        bool wantsFullRate() const override { return false; }
        void pointerDown(const funkgui::PointerEvent& e) override
        {
            if (e.popup && kCell.contains({ e.x, e.y }))
                shown = host->showMenu(sampleMenu(), menu.menu());
        }
        bool key(const funkgui::KeyEvent& e) override
        {
            if (e.key != funkgui::Key::enter)
                return false;
            FileRequest r;
            r.mode = FileRequest::Mode::openMany;
            r.title = "Import presets";
            r.pattern = "*.fcmppreset";
            chooserOpened = host->chooseFiles(r, files.files());
            return true;
        }
        void accessibility(std::vector<funkgui::A11yItem>&) const override {}
        uint32_t a11yRevision() const override { return 0; }
        void a11yAction(uint32_t, funkgui::A11yAction, double) override {}
        void closeGestures() override
        {
            ++closes;
            menuDroppedAtClose = menu.dropped();
            filesDroppedAtClose = files.dropped();
        }
    };

    bool sameColour(funkgui::Col a, funkgui::Col b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

    bool sameItem(const MenuItem& a, const MenuItem& b)
    {
        return a.id == b.id && a.label == b.label && a.enabled == b.enabled && a.checked == b.checked
            && a.separator == b.separator;
    }

    bool sameMenu(const MenuRequest& a, const MenuRequest& b)
    {
        if (a.items.size() != b.items.size())
            return false;
        for (size_t i = 0; i < a.items.size(); ++i)
            if (!sameItem(a.items[i], b.items[i]))
                return false;
        return a.anchor.x == b.anchor.x && a.anchor.y == b.anchor.y && a.anchor.w == b.anchor.w
            && a.anchor.h == b.anchor.h && sameColour(a.theme.ground, b.theme.ground)
            && sameColour(a.theme.ink100, b.theme.ink100);
    }

    FileRequest saveRequest(const char* pattern)
    {
        FileRequest r;
        r.mode = FileRequest::Mode::save;
        r.title = "Export preset";
        r.pattern = pattern;
        r.suggestedName = "My Preset.fcmppreset";
        return r;
    }

    // One save through a fresh host: the path the callback receives for `picked`.
    std::string savedAs(const char* pattern, const char* picked)
    {
        ServicePanel panel;
        HeadlessHost host(panel);
        Call p;
        host.chooseFiles(saveRequest(pattern), p.files());
        return host.returnFiles({ picked }) && p.runs == 1 && p.paths.size() == 1 ? p.paths[0] : std::string("?");
    }
}

int main(int argc, char** argv)
{
    const funkgui::HeadlessGuiScope gui;                 // JUCE's GUI side, when there is JUCE: the atlas bakes there
    T::Probe P("fg.host.services", "", argc, argv);
    namespace hs = funkgui::hostservice;
    constexpr unsigned kAll = hs::menus | hs::fileChooser | hs::clipboard;
   #if defined(__APPLE__)
    constexpr bool kPlatformMeta = true;
   #else
    constexpr bool kPlatformMeta = false;
   #endif

    // ---- a pre-v0.12.0 host: the defaults refuse --------------------------------------------------------------------
    {
        PlainHost plain;
        HostServices& h = plain;
        P.eq("defaults.services_none", h.services(), 0);
        Call menu, files;
        P.eq("defaults.show_menu_refused", h.showMenu(sampleMenu(), menu.menu()), 0);
        P.eq("defaults.show_menu_callback_dropped", menu.dropped(), 1);
        h.dismissMenus();
        P.eq("defaults.choose_files_refused", h.chooseFiles(saveRequest("*.fcmppreset"), files.files()), 0);
        P.eq("defaults.choose_files_callback_dropped", files.dropped(), 1);
        P.eq("defaults.copy_text_refused", h.copyText("text"), 0);
        P.eq("defaults.command_key_is_platform", h.commandKeyIsMeta(), kPlatformMeta);
    }

    // ---- HeadlessHost: what it reports, and a Panel that asks for nothing -------------------------------------------
    {
        ServicePanel panel;
        HeadlessHost host(panel);
        HostServices& h = host;
        P.eq("headless.services_all", h.services(), kAll);
        P.eq("headless.service_bits", hs::menus == 1u && hs::fileChooser == 2u && hs::clipboard == 4u, 1);
        P.eq("headless.command_key_default_is_platform", h.commandKeyIsMeta(), kPlatformMeta);
        host.setCommandKeyIsMeta(true);
        P.eq("headless.command_key_set_meta", h.commandKeyIsMeta(), 1);
        host.setCommandKeyIsMeta(false);
        P.eq("headless.command_key_set_ctrl", h.commandKeyIsMeta(), 0);

        host.click(40.0f, 38.0f);                        // a plain click: the Panel asks for nothing
        host.tick(2);
        host.draw();
        const auto& log = host.log;
        P.eq("untouched.log", log.menuRequests == 0 && log.menuDismissals == 0 && log.fileRequests == 0
                                  && log.copies == 0 && log.lastMenu.items.empty() && log.lastFiles.title.empty()
                                  && log.lastCopy.empty() && log.menus == 0 && log.nudges == 0 && log.batches == 0, 1);
        P.eq("untouched.nothing_pending", host.pendingMenu() == nullptr && host.pendingFiles() == nullptr, 1);
        P.eq("untouched.answers_refused", !host.chooseMenuItem(1) && !host.chooseMenuItem("Save") && !host.cancelMenu()
                                              && !host.returnFiles({ "/tmp/a" }) && !host.cancelFiles(), 1);
    }

    // ---- a menu: logged, pending, answered --------------------------------------------------------------------------
    {
        ServicePanel panel;
        HeadlessHost host(panel);
        funkgui::Mods ctrl;
        ctrl.ctrl = true;
        host.click(40.0f, 38.0f, ctrl);                  // a popup click on the cell: the Panel calls showMenu
        P.eq("menu.taken", panel.shown, 1);
        P.eq("menu.not_run_inside_the_call", panel.menu.pending(), 1);   // back from the input handler: still unrun
        P.eq("menu.counted", host.log.menuRequests, 1);
        P.eq("menu.param_menu_count_untouched", host.log.menus, 0);
        P.eq("menu.logged", sameMenu(host.log.lastMenu, sampleMenu()), 1);
        P.eq("menu.pending_is_the_log", host.pendingMenu() == &host.log.lastMenu, 1);
        P.eq("menu.pending_items", host.pendingMenu() != nullptr && host.pendingMenu()->items.size() == 4, 1);
        host.tick(5);                                    // time alone answers nothing
        P.eq("menu.still_pending_after_ticks", panel.menu.pending() && host.pendingMenu() != nullptr, 1);

        // Answers the user could not give leave it pending and run nothing.
        P.eq("menu.unknown_id_refused", host.chooseMenuItem(99), 0);
        P.eq("menu.zero_id_refused", host.chooseMenuItem(0), 0);
        P.eq("menu.disabled_refused", host.chooseMenuItem(3), 0);
        P.eq("menu.disabled_label_refused", host.chooseMenuItem("Delete"), 0);
        P.eq("menu.unknown_label_refused", host.chooseMenuItem("Nope"), 0);
        P.eq("menu.empty_label_is_not_the_separator", host.chooseMenuItem(""), 0);
        P.eq("menu.refusals_ran_nothing", panel.menu.pending() && host.pendingMenu() != nullptr, 1);

        P.eq("menu.chosen", host.chooseMenuItem(2), 1);
        P.eq("menu.callback_ran_once_with_id", panel.menu.runs == 1 && panel.menu.id == 2, 1);
        P.eq("menu.callback_destroyed_after_run", panel.menu.alive.expired(), 1);
        P.eq("menu.no_longer_pending", host.pendingMenu() == nullptr, 1);
        P.eq("menu.request_kept_in_log", sameMenu(host.log.lastMenu, sampleMenu()), 1);
        // At most once: a second answer finds nothing.
        P.eq("menu.second_answer_refused", !host.chooseMenuItem(1) && !host.cancelMenu(), 1);
        P.eq("menu.at_most_once", panel.menu.runs, 1);

        host.click(40.0f, 38.0f, ctrl);
        P.eq("menu.by_label", host.chooseMenuItem("Save") && panel.menu.runs == 1 && panel.menu.id == 1, 1);
        host.click(40.0f, 38.0f, ctrl);
        P.eq("menu.by_utf8_label", host.chooseMenuItem("Save As\xE2\x80\xA6") && panel.menu.id == 2, 1);
        host.click(40.0f, 38.0f, ctrl);
        P.eq("menu.cancelled", host.cancelMenu() && panel.menu.runs == 1 && panel.menu.id == 0, 1);
        P.eq("menu.cancel_at_most_once", !host.cancelMenu() && panel.menu.runs == 1, 1);
        P.eq("menu.requests_counted", host.log.menuRequests, 4);

        host.click(100.0f, 80.0f, ctrl);                 // a popup click elsewhere: no menu, the log does not move
        P.eq("menu.none_outside_the_cell", host.log.menuRequests == 4 && host.pendingMenu() == nullptr, 1);
    }

    // ---- a menu: requests the host refuses --------------------------------------------------------------------------
    {
        ServicePanel panel;
        HeadlessHost host(panel);
        HostServices& h = host;
        Call empty, badId, separators, first, second, third, fourth;
        P.eq("menu_refused.no_items", h.showMenu(MenuRequest{}, empty.menu()), 0);
        P.eq("menu_refused.no_items_dropped", empty.dropped() && host.pendingMenu() == nullptr, 1);
        MenuRequest bad = sampleMenu();
        bad.items[1].id = 0;                             // an item, not a separator, with no id
        P.eq("menu_refused.item_without_id", h.showMenu(bad, badId.menu()), 0);
        P.eq("menu_refused.item_without_id_dropped", badId.dropped() && host.pendingMenu() == nullptr, 1);
        P.eq("menu_refused.counted_and_logged", host.log.menuRequests == 2 && sameMenu(host.log.lastMenu, bad), 1);
        MenuRequest negative = sampleMenu();
        negative.items[0].id = -4;
        P.eq("menu_refused.negative_id", h.showMenu(negative, {}), 0);
        // Separators alone: nothing could be chosen, and the live host has no menu to open for it.
        MenuRequest onlySeparators;
        onlySeparators.items.push_back({ .separator = true });
        onlySeparators.items.push_back({ .separator = true });
        P.eq("menu_refused.separators_alone", h.showMenu(onlySeparators, separators.menu()), 0);
        P.eq("menu_refused.separators_alone_dropped", separators.dropped() && host.pendingMenu() == nullptr, 1);
        P.eq("menu_refused.separators_alone_counted_and_logged",
             host.log.menuRequests == 4 && sameMenu(host.log.lastMenu, onlySeparators), 1);
        P.eq("menu_refused.no_answer_to_a_refused_menu", !host.chooseMenuItem(1) && !host.cancelMenu(), 1);
        // The separator of a menu that is pending cannot be chosen, by its id (0) or by its label (none).
        const bool firstTaken = h.showMenu(sampleMenu(), first.menu());
        P.eq("menu_refused.separator_cannot_be_chosen",
             firstTaken && !host.chooseMenuItem(0) && !host.chooseMenuItem("") && first.pending()
                 && host.pendingMenu() != nullptr, 1);
        // A refused request still replaces the menu that was showing. (Its own callback is the call's argument: it
        // goes when the statement that made the call ends.)
        const bool refused = !h.showMenu(MenuRequest{}, second.menu());
        P.eq("menu_refused.replaces_the_pending", refused && first.dropped() && second.dropped()
                                                      && host.pendingMenu() == nullptr, 1);
        const bool thirdTaken = h.showMenu(sampleMenu(), third.menu());
        const bool refusedToo = !h.showMenu(onlySeparators, fourth.menu());
        P.eq("menu_refused.separators_alone_replace_the_pending",
             thirdTaken && refusedToo && third.dropped() && fourth.dropped() && host.pendingMenu() == nullptr, 1);
        // An empty callback is taken like any other: the answer has nothing to call.
        P.eq("menu.empty_callback", h.showMenu(sampleMenu(), {}) && host.pendingMenu() != nullptr
                                        && host.chooseMenuItem(1) && host.pendingMenu() == nullptr, 1);
    }

    // ---- a menu: replacement, dismissMenus, a callback that asks again ----------------------------------------------
    {
        ServicePanel panel;
        HeadlessHost host(panel);
        HostServices& h = host;
        Call a, b;
        h.showMenu(sampleMenu(), a.menu());
        MenuRequest other;
        other.items.push_back({ 7, "Other" });
        P.eq("replace.second_taken", h.showMenu(other, b.menu()), 1);
        P.eq("replace.first_dropped_unrun", a.dropped(), 1);
        P.eq("replace.second_pending", b.pending() && host.pendingMenu() != nullptr
                                           && host.pendingMenu()->items.size() == 1, 1);
        P.eq("replace.first_item_is_gone", host.chooseMenuItem(1), 0);
        P.eq("replace.second_answered", host.chooseMenuItem(7) && b.runs == 1 && b.id == 7 && a.runs == 0, 1);

        Call c;
        h.showMenu(sampleMenu(), c.menu());
        h.dismissMenus();
        P.eq("dismiss.callback_dropped_unrun", c.dropped(), 1);
        P.eq("dismiss.nothing_pending", host.pendingMenu() == nullptr, 1);
        P.eq("dismiss.no_answer_after", !host.chooseMenuItem(1) && !host.cancelMenu() && c.runs == 0, 1);
        h.dismissMenus();                                // nothing showing: counted, nothing else
        P.eq("dismiss.counted", host.log.menuDismissals, 2);

        // A callback may ask for the next menu: it is pending when the answer returns, and has not run.
        Call follow;
        int outerRuns = 0;
        bool followTaken = false, pendingInside = true;
        h.showMenu(sampleMenu(), [&](int id) {
            ++outerRuns;
            pendingInside = host.pendingMenu() != nullptr;       // its own request is no longer pending
            if (id == 1)
                followTaken = h.showMenu(other, follow.menu());
        });
        P.eq("reentrant.outer_answered", host.chooseMenuItem(1) && outerRuns == 1, 1);
        P.eq("reentrant.own_request_cleared_first", pendingInside, 0);
        P.eq("reentrant.follow_up_pending", followTaken && follow.pending() && host.pendingMenu() != nullptr
                                                && host.pendingMenu()->items.size() == 1, 1);
        P.eq("reentrant.follow_up_answered", host.chooseMenuItem("Other") && follow.runs == 1 && follow.id == 7
                                                 && outerRuns == 1, 1);
        // A callback may dismiss: there is nothing left of its own menu to drop.
        int dismissRuns = 0;
        h.showMenu(sampleMenu(), [&](int) {
            ++dismissRuns;
            h.dismissMenus();
        });
        P.eq("reentrant.dismiss_inside", host.cancelMenu() && dismissRuns == 1 && host.pendingMenu() == nullptr, 1);
    }

    // ---- a file chooser ---------------------------------------------------------------------------------------------
    {
        ServicePanel panel;
        HeadlessHost host(panel);
        HostServices& h = host;
        host.keys("return");                             // the Panel's key handler calls chooseFiles (openMany)
        P.eq("files.taken", panel.chooserOpened, 1);
        P.eq("files.not_run_inside_the_call", panel.files.pending(), 1);
        const FileRequest& r = host.log.lastFiles;
        P.eq("files.logged", host.log.fileRequests == 1 && r.mode == FileRequest::Mode::openMany
                                 && r.title == "Import presets" && r.pattern == "*.fcmppreset"
                                 && r.suggestedName.empty(), 1);
        P.eq("files.pending_is_the_log", host.pendingFiles() == &host.log.lastFiles, 1);
        P.eq("files.no_path_is_not_an_answer", host.returnFiles({}), 0);
        const std::vector<std::string> two = { "/tmp/a.fcmppreset", "/tmp/\xC3\xBC b.fcmppreset" };
        P.eq("files.returned", host.returnFiles(two), 1);
        P.eq("files.callback_ran_once_with_paths", panel.files.runs == 1 && panel.files.paths == two, 1);
        P.eq("files.callback_destroyed_after_run", panel.files.alive.expired(), 1);
        P.eq("files.at_most_once", !host.returnFiles(two) && !host.cancelFiles() && panel.files.runs == 1
                                       && host.pendingFiles() == nullptr, 1);

        host.keys("return");
        P.eq("files.cancelled", host.cancelFiles() && panel.files.runs == 1 && panel.files.paths.empty(), 1);
        P.eq("files.cancel_at_most_once", !host.cancelFiles() && panel.files.runs == 1, 1);

        // Mode::open: one path only.
        Call one;
        FileRequest open;
        open.mode = FileRequest::Mode::open;
        open.title = "Open";
        P.eq("files.open_taken", h.chooseFiles(open, one.files()), 1);
        P.eq("files.open_refuses_several", !host.returnFiles(two) && one.pending(), 1);
        P.eq("files.open_one", host.returnFiles({ "/tmp/a.txt" }) && one.runs == 1
                                   && one.paths == std::vector<std::string>{ "/tmp/a.txt" }, 1);

        // Mode::save: logged with its suggested name; one path.
        Call save;
        h.chooseFiles(saveRequest("*.fcmppreset"), save.files());
        P.eq("files.save_logged", r.mode == FileRequest::Mode::save && r.title == "Export preset"
                                      && r.suggestedName == "My Preset.fcmppreset" && host.log.fileRequests == 4, 1);
        P.eq("files.save_refuses_several", !host.returnFiles(two) && save.pending(), 1);
        P.eq("files.save_one", host.returnFiles({ "/tmp/out.fcmppreset" }) && save.runs == 1
                                   && save.paths == std::vector<std::string>{ "/tmp/out.fcmppreset" }, 1);

        // Replacement: the first chooser's callback is destroyed unrun.
        Call a, b;
        h.chooseFiles(open, a.files());
        P.eq("files.replace_second_taken", h.chooseFiles(saveRequest("*.fcmppreset"), b.files()), 1);
        P.eq("files.replace_first_dropped_unrun", a.dropped() && b.pending(), 1);
        P.eq("files.replace_second_answered", host.cancelFiles() && b.runs == 1 && a.runs == 0, 1);

        // A callback may ask again.
        Call follow;
        int outerRuns = 0;
        h.chooseFiles(open, [&](const std::vector<std::string>&) {
            ++outerRuns;
            h.chooseFiles(open, follow.files());
        });
        P.eq("files.reentrant", host.returnFiles({ "/tmp/a" }) && outerRuns == 1 && follow.pending()
                                    && host.pendingFiles() != nullptr, 1);
        P.eq("files.reentrant_answered", host.cancelFiles() && follow.runs == 1 && outerRuns == 1, 1);
    }

    // ---- Mode::save: the path ends in the pattern's extension -------------------------------------------------------
    {
        const auto same = [&P](const char* key, const std::string& got, const char* want) {
            if (!P.eq(key, got == want, 1))
                std::printf("  %s: got '%s', want '%s'\n", key, got.c_str(), want);
        };
        same("save_ext.appended", savedAs("*.fcmppreset", "/tmp/out"), "/tmp/out.fcmppreset");
        same("save_ext.replaced", savedAs("*.fcmppreset", "/tmp/out.txt"), "/tmp/out.fcmppreset");
        same("save_ext.kept", savedAs("*.fcmppreset", "/tmp/out.fcmppreset"), "/tmp/out.fcmppreset");
        same("save_ext.kept_without_case", savedAs("*.fcmppreset", "/tmp/OUT.FCMPPRESET"), "/tmp/OUT.FCMPPRESET");
        same("save_ext.last_dot_only", savedAs("*.fcmppreset", "/tmp/v1.2 bass"), "/tmp/v1.fcmppreset");
        same("save_ext.dot_in_a_folder", savedAs("*.fcmppreset", "/tmp/a.b/out"), "/tmp/a.b/out.fcmppreset");
        same("save_ext.hidden_file", savedAs("*.fcmppreset", "/tmp/.out"), "/tmp/.fcmppreset");
        same("save_ext.no_pattern", savedAs("", "/tmp/out.txt"), "/tmp/out.txt");
        same("save_ext.any_file", savedAs("*", "/tmp/out.txt"), "/tmp/out.txt");
        same("save_ext.wildcard_extension", savedAs("*.*", "/tmp/out"), "/tmp/out");
        same("save_ext.several_patterns", savedAs("*.a;*.b", "/tmp/out"), "/tmp/out");

        // An open is never rewritten.
        ServicePanel panel;
        HeadlessHost host(panel);
        Call p;
        FileRequest open;
        open.pattern = "*.fcmppreset";
        host.chooseFiles(open, p.files());
        P.eq("save_ext.open_untouched", host.returnFiles({ "/tmp/in.txt" }) && p.paths.size() == 1
                                            && p.paths[0] == "/tmp/in.txt", 1);
    }

    // ---- a menu and a chooser are separate; the clipboard -----------------------------------------------------------
    {
        ServicePanel panel;
        HeadlessHost host(panel);
        HostServices& h = host;
        Call menu, files;
        h.showMenu(sampleMenu(), menu.menu());
        h.chooseFiles(saveRequest("*.fcmppreset"), files.files());
        P.eq("both.pending_together", menu.pending() && files.pending() && host.pendingMenu() != nullptr
                                          && host.pendingFiles() != nullptr, 1);
        h.dismissMenus();
        P.eq("both.dismiss_leaves_the_chooser", menu.dropped() && files.pending() && host.pendingFiles() != nullptr, 1);
        Call again;
        h.showMenu(sampleMenu(), again.menu());
        P.eq("both.chooser_answer_leaves_the_menu", host.cancelFiles() && files.runs == 1 && again.pending(), 1);
        P.eq("both.menu_answer", host.chooseMenuItem(1) && again.runs == 1, 1);

        P.eq("copy.written", h.copyText("FunkGui 0.12\nline two"), 1);
        P.eq("copy.logged", host.log.copies == 1 && host.log.lastCopy == "FunkGui 0.12\nline two", 1);
        const std::string utf8 = "\xC2\xB5S \xE2\x80\x94 \xC3\xBC";
        P.eq("copy.utf8_kept", h.copyText(utf8) && host.log.lastCopy == utf8 && host.log.copies == 2, 1);
        P.eq("copy.empty", h.copyText({}) && host.log.lastCopy.empty() && host.log.copies == 3, 1);
        P.eq("copy.nothing_pending", host.pendingMenu() == nullptr && host.pendingFiles() == nullptr, 1);
    }

    // ---- the host lets go: nothing pending survives it, and nothing runs --------------------------------------------
    {
        ServicePanel panel;
        {
            HeadlessHost host(panel);
            funkgui::Mods ctrl;
            ctrl.ctrl = true;
            host.click(40.0f, 38.0f, ctrl);
            host.keys("return");
            P.eq("let_go.both_pending", panel.menu.pending() && panel.files.pending() && panel.closes == 0, 1);
        }
        P.eq("let_go.gestures_closed_once", panel.closes, 1);
        // Dropped before the Panel heard closeGestures(): nothing it does while its host goes can reach a callback.
        P.eq("let_go.dropped_before_close_gestures", panel.menuDroppedAtClose && panel.filesDroppedAtClose, 1);
        P.eq("let_go.never_run", panel.menu.dropped() && panel.files.dropped(), 1);
    }
    {
        // Nothing pending: the destructor is the one it always was.
        ServicePanel panel;
        {
            HeadlessHost host(panel);
            host.tick(1);
        }
        P.eq("let_go.plain_destructor", panel.closes, 1);
    }
    return P.finish();
}

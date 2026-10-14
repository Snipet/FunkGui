// FUNKGUI_TEST name=fg.announce timeout=120 gpu=0
//
// fg.announce (FSynth G1): HostServices::announce and a11yFocus (panel/HostServices.h), as a JUCE-free build sees
// them. Spec rows only; no JUCE, so it runs in every configuration.
//
// - default.*   a host written before them (the pure virtuals only) compiles; both calls do nothing, with and
//               without a priority: they reach none of the host's own calls, and services() is what it was.
// - headless.*  HeadlessHost logs every announcement that has a text, in call order and whatever its priority, with
//               UTF-8 kept; an empty text is not logged (no host posts one). It logs every a11yFocus call: the count,
//               and the last id asked for, 0 included. Nothing is spoken and nothing takes focus.
// - panel.*     a Panel that patches by keyboard, as a product would: Return picks a port up, Tab visits a target,
//               Return connects, Escape cancels; each step speaks and moves the screen reader's cursor, and the log
//               holds the session in order. The keyboard focus is the Panel's own state and does not move.
// - enum.*      the three priorities, in JUCE's order.
// EditorHost's side (juce::AccessibilityHandler::postAnnouncement, and the bridge child's grabFocus) needs a window
// and an assistive client: it is checked by hand under VoiceOver.

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Theme.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Input.h>
#include <funkgui/panel/Panel.h>
#include <funkgui/test/Harness.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace T = funkgui::test;
using funkgui::HeadlessHost;
using funkgui::HostServices;

namespace
{
    // A host as every host was before the calls existed: the pure virtuals and nothing else, each counted, so a
    // default that reached one of them would show.
    class PlainHost final : public HostServices
    {
    public:
        mutable int calls = 0;

        void   setUnboundedDrag(bool) override { ++calls; }
        void   showParamMenu(funkgui::ParamPort&, float, float) override { ++calls; }
        void   nudgeFullRate() override { ++calls; }
        double nowSeconds() const override
        {
            ++calls;
            return 0.0;
        }
        void   beginBatch() override { ++calls; }
        void   endBatch() override { ++calls; }
    };

    // Three ports in a row. Return on a port picks its cable up; Tab then visits the other ports; Return connects,
    // Escape cancels. Every step is spoken, and the screen reader's cursor follows the port in hand.
    class PatchPanel final : public funkgui::Panel
    {
    public:
        HostServices* host = nullptr;
        int  focus = 0;                                  // the port the keyboard is on: the Panel's own state
        int  carriedFrom = -1;
        int  connections = 0;

        static constexpr uint32_t kFirstId = 100;
        static constexpr const char* kNames[3] = { "Source A Out", "Filter 1 In", "Mix A In" };

        void attach(HostServices& h) override { host = &h; }
        int  width() const override { return 240; }
        int  height() const override { return 60; }
        void tick(float) override {}
        void draw(funkgui::Canvas&, const funkgui::Theme&) override {}
        bool wantsFullRate() const override { return false; }
        bool key(const funkgui::KeyEvent& e) override
        {
            using funkgui::Key;
            if (e.key == Key::enter && carriedFrom < 0)
            {
                carriedFrom = focus;
                host->announce(std::string("Carrying from ") + kNames[focus] + ". 2 targets.");
                return true;
            }
            if (e.key == Key::tab && carriedFrom >= 0)
            {
                do
                    focus = (focus + 1) % 3;
                while (focus == carriedFrom);
                host->a11yFocus(kFirstId + static_cast<uint32_t>(focus));
                host->announce(kNames[focus], HostServices::Announce::low);
                return true;
            }
            if (e.key == Key::enter)
            {
                ++connections;
                host->announce(std::string("Connected ") + kNames[carriedFrom] + " to " + kNames[focus],
                               HostServices::Announce::high);
                carriedFrom = -1;
                return true;
            }
            if (e.key == Key::escape && carriedFrom >= 0)
            {
                focus = carriedFrom;
                carriedFrom = -1;
                host->a11yFocus(kFirstId + static_cast<uint32_t>(focus));
                host->announce("Cancelled", HostServices::Announce::medium);
                return true;
            }
            return false;
        }
        void accessibility(std::vector<funkgui::A11yItem>& out) const override
        {
            for (int i = 0; i < 3; ++i)
            {
                funkgui::A11yItem it;
                it.id = kFirstId + static_cast<uint32_t>(i);
                it.role = funkgui::A11yRole::button;
                it.title = kNames[i];
                it.bounds = { 8.0f + 76.0f * static_cast<float>(i), 18.0f, 24.0f, 24.0f };
                out.push_back(it);
            }
        }
        uint32_t a11yRevision() const override { return 0; }
        void a11yAction(uint32_t, funkgui::A11yAction, double) override {}
        void closeGestures() override {}
    };
}

int main(int argc, char** argv)
{
    const funkgui::HeadlessGuiScope gui;                 // JUCE's GUI side, when there is JUCE: the atlas bakes there
    T::Probe P("fg.announce", "", argc, argv);

    // ---- the priorities ---------------------------------------------------------------------------------------------
    {
        using A = HostServices::Announce;
        P.eq("enum.order", static_cast<int>(A::low) == 0 && static_cast<int>(A::medium) == 1
                               && static_cast<int>(A::high) == 2, 1);
    }

    // ---- the defaults do nothing ------------------------------------------------------------------------------------
    {
        PlainHost plain;
        HostServices& h = plain;
        h.announce("Connected Source A Out to Filter 1 In");
        h.announce("Refused", HostServices::Announce::high);
        h.announce("");
        h.a11yFocus(100);
        h.a11yFocus(0);
        P.eq("default.reaches_no_host_call", plain.calls, 0);
        P.eq("default.services_none", plain.services(), 0);
        P.eq("default.clipboard_empty", h.clipboardText().empty(), 1);
    }

    // ---- HeadlessHost logs them in order ----------------------------------------------------------------------------
    {
        PatchPanel panel;
        HeadlessHost host(panel);
        HostServices& h = host;                          // as a Panel reaches it
        namespace hs = funkgui::hostservice;
        P.eq("headless.initial", host.log.announcements.empty() && host.log.a11yFocusId == 0
                                     && host.log.a11yFocusCalls == 0, 1);
        P.eq("headless.services_unchanged", host.services(), hs::menus | hs::fileChooser | hs::clipboard);

        h.announce("first");
        h.announce("second", HostServices::Announce::low);
        h.announce("third \xE2\x80\x94 na\xC3\xAFve", HostServices::Announce::high);
        P.eq("headless.announcements_in_order",
             host.log.announcements == std::vector<std::string>{ "first", "second", "third \xE2\x80\x94 na\xC3\xAFve" },
             1);
        h.announce("");
        h.announce(std::string_view{});
        P.eq("headless.empty_not_logged", static_cast<int64_t>(host.log.announcements.size()), 3);
        h.announce("first");                             // the same text again is another announcement
        P.eq("headless.repeat_logged", host.log.announcements.size() == 4 && host.log.announcements.back() == "first",
             1);

        h.a11yFocus(100);
        P.eq("headless.focus_logged", host.log.a11yFocusId == 100 && host.log.a11yFocusCalls == 1, 1);
        h.a11yFocus(102);
        h.a11yFocus(102);
        P.eq("headless.focus_every_call", host.log.a11yFocusId == 102 && host.log.a11yFocusCalls == 3, 1);
        h.a11yFocus(0);
        P.eq("headless.focus_zero_logged", host.log.a11yFocusId == 0 && host.log.a11yFocusCalls == 4, 1);
        h.a11yFocus(0xffffffffu);                        // an id the Panel does not list: logged all the same
        P.eq("headless.focus_unlisted_logged", host.log.a11yFocusId == 0xffffffffu && host.log.a11yFocusCalls == 5, 1);

        // Nothing else moved: no menu, no chooser, no copy, no clock.
        P.eq("headless.other_log_untouched", host.log.menus == 0 && host.log.menuRequests == 0
                                                 && host.log.fileRequests == 0 && host.log.copies == 0
                                                 && host.log.nudges == 0 && host.log.drops == 0, 1);
        P.near("headless.clock_still", host.nowSeconds(), 0.0, 0.0);
        // The items a Panel lists are what they were: speech and focus are not part of the model.
        P.eq("headless.items_unchanged", static_cast<int64_t>(host.accessibility().size()), 3);
    }

    // ---- a Panel that patches by keyboard ---------------------------------------------------------------------------
    {
        PatchPanel panel;
        HeadlessHost host(panel);
        host.keys("return");                             // pick up at Source A Out
        P.eq("panel.pick_up",
             host.log.announcements == std::vector<std::string>{ "Carrying from Source A Out. 2 targets." }
                 && host.log.a11yFocusCalls == 0, 1);
        host.keys("tab,tab");                            // Filter 1 In, then Mix A In
        P.eq("panel.targets_spoken", host.log.announcements.size() == 3 && host.log.announcements[1] == "Filter 1 In"
                                         && host.log.announcements[2] == "Mix A In", 1);
        P.eq("panel.cursor_follows", host.log.a11yFocusId == 102 && host.log.a11yFocusCalls == 2, 1);
        host.keys("tab");                                // wraps past the source: only valid targets
        P.eq("panel.wraps_to_valid_target", host.log.a11yFocusId == 101 && host.log.announcements.back() == "Filter 1 In",
             1);
        host.keys("return");                             // drop
        P.eq("panel.connected", panel.connections == 1
                                    && host.log.announcements.back() == "Connected Source A Out to Filter 1 In", 1);
        host.keys("return,tab,escape");                  // pick up at Filter 1 In, visit Mix A In, cancel
        P.eq("panel.cancelled", host.log.announcements.back() == "Cancelled" && panel.carriedFrom == -1
                                    && panel.focus == 1 && host.log.a11yFocusId == 101, 1);
        const std::vector<std::string> session{ "Carrying from Source A Out. 2 targets.", "Filter 1 In", "Mix A In",
                                                "Filter 1 In", "Connected Source A Out to Filter 1 In",
                                                "Carrying from Filter 1 In. 2 targets.", "Mix A In", "Cancelled" };
        P.eq("panel.session_in_order", host.log.announcements == session, 1);
        P.eq("panel.focus_calls", host.log.a11yFocusCalls, 5);
    }

    return P.finish();
}

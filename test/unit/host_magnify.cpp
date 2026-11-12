// FUNKGUI_TEST name=fg.host.magnify timeout=120 gpu=0
//
// fg.host.magnify (F1; panel/Input.h MagnifyEvent, Panel::magnify, panel/HeadlessHost.h wheel(WheelEvent) and
// magnify): spec rows only; no JUCE, so it runs in every configuration.
//
// - magnify.*  HeadlessHost::magnify delivers the event as given (position, scale, Mods) after a pointerMove there
//              with the Mods, popup false even with Ctrl, Button::left; it returns the Panel's answer, and Panel's
//              default magnify answers false. MagnifyEvent's defaults are scale 1 at (0, 0).
// - wheel.*    HeadlessHost::wheel(WheelEvent) delivers the event whole (dx, dy, smooth, reversed, inertial, Mods with
//              Ctrl) after a pointerMove with popup false; it returns the Panel's answer. The old wheel(x, y, dy, ...)
//              still sends dx 0, not reversed, not inertial, and its pointerMove's popup is its ctrl.
// - state.*    neither ends an open press, and both move pointer().

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
#include <vector>

namespace T = funkgui::test;
using funkgui::Button;
using funkgui::HeadlessHost;
using funkgui::MagnifyEvent;
using funkgui::Mods;
using funkgui::PointerEvent;
using funkgui::WheelEvent;

namespace
{
    // A Panel that keeps the last of each event and answers `answer`; magnify only when `takesMagnify`, else
    // Panel's default.
    class RecPanel : public funkgui::Panel
    {
    public:
        std::vector<PointerEvent> moves;
        std::vector<WheelEvent>   wheels;
        int  ups = 0;
        bool answer = true;

        void attach(funkgui::HostServices&) override {}
        int  width() const override { return 200; }
        int  height() const override { return 120; }
        void tick(float) override {}
        void draw(funkgui::Canvas&, const funkgui::Theme&) override {}
        bool wantsFullRate() const override { return false; }
        void pointerMove(const PointerEvent& e) override { moves.push_back(e); }
        void pointerUp(const PointerEvent&) override { ++ups; }
        bool wheel(const WheelEvent& w) override
        {
            wheels.push_back(w);
            return answer;
        }
        void accessibility(std::vector<funkgui::A11yItem>&) const override {}
        uint32_t a11yRevision() const override { return 0; }
        void a11yAction(uint32_t, funkgui::A11yAction, double) override {}
        void closeGestures() override {}
    };

    class ZoomPanel final : public RecPanel
    {
    public:
        std::vector<MagnifyEvent> pinches;
        bool magnify(const MagnifyEvent& g) override
        {
            pinches.push_back(g);
            return answer;
        }
    };

    bool sameMods(Mods a, Mods b) { return a.shift == b.shift && a.cmd == b.cmd && a.alt == b.alt && a.ctrl == b.ctrl; }
}

int main(int argc, char** argv)
{
    const funkgui::HeadlessGuiScope gui;
    T::Probe P("fg.host.magnify", "", argc, argv);

    const MagnifyEvent def{};
    P.eq("magnify.defaults", def.x == 0.0f && def.y == 0.0f && def.scale == 1.0f && sameMods(def.mods, Mods{}), 1);

    Mods ctrlShift;
    ctrlShift.ctrl = ctrlShift.shift = true;

    // ---- magnify ----------------------------------------------------------------------------------------------------
    {
        ZoomPanel panel;
        HeadlessHost host(panel);
        MagnifyEvent g;
        g.x = 41.5f;
        g.y = 17.25f;
        g.scale = 1.0625f;
        g.mods = ctrlShift;
        P.eq("magnify.answer_true", host.magnify(g), 1);
        P.eq("magnify.delivered_once", panel.pinches.size(), 1);
        const MagnifyEvent& got = panel.pinches.back();
        P.eq("magnify.as_given", got.x == g.x && got.y == g.y && got.scale == g.scale && sameMods(got.mods, g.mods), 1);
        P.eq("magnify.move_first", panel.moves.size(), 1);
        const PointerEvent& mv = panel.moves.back();
        P.eq("magnify.move_at", mv.x == g.x && mv.y == g.y && sameMods(mv.mods, g.mods), 1);
        P.eq("magnify.move_no_popup", mv.popup, 0);
        P.eq("magnify.move_left", mv.button == Button::left, 1);
        P.eq("magnify.pointer", host.pointer().x == g.x && host.pointer().y == g.y, 1);
        panel.answer = false;
        g.scale = 0.875f;
        P.eq("magnify.answer_false", host.magnify(g), 0);
        P.eq("magnify.closing_scale", panel.pinches.back().scale == 0.875f, 1);
    }
    {
        RecPanel panel;                                  // magnify not overridden: Panel's default
        HeadlessHost host(panel);
        MagnifyEvent g;
        g.scale = 2.0f;
        P.eq("magnify.default_false", host.magnify(g), 0);
    }

    // ---- wheel(WheelEvent) ------------------------------------------------------------------------------------------
    {
        RecPanel panel;
        HeadlessHost host(panel);
        WheelEvent w;
        w.x = 12.0f;
        w.y = 99.5f;
        w.dx = -0.125f;
        w.dy = 0.375f;
        w.smooth = true;
        w.reversed = true;
        w.inertial = true;
        w.mods = ctrlShift;
        P.eq("wheel.answer_true", host.wheel(w), 1);
        const WheelEvent& got = panel.wheels.back();
        P.eq("wheel.whole", got.x == w.x && got.y == w.y && got.dx == w.dx && got.dy == w.dy && got.smooth
                                && got.reversed && got.inertial && sameMods(got.mods, w.mods), 1);
        P.eq("wheel.move_no_popup", panel.moves.size() == 1 && !panel.moves.back().popup
                                        && sameMods(panel.moves.back().mods, ctrlShift), 1);
        panel.answer = false;
        P.eq("wheel.answer_false", host.wheel(w), 0);

        // The old call: as before.
        Mods ctrl;
        ctrl.ctrl = true;
        host.wheel(3.0f, 4.0f, -1.0f, false, ctrl);
        const WheelEvent& old = panel.wheels.back();
        P.eq("wheel.old_fields", old.dx == 0.0f && old.dy == -1.0f && !old.smooth && !old.reversed && !old.inertial
                                     && sameMods(old.mods, ctrl), 1);
        P.eq("wheel.old_move_popup_is_ctrl", panel.moves.back().popup, 1);
    }

    // ---- state: an open press stays open ----------------------------------------------------------------------------
    {
        ZoomPanel panel;
        HeadlessHost host(panel);
        host.press(10.0f, 10.0f, Button::middle);
        MagnifyEvent g;
        g.x = 50.0f;
        g.y = 60.0f;
        g.scale = 1.5f;
        host.magnify(g);
        WheelEvent w;
        w.x = 70.0f;
        w.y = 80.0f;
        w.dy = 1.0f;
        host.wheel(w);
        P.eq("state.press_open", host.pressed() && panel.ups == 0, 1);
        P.eq("state.pointer", host.pointer().x == 70.0f && host.pointer().y == 80.0f, 1);
        host.release();
        P.eq("state.released", !host.pressed() && panel.ups == 1, 1);
    }
    return P.finish();
}

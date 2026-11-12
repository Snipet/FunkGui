// FUNKGUI_TEST name=fg.host.buttons timeout=120 gpu=0
//
// fg.host.buttons (F1; panel/Input.h Button, panel/HeadlessHost.h "F1"): PointerEvent::button through HeadlessHost.
// Spec rows only; no JUCE, so it runs in every configuration.
//
// - default.*  PointerEvent's button is Button::left, and the calls without a Button (click, doubleClick, drag,
//              press/moveTo/release, move, wheel) send Button::left on every event, as before F1.
// - same.*     each Button overload with Button::left sends the old call's events bit for bit (kinds, positions,
//              Mods, clicks, popup).
// - middle.*   click, doubleClick, drag and press/moveTo/release with Button::middle: every event of the gesture
//              carries it (the pointerMove first included), popup stays false, and the positions are the old ones.
// - right.*    with Button::right: the button on every event, and popup on each (JUCE's isPopupMenu), with no ctrl.
// - after.*    a press's button ends with its release: the next plain move and click say left again.

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Theme.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Input.h>
#include <funkgui/panel/Panel.h>
#include <funkgui/test/Harness.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace T = funkgui::test;
using funkgui::Button;
using funkgui::HeadlessHost;
using funkgui::Mods;
using funkgui::PointerEvent;
using funkgui::WheelEvent;

namespace
{
    enum class Kind : uint8_t { move, down, drag, up, doubleClick };

    struct Event
    {
        Kind         kind = Kind::move;
        PointerEvent e{};
    };

    bool sameBits(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }

    bool sameEvent(const Event& a, const Event& b)
    {
        return a.kind == b.kind && sameBits(a.e.x, b.e.x) && sameBits(a.e.y, b.e.y) && a.e.clicks == b.e.clicks
            && a.e.popup == b.e.popup && a.e.button == b.e.button && a.e.mods.shift == b.e.mods.shift
            && a.e.mods.cmd == b.e.mods.cmd && a.e.mods.alt == b.e.mods.alt && a.e.mods.ctrl == b.e.mods.ctrl;
    }

    bool sameEvents(const std::vector<Event>& a, const std::vector<Event>& b)
    {
        if (a.size() != b.size() || a.empty())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (!sameEvent(a[i], b[i]))
                return false;
        return true;
    }

    // Every event says `b`; and, with popupToo, every event's popup is `popup`.
    bool allButton(const std::vector<Event>& v, Button b)
    {
        if (v.empty())
            return false;
        for (const Event& ev : v)
            if (ev.e.button != b)
                return false;
        return true;
    }

    bool allPopup(const std::vector<Event>& v, bool popup)
    {
        if (v.empty())
            return false;
        for (const Event& ev : v)
            if (ev.e.popup != popup)
                return false;
        return true;
    }

    // The same events with every button rewritten to left: what a Panel that never reads `button` sees.
    std::vector<Event> asLeft(std::vector<Event> v)
    {
        for (Event& ev : v)
            ev.e.button = Button::left;
        return v;
    }

    class RecPanel final : public funkgui::Panel
    {
    public:
        std::vector<Event> events;

        void attach(funkgui::HostServices&) override {}
        int  width() const override { return 200; }
        int  height() const override { return 120; }
        void tick(float) override {}
        void draw(funkgui::Canvas&, const funkgui::Theme&) override {}
        bool wantsFullRate() const override { return false; }
        void pointerMove(const PointerEvent& e) override { events.push_back({ Kind::move, e }); }
        void pointerDown(const PointerEvent& e) override { events.push_back({ Kind::down, e }); }
        void pointerDrag(const PointerEvent& e) override { events.push_back({ Kind::drag, e }); }
        void pointerUp(const PointerEvent& e) override { events.push_back({ Kind::up, e }); }
        void doubleClick(const PointerEvent& e) override { events.push_back({ Kind::doubleClick, e }); }
        bool wheel(const WheelEvent&) override { return true; }
        void accessibility(std::vector<funkgui::A11yItem>&) const override {}
        uint32_t a11yRevision() const override { return 0; }
        void a11yAction(uint32_t, funkgui::A11yAction, double) override {}
        void closeGestures() override {}
    };

    enum class Gesture : uint8_t { click, doubleClick, drag, split };

    // One gesture through a fresh host: the old call (no Button) when `withButton` is false.
    std::vector<Event> run(Gesture g, bool withButton, Button b, Mods m = {})
    {
        RecPanel panel;
        HeadlessHost host(panel);
        const float x0 = 10.1f, y0 = 20.3f, x1 = 133.7f, y1 = 71.9f;
        switch (g)
        {
            case Gesture::click:
                withButton ? host.click(x0, y0, b, m) : host.click(x0, y0, m);
                break;
            case Gesture::doubleClick:
                withButton ? host.doubleClick(x0, y0, b, m) : host.doubleClick(x0, y0, m);
                break;
            case Gesture::drag:
                withButton ? host.drag(x0, y0, x1, y1, b, 5, m) : host.drag(x0, y0, x1, y1, 5, m);
                break;
            case Gesture::split:
                withButton ? host.press(x0, y0, b, m) : host.press(x0, y0, m);
                host.moveTo(x1, y1, 3);
                host.release();
                break;
        }
        return panel.events;
    }
}

int main(int argc, char** argv)
{
    const funkgui::HeadlessGuiScope gui;
    T::Probe P("fg.host.buttons", "", argc, argv);

    P.eq("default.struct", PointerEvent{}.button == Button::left, 1);

    constexpr Gesture kAll[] = { Gesture::click, Gesture::doubleClick, Gesture::drag, Gesture::split };
    constexpr const char* kName[] = { "click", "double", "drag", "split" };
    Mods shift;
    shift.shift = true;
    Mods ctrl;
    ctrl.ctrl = true;

    for (int i = 0; i < 4; ++i)
    {
        const Gesture g = kAll[i];
        const std::string n = kName[i];
        const std::vector<Event> old = run(g, false, Button::left, shift);
        P.eq("default." + n, allButton(old, Button::left), 1);
        P.eq("same." + n, sameEvents(old, run(g, true, Button::left, shift)), 1);
        P.eq("same." + n + "_ctrl", sameEvents(run(g, false, Button::left, ctrl), run(g, true, Button::left, ctrl)), 1);

        const std::vector<Event> mid = run(g, true, Button::middle, shift);
        P.eq("middle." + n + ".button", allButton(mid, Button::middle), 1);
        P.eq("middle." + n + ".no_popup", allPopup(mid, false), 1);
        P.eq("middle." + n + ".as_before", sameEvents(asLeft(mid), old), 1);

        const std::vector<Event> right = run(g, true, Button::right);
        P.eq("right." + n + ".button", allButton(right, Button::right), 1);
        P.eq("right." + n + ".popup", allPopup(right, true), 1);
    }

    // move() and the old wheel() say left.
    {
        RecPanel panel;
        HeadlessHost host(panel);
        host.move(5.0f, 6.0f);
        host.wheel(7.0f, 8.0f, 0.25f, true);
        P.eq("default.move_wheel", panel.events.size() == 2 && allButton(panel.events, Button::left), 1);
    }

    // A middle press ends with its release: the next plain calls are left again.
    {
        RecPanel panel;
        HeadlessHost host(panel);
        host.press(30.0f, 30.0f, Button::middle);
        host.moveTo(60.0f, 40.0f, 2);
        host.release();
        const size_t n = panel.events.size();
        P.eq("after.press_events", n == 5 && allButton(panel.events, Button::middle), 1);
        host.moveTo(70.0f, 45.0f, 1);
        host.click(80.0f, 50.0f);
        const std::vector<Event> rest(panel.events.begin() + static_cast<std::ptrdiff_t>(n), panel.events.end());
        P.eq("after.left_again", rest.size() == 4 && allButton(rest, Button::left), 1);
        // A second press while pressed releases the first with the first's button.
        host.press(10.0f, 10.0f, Button::right);
        host.press(20.0f, 20.0f, Button::middle);
        const Event& up = panel.events[panel.events.size() - 3];
        P.eq("after.second_press_releases_first", up.kind == Kind::up && up.e.button == Button::right, 1);
        host.release();
        P.eq("after.second_release", panel.events.back().kind == Kind::up
                                         && panel.events.back().e.button == Button::middle, 1);
    }
    return P.finish();
}

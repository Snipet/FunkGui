// FUNKGUI_TEST name=fg.headless.press timeout=120 gpu=0
//
// fg.headless.press (FSynth G1): HeadlessHost's drag in steps, file drops and cursor (panel/HeadlessHost.h, "FSynth G1
// additions"). Spec rows only; no JUCE, so it runs in every configuration.
//
// - split.*    press, moveTo and release send the events of drag(), bit for bit: the same kinds in the same order
//              with the same positions, Mods, click counts and popup flags, for one step, several, none asked for,
//              with Mods, and for a ctrl (popup) press.
// - mid.*      a draw() between two moveTo calls records the Panel as it stands while the hand is down, which no
//              atomic drag() can show; after release() the frame is the idle one again.
// - state.*    pressed() and pointer() through every call; a second press releases first; release() without a press
//              sends nothing; moveTo without a press sends pointerMove events with no Mods; click(), doubleClick() and
//              drag() release an open press first (its pointerUp before their pointerMove) and end with their own;
//              no input ticks the clock.
// - drop.*     dropFiles: filesInterest, then filesDragEnter, filesDragMove and filesDropped at the drop position
//              with the paths, in that order, when accepted; nothing after a refusal; log.drops counts both.
// - cursor.*   cursor() is Panel::cursor() after the last event, for each of the nine values.

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/Prim.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/core/Theme.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Input.h>
#include <funkgui/panel/Panel.h>
#include <funkgui/test/Harness.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace T = funkgui::test;
using funkgui::Cursor;
using funkgui::HeadlessHost;
using funkgui::Mods;
using funkgui::Point;
using funkgui::PointerEvent;

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
            && a.e.popup == b.e.popup && a.e.mods.shift == b.e.mods.shift && a.e.mods.cmd == b.e.mods.cmd
            && a.e.mods.alt == b.e.mods.alt && a.e.mods.ctrl == b.e.mods.ctrl;
    }

    bool sameEvents(const std::vector<Event>& a, const std::vector<Event>& b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (!sameEvent(a[i], b[i]))
                return false;
        return true;
    }

    // A Panel that carries a cable: it records every pointer event and file callback, draws a segment from where the
    // press began to where the hand is while it is down, and answers the cursor of its state.
    class CablePanel final : public funkgui::Panel
    {
    public:
        std::vector<Event> events;
        std::vector<std::string> files;                  // the file callbacks, one line each
        bool   carrying = false;
        Point  from{}, hand{};
        bool   acceptFiles = true;
        Cursor shown = Cursor::normal;
        int    ticks = 0;

        void attach(funkgui::HostServices&) override {}
        int  width() const override { return 200; }
        int  height() const override { return 120; }
        void tick(float) override { ++ticks; }
        void draw(funkgui::Canvas& c, const funkgui::Theme& th) override
        {
            c.rrect(8.0f, 8.0f, 24.0f, 24.0f, 2.0f, th.ink52);
            if (carrying)
                c.segment(from.x, from.y, hand.x, hand.y, 2.5f, th.accent);
        }
        bool wantsFullRate() const override { return false; }
        void pointerMove(const PointerEvent& e) override
        {
            events.push_back({ Kind::move, e });
            shown = carrying ? Cursor::grabbing : Cursor::grab;
        }
        void pointerDown(const PointerEvent& e) override
        {
            events.push_back({ Kind::down, e });
            carrying = true;
            from = hand = { e.x, e.y };
            shown = Cursor::grabbing;
        }
        void pointerDrag(const PointerEvent& e) override
        {
            events.push_back({ Kind::drag, e });
            hand = { e.x, e.y };
            shown = e.x > 150.0f ? Cursor::notAllowed : Cursor::grabbing;
        }
        void pointerUp(const PointerEvent& e) override
        {
            events.push_back({ Kind::up, e });
            carrying = false;
            shown = Cursor::grab;
        }
        void doubleClick(const PointerEvent& e) override { events.push_back({ Kind::doubleClick, e }); }
        Cursor cursor() const override { return shown; }
        void accessibility(std::vector<funkgui::A11yItem>&) const override {}
        uint32_t a11yRevision() const override { return 0; }
        void a11yAction(uint32_t, funkgui::A11yAction, double) override {}
        void closeGestures() override {}

        bool filesInterest(const std::vector<std::string>& paths) const override
        {
            const_cast<CablePanel*>(this)->files.push_back("interest " + joined(paths));
            return acceptFiles;
        }
        void filesDragEnter(const std::vector<std::string>& paths, float x, float y) override
        {
            files.push_back("enter " + joined(paths) + at(x, y));
        }
        void filesDragMove(float x, float y) override { files.push_back("move" + at(x, y)); }
        void filesDragExit() override { files.push_back("exit"); }
        void filesDropped(const std::vector<std::string>& paths) override { files.push_back("drop " + joined(paths)); }

    private:
        static std::string joined(const std::vector<std::string>& paths)
        {
            std::string s;
            for (const std::string& p : paths)
                s += (s.empty() ? "" : "|") + p;
            return s;
        }
        static std::string at(float x, float y)
        {
            char buf[48];
            std::snprintf(buf, sizeof buf, " @%g,%g", static_cast<double>(x), static_cast<double>(y));
            return buf;
        }
    };

    // The events of an atomic drag, and of the same drag made in three calls.
    std::vector<Event> dragged(float x0, float y0, float x1, float y1, int steps, Mods m)
    {
        CablePanel panel;
        HeadlessHost host(panel);
        host.drag(x0, y0, x1, y1, steps, m);
        return panel.events;
    }

    std::vector<Event> split(float x0, float y0, float x1, float y1, int steps, Mods m)
    {
        CablePanel panel;
        HeadlessHost host(panel);
        host.press(x0, y0, m);
        host.moveTo(x1, y1, steps);
        host.release();
        return panel.events;
    }

    int segments(const funkgui::PrimList& l)
    {
        int n = 0;
        for (const funkgui::Prim& p : l.prims)
            n += static_cast<int>(p.d2[2] + 0.5f) == static_cast<int>(funkgui::PrimKind::segment) ? 1 : 0;
        return n;
    }
}

int main(int argc, char** argv)
{
    const funkgui::HeadlessGuiScope gui;                 // JUCE's GUI side, when there is JUCE: the atlas bakes there
    T::Probe P("fg.headless.press", "", argc, argv);

    // ---- split: press, moveTo, release are drag()'s events ----------------------------------------------------------
    {
        Mods shift;
        shift.shift = true;
        Mods ctrl;
        ctrl.ctrl = true;
        Mods all;
        all.shift = all.cmd = all.alt = true;
        // Positions that are not exact in float, so an event computed another way would differ in its last bits.
        const float x0 = 20.3f, y0 = 16.7f, x1 = 133.1f, y1 = 71.9f;
        P.eq("split.one_step", sameEvents(split(x0, y0, x1, y1, 1, {}), dragged(x0, y0, x1, y1, 1, {})), 1);
        P.eq("split.eight_steps", sameEvents(split(x0, y0, x1, y1, 8, {}), dragged(x0, y0, x1, y1, 8, {})), 1);
        P.eq("split.seven_steps", sameEvents(split(x0, y0, x1, y1, 7, {}), dragged(x0, y0, x1, y1, 7, {})), 1);
        P.eq("split.no_steps_is_one", sameEvents(split(x0, y0, x1, y1, 0, {}), dragged(x0, y0, x1, y1, 0, {})), 1);
        P.eq("split.shift", sameEvents(split(x0, y0, x1, y1, 3, shift), dragged(x0, y0, x1, y1, 3, shift)), 1);
        P.eq("split.three_mods", sameEvents(split(x0, y0, x1, y1, 3, all), dragged(x0, y0, x1, y1, 3, all)), 1);
        P.eq("split.ctrl_is_popup", sameEvents(split(x0, y0, x1, y1, 2, ctrl), dragged(x0, y0, x1, y1, 2, ctrl)), 1);

        const std::vector<Event> ev = split(x0, y0, x1, y1, 3, ctrl);
        P.eq("split.count", static_cast<int64_t>(ev.size()), 6);           // move, down, three drags, up
        const bool order = ev.size() == 6 && ev[0].kind == Kind::move && ev[1].kind == Kind::down
                        && ev[2].kind == Kind::drag && ev[3].kind == Kind::drag && ev[4].kind == Kind::drag
                        && ev[5].kind == Kind::up;
        P.eq("split.order", order, 1);
        if (order)
        {
            P.eq("split.press_at_start", sameBits(ev[1].e.x, x0) && sameBits(ev[1].e.y, y0), 1);
            P.eq("split.last_drag_at_end", sameBits(ev[4].e.x, x1) && sameBits(ev[4].e.y, y1), 1);
            P.eq("split.up_at_end", sameBits(ev[5].e.x, x1) && sameBits(ev[5].e.y, y1), 1);
            P.eq("split.popup_on_every_event", ev[0].e.popup && ev[1].e.popup && ev[3].e.popup && ev[5].e.popup, 1);
            P.eq("split.single_clicks", ev[1].e.clicks == 1 && ev[3].e.clicks == 1 && ev[5].e.clicks == 1, 1);
        }
    }

    // ---- mid: a frame in the middle of a drag ------------------------------------------------------------------------
    {
        CablePanel panel;
        HeadlessHost host(panel);
        P.eq("mid.idle.segments", segments(host.draw()), 0);
        host.press(20.0f, 20.0f);
        P.eq("mid.pressed.segments", segments(host.draw()), 1);
        host.moveTo(60.0f, 40.0f, 2);
        const funkgui::PrimList half = host.draw();
        host.moveTo(120.0f, 90.0f, 3);
        const funkgui::PrimList later = host.draw();
        P.eq("mid.half.segments", segments(half), 1);
        P.eq("mid.later.segments", segments(later), 1);
        // The cable runs from the press to the hand: a segment's quad spans both ends (its radius and apron beyond).
        const auto spans = [](const funkgui::PrimList& l, float x, float y) {
            for (const funkgui::Prim& p : l.prims)
                if (static_cast<int>(p.d2[2] + 0.5f) == static_cast<int>(funkgui::PrimKind::segment))
                {
                    const float cx = (p.x0 + p.x1) * 0.5f, cy = (p.y0 + p.y1) * 0.5f;
                    const auto near = [](float a, float b) { return std::fabs(a - b) < 1.0e-3f; };
                    return near(cx + p.d1[0], 20.0f) && near(cy + p.d1[1], 20.0f) && near(cx + p.d1[2], x)
                        && near(cy + p.d1[3], y);
                }
            return false;
        };
        P.eq("mid.half.cable_to_the_hand", spans(half, 60.0f, 40.0f), 1);
        P.eq("mid.later.cable_to_the_hand", spans(later, 120.0f, 90.0f), 1);
        P.eq("mid.cursor_while_carried", host.cursor() == Cursor::grabbing, 1);
        host.release();
        P.eq("mid.released.segments", segments(host.draw()), 0);
        P.eq("mid.no_tick", panel.ticks, 0);                           // no input ticks the clock
        P.near("mid.clock_still", host.nowSeconds(), 0.0, 0.0);
    }

    // ---- state: pressed(), pointer(), and what ends a press ---------------------------------------------------------
    {
        CablePanel panel;
        HeadlessHost host(panel);
        P.eq("state.initial", !host.pressed() && host.pointer().x == 0.0f && host.pointer().y == 0.0f, 1);
        host.release();
        P.eq("state.release_without_press_sends_nothing", static_cast<int64_t>(panel.events.size()), 0);

        host.moveTo(30.0f, 10.0f, 2);                    // not pressed: plain moves, no Mods
        const bool moves = panel.events.size() == 2 && panel.events[0].kind == Kind::move
                        && panel.events[1].kind == Kind::move && sameBits(panel.events[0].e.x, 15.0f)
                        && sameBits(panel.events[0].e.y, 5.0f) && sameBits(panel.events[1].e.x, 30.0f)
                        && sameBits(panel.events[1].e.y, 10.0f) && !panel.events[1].e.mods.shift
                        && !panel.events[1].e.popup;
        P.eq("state.move_to_unpressed_is_moves", moves, 1);
        P.eq("state.pointer_after_move_to", host.pointer().x == 30.0f && host.pointer().y == 10.0f, 1);

        Mods shift;
        shift.shift = true;
        host.press(40.0f, 20.0f, shift);
        P.eq("state.pressed", host.pressed(), 1);
        P.eq("state.pointer_at_press", host.pointer().x == 40.0f && host.pointer().y == 20.0f, 1);
        host.moveTo(50.0f, 30.0f);
        P.eq("state.pointer_follows", host.pointer().x == 50.0f && host.pointer().y == 30.0f, 1);
        P.eq("state.drag_carries_press_mods", panel.events.back().kind == Kind::drag && panel.events.back().e.mods.shift,
             1);

        // A second press while pressed releases first, where the hand is, with the first press's Mods.
        const size_t before = panel.events.size();
        host.press(90.0f, 60.0f);
        const bool released = panel.events.size() == before + 3 && panel.events[before].kind == Kind::up
                           && sameBits(panel.events[before].e.x, 50.0f) && sameBits(panel.events[before].e.y, 30.0f)
                           && panel.events[before].e.mods.shift && panel.events[before + 1].kind == Kind::move
                           && panel.events[before + 2].kind == Kind::down && !panel.events[before + 2].e.mods.shift;
        P.eq("state.second_press_releases_first", released, 1);
        P.eq("state.still_pressed", host.pressed(), 1);
        host.release();
        P.eq("state.released", !host.pressed() && panel.events.back().kind == Kind::up
                                   && sameBits(panel.events.back().e.x, 90.0f), 1);
        P.eq("state.pointer_stays", host.pointer().x == 90.0f && host.pointer().y == 60.0f, 1);

        // The atomic calls release an open press first, as a second press() does: the event before their own
        // pointerMove is that press's pointerUp, where the hand is and with the press's Mods. Then come their own
        // events, which end with their own pointerUp: no press is left open, and release() then sends nothing.
        const auto releasedFirst = [&panel](size_t from, size_t count, float hx, float hy) {
            const std::vector<Event>& ev = panel.events;
            return ev.size() == from + count && ev[from].kind == Kind::up && sameBits(ev[from].e.x, hx)
                && sameBits(ev[from].e.y, hy) && ev[from].e.mods.shift && ev[from + 1].kind == Kind::move
                && !ev[from + 1].e.mods.shift && ev.back().kind == Kind::up;
        };
        host.press(10.0f, 10.0f, shift);
        host.moveTo(16.0f, 18.0f);
        size_t from = panel.events.size();
        host.click(12.0f, 12.0f);
        P.eq("state.click_releases_first", releasedFirst(from, 4, 16.0f, 18.0f), 1);    // up; move, down, up
        P.eq("state.click_ends_press", !host.pressed() && host.pointer().x == 12.0f, 1);
        host.press(10.0f, 10.0f, shift);
        host.moveTo(16.0f, 18.0f);
        from = panel.events.size();
        host.drag(10.0f, 10.0f, 70.0f, 40.0f, 2);
        P.eq("state.drag_releases_first", releasedFirst(from, 6, 16.0f, 18.0f), 1);     // up; move, down, 2 drags, up
        P.eq("state.drag_ends_press", !host.pressed() && host.pointer().x == 70.0f && host.pointer().y == 40.0f, 1);
        host.press(10.0f, 10.0f, shift);
        host.moveTo(16.0f, 18.0f);
        from = panel.events.size();
        host.doubleClick(14.0f, 14.0f);
        P.eq("state.double_click_releases_first", releasedFirst(from, 7, 16.0f, 18.0f), 1);   // up; the six of it
        P.eq("state.double_click_ends_press", !host.pressed() && host.pointer().x == 14.0f, 1);
        // Without an open press they send what they always sent: no pointerUp before their pointerMove.
        from = panel.events.size();
        host.click(12.0f, 12.0f);
        P.eq("state.click_without_press_unchanged",
             panel.events.size() == from + 3 && panel.events[from].kind == Kind::move, 1);
        const size_t n = panel.events.size();
        host.release();
        P.eq("state.release_after_atomic_sends_nothing", panel.events.size() == n, 1);
        host.move(33.0f, 44.0f);
        host.wheel(35.0f, 46.0f, 0.5f);
        P.eq("state.pointer_after_wheel", host.pointer().x == 35.0f && host.pointer().y == 46.0f, 1);
        P.eq("state.no_tick", panel.ticks, 0);
    }

    // ---- drop: a file drag as EditorHost forwards one ---------------------------------------------------------------
    {
        CablePanel panel;
        HeadlessHost host(panel);
        const std::vector<std::string> paths{ "/tmp/a.preset", "/tmp/b \xC3\xA9.preset" };
        P.eq("drop.accepted", host.dropFiles(paths, 64.5f, 32.25f), 1);
        const std::vector<std::string> want{ "interest /tmp/a.preset|/tmp/b \xC3\xA9.preset",
                                             "enter /tmp/a.preset|/tmp/b \xC3\xA9.preset @64.5,32.25",
                                             "move @64.5,32.25", "drop /tmp/a.preset|/tmp/b \xC3\xA9.preset" };
        P.eq("drop.order", panel.files == want, 1);
        P.eq("drop.counted", host.log.drops, 1);
        P.eq("drop.is_not_pointer_input", panel.events.empty() && !host.pressed() && host.pointer().x == 0.0f, 1);

        panel.files.clear();
        panel.acceptFiles = false;
        P.eq("drop.refused", host.dropFiles(paths, 10.0f, 10.0f), 0);
        P.eq("drop.refused_sends_nothing_more",
             panel.files == std::vector<std::string>{ "interest /tmp/a.preset|/tmp/b \xC3\xA9.preset" }, 1);
        P.eq("drop.refused_counted", host.log.drops, 2);
        panel.files.clear();
        panel.acceptFiles = true;
        P.eq("drop.no_paths", host.dropFiles({}, 1.0f, 2.0f), 1);         // the Panel decides, as with a real drag
        P.eq("drop.no_paths_order", panel.files == std::vector<std::string>{ "interest ", "enter  @1,2", "move @1,2",
                                                                              "drop " }, 1);
    }

    // ---- cursor: Panel::cursor() after the last event ---------------------------------------------------------------
    {
        CablePanel panel;
        HeadlessHost host(panel);
        P.eq("cursor.initial", host.cursor() == Cursor::normal, 1);
        host.move(20.0f, 20.0f);
        P.eq("cursor.grab", host.cursor() == Cursor::grab, 1);
        host.press(20.0f, 20.0f);
        P.eq("cursor.grabbing", host.cursor() == Cursor::grabbing, 1);
        host.moveTo(180.0f, 20.0f);
        P.eq("cursor.not_allowed", host.cursor() == Cursor::notAllowed, 1);
        host.release();
        P.eq("cursor.grab_again", host.cursor() == Cursor::grab, 1);
        bool all = true;
        for (const Cursor c : { Cursor::normal, Cursor::leftRight, Cursor::upDown, Cursor::pointingHand,
                                Cursor::crosshair, Cursor::grab, Cursor::grabbing, Cursor::copy, Cursor::notAllowed })
        {
            panel.shown = c;
            all = all && host.cursor() == c;
        }
        P.eq("cursor.every_value", all, 1);
        // The four new values follow the five: the numbers a product may have stored stay.
        P.eq("cursor.numbers", static_cast<int>(Cursor::crosshair) == 4 && static_cast<int>(Cursor::grab) == 5
                                   && static_cast<int>(Cursor::grabbing) == 6 && static_cast<int>(Cursor::copy) == 7
                                   && static_cast<int>(Cursor::notAllowed) == 8, 1);
    }

    return P.finish();
}

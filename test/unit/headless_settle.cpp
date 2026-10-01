// FUNKGUI_TEST name=fg.headless.settle timeout=600 gpu=0
//
// fg.headless.settle: funkgui::HeadlessHost (panel/HeadlessHost.h; 02 §3.6, §3.7) against a recording Panel. settle()
// is exact and deterministic with a fixed dt (the frame count equals the ease's own step count, twice over, and 0 when
// already settled), and reports a Panel that never settles as maxFrames + 1 after exactly maxFrames ticks; tick()
// drives Panel::tick, the simulated clock and Panel::idle; draw() fills FrameInfo from the host; the input replay
// (move, click, ctrl-click, doubleClick, drag, wheel, keys with HR's grammar plus home/end/pageup/pagedown) reaches
// the Panel as JUCE's event sequences; the HostServices calls are logged, and a GestureController's wheel burst is
// closed by the idle calls of the simulated clock; accessibility() and writeDump() (v2, parses back bit-equal) work;
// the destructor closes the Panel's gestures. Spec rows only.

#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/core/Ease.h>
#include <funkgui/core/Theme.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/panel/Panel.h>
#include <funkgui/params/GestureController.h>
#include <funkgui/params/ParamPort.h>
#include <funkgui/test/Harness.h>

#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace T = funkgui::test;
using funkgui::HeadlessHost;
using funkgui::Key;
using funkgui::Mods;

namespace
{
    bool sameBits(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }

    std::string num(double v)
    {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%g", v);
        return buf;
    }

    std::string mods(const Mods& m)
    {
        return std::string(m.shift ? "S" : "-") + (m.cmd ? "C" : "-") + (m.alt ? "A" : "-") + (m.ctrl ? "T" : "-");
    }

    const char* keyName(Key k)
    {
        switch (k)
        {
            case Key::character: return "char";
            case Key::tab:       return "tab";
            case Key::up:        return "up";
            case Key::down:      return "down";
            case Key::left:      return "left";
            case Key::right:     return "right";
            case Key::pageUp:    return "pageup";
            case Key::pageDown:  return "pagedown";
            case Key::home:      return "home";
            case Key::end:       return "end";
            case Key::escape:    return "escape";
            case Key::enter:     return "enter";
            case Key::backspace: return "backspace";
            case Key::del:       return "del";
            case Key::space:     return "space";
        }
        return "?";
    }

    // A host parameter that logs its gesture calls ("b", "s=<v>", "e").
    class FakePort final : public funkgui::ParamPort
    {
    public:
        explicit FakePort(std::vector<std::string>& log) : log_(log) {}
        float value01() const override { return v_; }
        float default01() const override { return 0.5f; }
        int   numSteps() const override { return 0; }
        void  beginGesture() override { log_.push_back("b"); }
        void  setValue01(float v) override
        {
            v_ = v;
            log_.push_back("s=" + num(static_cast<double>(v)));
        }
        void  endGesture() override { log_.push_back("e"); }
        const char* id() const override { return "fake"; }
        void* native() const override { return nullptr; }

    private:
        std::vector<std::string>& log_;
        float v_ = 0.25f;
    };

    // A Panel that records every call and eases one value towards a target.
    class RecPanel final : public funkgui::Panel
    {
    public:
        std::vector<std::string> events, portLog;
        funkgui::HostServices* host = nullptr;
        int   attaches = 0, ticks = 0, idles = 0, closes = 0;
        float value = 0.0f, target = 0.0f, lastDt = 0.0f;
        double lastIdle = -1.0;
        bool  never = false;                             // wants full rate forever
        std::vector<double> nowAtTick;

        void attach(funkgui::HostServices& h) override
        {
            host = &h;
            ++attaches;
            gestures_.emplace(h);
        }
        int  width() const override { return 320; }
        int  height() const override { return 200; }
        void tick(float dt) override
        {
            ++ticks;
            lastDt = dt;
            nowAtTick.push_back(host->nowSeconds());
            value = funkgui::ease::toward(value, target, dt, 0.1f);
        }
        void idle(double nowSec) override
        {
            ++idles;
            lastIdle = nowSec;
            gestures_->poll(nowSec);
        }
        void draw(funkgui::Canvas& c, const funkgui::Theme& th) override
        {
            c.rrect(10.0f + 100.0f * value, 10.0f, 20.0f, 20.0f, 2.0f, th.ink100);
            c.text("SETTLE", 10.0f, 50.0f, funkgui::type::kLabel, th.ink52);
        }
        bool wantsFullRate() const override { return never || !sameBits(value, target); }

        void pointerMove(const funkgui::PointerEvent& e) override { log("move", e); }
        void pointerExit() override { events.push_back("exit"); }
        void pointerDown(const funkgui::PointerEvent& e) override
        {
            log("down", e);
            if (e.popup)
                host->showParamMenu(port_, e.x, e.y);
            else
                host->setUnboundedDrag(true);
        }
        void pointerDrag(const funkgui::PointerEvent& e) override { log("drag", e); }
        void pointerUp(const funkgui::PointerEvent& e) override
        {
            log("up", e);
            host->setUnboundedDrag(false);
        }
        void doubleClick(const funkgui::PointerEvent& e) override { log("dbl", e); }
        bool wheel(const funkgui::WheelEvent& w) override
        {
            events.push_back("wheel " + num(w.x) + " " + num(w.y) + " dx " + num(w.dx) + " dy " + num(w.dy)
                             + (w.smooth ? " smooth" : " notched") + (w.reversed ? " rev" : "")
                             + (w.inertial ? " inertial" : "") + " " + mods(w.mods));
            gestures_->wheelTo(port_, port_.value01() + 0.1f * w.dy, host->nowSeconds());
            return true;
        }
        bool key(const funkgui::KeyEvent& k) override
        {
            events.push_back(std::string("key ") + keyName(k.key) + " " + mods(k.mods) + " "
                             + std::to_string(static_cast<uint32_t>(k.ch)));
            if (k.key == Key::character && k.ch == U'b')
            {
                host->beginBatch();
                host->beginBatch();
                host->endBatch();
                host->nudgeFullRate();
            }
            return true;
        }
        void accessibility(std::vector<funkgui::A11yItem>& out) const override
        {
            funkgui::A11yItem item;
            item.id = 7;
            item.role = funkgui::A11yRole::button;
            item.title = "SETTLE";
            item.bounds = { 10.0f, 10.0f, 20.0f, 20.0f };
            out.push_back(item);
        }
        uint32_t a11yRevision() const override { return 1; }
        void a11yAction(uint32_t, funkgui::A11yAction, double) override {}
        void closeGestures() override
        {
            ++closes;
            gestures_->closeAll();
        }

        std::string take()
        {
            std::string s;
            for (const auto& e : events)
                s += (s.empty() ? "" : " | ") + e;
            events.clear();
            return s;
        }

    private:
        void log(const char* what, const funkgui::PointerEvent& e)
        {
            events.push_back(std::string(what) + " " + num(e.x) + " " + num(e.y) + " " + mods(e.mods) + " c"
                             + std::to_string(e.clicks) + (e.popup ? " popup" : ""));
        }

        FakePort port_{ portLog };
        std::optional<funkgui::GestureController> gestures_;
    };

    // The number of ticks ease::toward needs to land on `target` from `from`, counted the same way settle() must.
    int easeSteps(float from, float target, float dt)
    {
        int n = 0;
        for (float v = from; !sameBits(v, target) && n < 100000; ++n)
            v = funkgui::ease::toward(v, target, dt, 0.1f);
        return n;
    }

    bool sameList(const funkgui::PrimList& a, const funkgui::PrimList& b)
    {
        if (a.prims.size() != b.prims.size() || a.info.logicalW != b.info.logicalW || a.info.frame != b.info.frame
            || !sameBits(a.info.seconds, b.info.seconds) || !sameBits(a.info.dt, b.info.dt)
            || !sameBits(a.info.dpi, b.info.dpi) || a.info.fixedClock != b.info.fixedClock)
            return false;
        for (size_t i = 0; i < a.prims.size(); ++i)
            if (std::memcmp(&a.prims[i], &b.prims[i], sizeof(funkgui::Prim)) != 0)
                return false;
        return true;
    }
}

int main(int argc, char** argv)
{
    const funkgui::HeadlessGuiScope gui;                 // JUCE's GUI side, when there is JUCE: the atlas bakes there
    T::Probe P("fg.headless.settle", "", argc, argv);
    constexpr float kDt = 1.0f / 60.0f;

    // ---- attach, settle, tick, clock --------------------------------------------------------------------------------
    {
        RecPanel panel;
        HeadlessHost host(panel, 0, 2.0f);
        P.eq("attach.once", panel.attaches == 1 && panel.host == &host, 1);
        P.eq("settle.already_settled", host.settle(), 0);
        P.eq("settle.already_settled.no_ticks", panel.ticks, 0);

        panel.target = 1.0f;
        const int want = easeSteps(0.0f, 1.0f, kDt);
        const int got = host.settle();
        std::printf("settle: %d frames (ease steps %d)\n", got, want);
        P.eq("settle.frames_exact", got, want);
        P.in("settle.frames_plausible", got, 20, 80);
        P.eq("settle.lands_exactly", sameBits(panel.value, 1.0f) && !panel.wantsFullRate(), 1);
        P.eq("settle.ticks_match", panel.ticks, got);

        panel.target = 0.25f;
        const int back = host.settle(600, 1.0f / 120.0f);
        P.eq("settle.dt_120", back, easeSteps(1.0f, 0.25f, 1.0f / 120.0f));
        P.eq("settle.dt_120.slower", back > easeSteps(1.0f, 0.25f, kDt), 1);

        // The clock: tick(n, dt) advances nowSeconds() by dt per frame, Panel::tick sees the time before its frame,
        // idle the time after it.
        const double before = host.nowSeconds();
        const int ticksBefore = panel.ticks;
        panel.nowAtTick.clear();
        host.tick(3, 0.25f);
        P.eq("tick.count", panel.ticks - ticksBefore, 3);
        P.near("tick.clock", host.nowSeconds(), ((before + 0.25) + 0.25) + 0.25, 0.0);
        P.eq("tick.tick_sees_frame_start", panel.nowAtTick.size() == 3 && panel.nowAtTick[0] == before
                                               && panel.nowAtTick[2] == (before + 0.25) + 0.25, 1);
        P.eq("tick.idle_after_tick", panel.idles == panel.ticks && panel.lastIdle == host.nowSeconds(), 1);
        host.tick(0, 5.0f);
        P.eq("tick.zero_frames", panel.ticks - ticksBefore == 3 && sameBits(panel.lastDt, 0.25f), 1);

        // draw(): FrameInfo from the host.
        const funkgui::PrimList& l = host.draw();
        const funkgui::Theme graphite = funkgui::Theme::graphite();
        P.eq("draw.info", l.info.logicalW == 320 && l.info.logicalH == 200 && sameBits(l.info.dpi, 2.0f)
                              && l.info.clear.r == graphite.ground.r && l.info.clear.b == graphite.ground.b
                              && sameBits(l.info.textGamma, graphite.textGamma) && l.info.theme == 0
                              && l.info.frame == static_cast<uint32_t>(panel.ticks)
                              && sameBits(l.info.seconds, static_cast<float>(host.nowSeconds()))
                              && sameBits(l.info.dt, 0.25f) && l.info.fixedClock && !l.info.displayLinked
                              && sameBits(l.info.fps, 0.0f) && l.info.fullRate == panel.wantsFullRate()
                              && l.info.overflows == 0, 1);
        P.eq("draw.prims", static_cast<int64_t>(l.prims.size()), 1 + 6);
        P.eq("draw.no_missing_glyphs", l.missingGlyphs, 0);
    }
    {
        // Never settles: exactly maxFrames ticks, reported as maxFrames + 1.
        RecPanel panel;
        panel.never = true;
        HeadlessHost host(panel);
        P.eq("settle.never.default_limit", host.settle(), 601);
        P.eq("settle.never.ticks", panel.ticks, 600);
        P.eq("settle.never.small_limit", host.settle(50), 51);
        P.eq("settle.never.zero_limit", host.settle(0), 1);
        P.eq("settle.never.ticks_total", panel.ticks, 650);
    }
    {
        // The same settle from a fresh panel and host gives the same frame count and the same frame.
        const auto run = [](float dpi) {
            RecPanel panel;
            HeadlessHost host(panel, 1, dpi);
            panel.target = 0.8f;
            const int n = host.settle();
            const funkgui::PrimList l = host.draw();
            return std::make_pair(n, l);
        };
        const auto a = run(2.0f), b = run(2.0f);
        P.eq("settle.deterministic", a.first == b.first && sameList(a.second, b.second), 1);
        P.eq("draw.theme1", a.second.info.theme == 1 && a.second.info.clear.r == funkgui::Theme::paper().ground.r
                                && sameBits(a.second.info.textGamma, funkgui::Theme::paper().textGamma), 1);
    }

    // ---- input replay -----------------------------------------------------------------------------------------------
    {
        RecPanel panel;
        HeadlessHost host(panel);
        host.move(10.5f, 20.0f);
        P.eq("input.move", panel.take() == "move 10.5 20 ---- c1", 1);

        Mods shift;
        shift.shift = true;
        host.click(10.0f, 20.0f, shift);
        P.eq("input.click", panel.take() == "move 10 20 S--- c1 | down 10 20 S--- c1 | up 10 20 S--- c1", 1);
        P.eq("input.click.unbounded", !host.log.unbounded && host.log.menus == 0, 1);

        Mods ctrl;
        ctrl.ctrl = true;
        host.click(3.0f, 4.0f, ctrl);
        P.eq("input.ctrl_click_is_popup",
             panel.take() == "move 3 4 ---T c1 popup | down 3 4 ---T c1 popup | up 3 4 ---T c1 popup"
                 && host.log.menus == 1, 1);

        host.doubleClick(5.0f, 6.0f);
        P.eq("input.double_click", panel.take() == "move 5 6 ---- c1 | down 5 6 ---- c1 | up 5 6 ---- c1 | "
                                                   "down 5 6 ---- c2 | dbl 5 6 ---- c2 | up 5 6 ---- c2", 1);

        host.drag(0.0f, 0.0f, 10.0f, 20.0f, 4);
        P.eq("input.drag", panel.take() == "move 0 0 ---- c1 | down 0 0 ---- c1 | drag 2.5 5 ---- c1 | "
                                           "drag 5 10 ---- c1 | drag 7.5 15 ---- c1 | drag 10 20 ---- c1 | "
                                           "up 10 20 ---- c1", 1);
        host.drag(1.0f, 1.0f, 3.0f, 3.0f, 0);
        P.eq("input.drag_min_one_step",
             panel.take() == "move 1 1 ---- c1 | down 1 1 ---- c1 | drag 3 3 ---- c1 | up 3 3 ---- c1", 1);

        Mods cmd;
        cmd.cmd = true;
        host.wheel(3.0f, 4.0f, 0.5f, true, cmd);
        P.eq("input.wheel", panel.take() == "move 3 4 -C-- c1 | wheel 3 4 dx 0 dy 0.5 smooth -C--", 1);

        host.keys(" tab , shift+tab,Up,DOWN,cmd+A,x,return,space,home,end,pageup,pagedown,delete,backspace,escape,"
                  "alt+left,ctrl+right,SHIFT+CMD+z,bogus,,\xC2\xB0,left");
        const std::string keys = panel.take();
        const std::string want = "key tab ---- 0 | key tab S--- 0 | key up ---- 0 | key down ---- 0 | "
                                 "key char -C-- 97 | key char ---- 120 | key enter ---- 0 | key space ---- 32 | "
                                 "key home ---- 0 | key end ---- 0 | key pageup ---- 0 | key pagedown ---- 0 | "
                                 "key del ---- 0 | key backspace ---- 0 | key escape ---- 0 | key left --A- 0 | "
                                 "key right ---T 0 | key char SC-- 122 | key char ---- 176 | key left ---- 0";
        if (!P.eq("input.keys", keys == want, 1))
            std::printf("  keys: %s\n  want: %s\n", keys.c_str(), want.c_str());
        host.keys(nullptr);
        host.keys("");
        P.eq("input.keys_empty", panel.take().empty(), 1);

        // HostServices log: batches nest, nudges count.
        host.keys("b");
        panel.take();
        P.eq("services.log", host.log.batches == 2 && host.log.batchDepth == 1 && host.log.nudges == 1, 1);

        // A wheel burst through GestureController is closed by the host's idle calls once kWheelIdle has passed (the
        // first 40 ticks close the burst input.wheel opened).
        host.tick(40, kDt);
        panel.portLog.clear();
        host.wheel(1.0f, 1.0f, 1.0f);
        host.wheel(1.0f, 1.0f, 1.0f);
        const size_t open = panel.portLog.size();
        host.tick(29, kDt);                              // 0.483 s: still open
        const bool stillOpen = panel.portLog.size() == open;
        host.tick(2, kDt);                               // 0.517 s >= kWheelIdle
        P.eq("services.wheel_burst_closed_by_idle", stillOpen && open == 3 && panel.portLog.size() == 4
                                                        && panel.portLog.back() == "e", 1);
        panel.take();

        const auto items = host.accessibility();
        P.eq("a11y.items", items.size() == 1 && items[0].id == 7 && items[0].title == "SETTLE", 1);
    }

    // ---- writeDump, and the destructor ------------------------------------------------------------------------------
    {
        RecPanel panel;
        {
            HeadlessHost host(panel, 0, 2.0f);
            const char* path = "headless_settle.dump";
            std::remove(path);
            P.eq("dump.nothing_drawn", host.writeDump(path), 0);
            panel.target = 0.5f;
            host.settle();
            const funkgui::PrimList drawn = host.draw();
            P.eq("dump.written", host.writeDump(path), 1);
            P.eq("dump.null_path", host.writeDump(nullptr), 0);
            std::ifstream in(path, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            funkgui::PrimList back;
            const bool parsed = funkgui::PrimList::parseText(ss.str(), back);
            P.eq("dump.parses_bit_equal", parsed && sameList(back, drawn), 1);
            std::ifstream partial("headless_settle.dump.partial");
            P.eq("dump.no_partial_left", !partial.good(), 1);
            P.eq("destructor.not_yet", panel.closes, 0);
        }
        P.eq("destructor.closes_gestures", panel.closes, 1);
    }
    return P.finish();
}

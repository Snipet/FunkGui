// FUNKGUI_TEST name=fg.gesture timeout=300 gpu=0
//
// fg.gesture: GestureController (params/GestureController.h; 02 §5.2, A §3.5, K2 #7/#23/#27) against a recording fake
// port and fake host. Each case runs on fresh ports and compares the exact event sequence the host would see:
// b:<id> beginGesture, s:<id>=<v> setValue01, e:<id> endGesture, u1/u0 setUnboundedDrag, B/E beginBatch/endBatch.
// Also ValueModel::writeDetent's default (widgets/ValueModel.h), which writes through tap. Spec rows only.

#include <funkgui/panel/HostServices.h>
#include <funkgui/params/GestureController.h>
#include <funkgui/params/ParamPort.h>
#include <funkgui/test/Harness.h>
#include <funkgui/widgets/ValueModel.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace T = funkgui::test;

namespace
{
    struct EventLog
    {
        std::vector<std::string> events;

        void add(std::string e) { events.push_back(std::move(e)); }

        std::string take()
        {
            std::string s;
            for (const auto& e : events)
            {
                if (!s.empty())
                    s += ' ';
                s += e;
            }
            events.clear();
            return s;
        }
    };

    std::string num(float v)
    {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%g", static_cast<double>(v));
        return buf;
    }

    // A host parameter that records every call. `quantum` > 0 makes it store its value rounded to that step, like a
    // JUCE parameter with an interval, so value01() can differ from what was written.
    class FakePort final : public funkgui::ParamPort
    {
    public:
        FakePort(const char* id, EventLog& log, float value = 0.0f, float quantum = 0.0f)
            : id_(id), log_(log), value_(value), quantum_(quantum) {}

        float value01() const override { return value_; }
        float default01() const override { return 0.0f; }
        int   numSteps() const override { return std::numeric_limits<int>::max(); }

        void beginGesture() override
        {
            if (open_ > 0)
                nested_ = true;
            ++open_;
            log_.add(std::string("b:") + id_);
        }

        void setValue01(float v) override
        {
            if (open_ == 0)
                writeOutsideGesture_ = true;
            value_ = quantum_ > 0.0f ? std::round(v / quantum_) * quantum_ : v;
            log_.add(std::string("s:") + id_ + "=" + num(v));
        }

        void endGesture() override
        {
            if (open_ == 0)
                unmatchedEnd_ = true;
            else
                --open_;
            log_.add(std::string("e:") + id_);
        }

        const char* id() const override { return id_; }
        void* native() const override { return nullptr; }

        // The discipline JUCE asserts on: no nested begin, no end without a begin, no write outside a gesture.
        bool clean() const { return open_ == 0 && !nested_ && !unmatchedEnd_ && !writeOutsideGesture_; }

    private:
        const char* id_;
        EventLog&   log_;
        float       value_;
        float       quantum_;
        int         open_ = 0;
        bool        nested_ = false, unmatchedEnd_ = false, writeOutsideGesture_ = false;
    };

    class FakeHost final : public funkgui::HostServices
    {
    public:
        explicit FakeHost(EventLog& log) : log_(log) {}

        void   setUnboundedDrag(bool on) override { log_.add(on ? "u1" : "u0"); }
        void   showParamMenu(funkgui::ParamPort& p, float, float) override { log_.add(std::string("menu:") + p.id()); }
        void   nudgeFullRate() override { log_.add("nudge"); }
        double nowSeconds() const override { return 0.0; }
        void   beginBatch() override { ++depth; log_.add("B"); }
        void   endBatch() override { --depth; log_.add("E"); }

        int depth = 0;

    private:
        EventLog& log_;
    };

    // A stepped slot with three detents, for ValueModel::writeDetent's default.
    class FakeModel final : public funkgui::ValueModel
    {
    public:
        explicit FakeModel(funkgui::ParamPort* p) : port_(p) {}

        uint64_t key() const override { return 1; }
        void view(funkgui::ValueView& v) const override
        {
            v.state = state;
            v.nDetents = 3;
            v.detents = kDetents;
        }
        funkgui::ParamPort* port() override { return port_; }
        float host01FromTrack(float t) const override { return t; }
        float defaultHost01() const override { return 0.5f; }

        funkgui::ValueState state = funkgui::ValueState::stepped;

    private:
        static constexpr funkgui::Detent kDetents[3] = { { 0.0f, "2", "2 to 1" }, { 0.5f, "4", "4 to 1" },
                                                          { 1.0f, "10", "10 to 1" } };
        funkgui::ParamPort* port_;
    };

    // Checks one step's events; prints both sequences when they differ.
    void expect(T::Probe& P, std::string_view key, EventLog& log, std::string_view want)
    {
        const std::string got = log.take();
        if (got != want)
            std::printf("INFO     %.*s: got \"%s\", want \"%.*s\"\n", static_cast<int>(key.size()), key.data(),
                        got.c_str(), static_cast<int>(want.size()), want.data());
        P.eq(key, got == want, 1);
    }
}

int main(int argc, char** argv)
{
    T::Probe P("fg.gesture", "", argc, argv);
    using funkgui::GestureController;

    P.near("wheel_idle_seconds", GestureController::kWheelIdle, 0.5, 0.0);

    // ---- tap: begin/set/end, nothing at all when unchanged, clamped, NaN refused ------------------------------------
    {
        EventLog log;
        FakeHost host(log);
        FakePort a("a", log, 0.25f);
        GestureController g(host);
        g.tap(a, 0.25f);
        expect(P, "tap.unchanged_writes_nothing", log, "");
        g.tap(a, 0.5f);
        expect(P, "tap.triple", log, "b:a s:a=0.5 e:a");
        g.tap(a, 1.5f);
        expect(P, "tap.clamps_high", log, "b:a s:a=1 e:a");
        g.tap(a, 1.0f);
        expect(P, "tap.clamped_unchanged_writes_nothing", log, "");
        g.tap(a, -3.0f);
        expect(P, "tap.clamps_low", log, "b:a s:a=0 e:a");
        g.tap(a, std::numeric_limits<float>::quiet_NaN());
        expect(P, "tap.nan_writes_nothing", log, "");
        g.tap(a, -0.0f);
        expect(P, "tap.minus_zero_is_zero", log, "");
        P.eq("tap.no_open_state", g.dragging() || g.wheeling(), 0);
        P.eq("tap.port_discipline", a.clean(), 1);
        P.eq("tap.host_is_the_given_one", &g.host() == &host, 1);
    }

    // ---- drag: one gesture from down to up, unbounded pointer, writes only when the bits change ---------------------
    {
        EventLog log;
        FakeHost host(log);
        FakePort a("a", log, 0.5f);
        GestureController g(host);
        g.beginDrag(a);
        expect(P, "drag.begin", log, "b:a u1");
        P.eq("drag.dragging", g.dragging(), 1);
        P.eq("drag.param", g.dragParam() == &a, 1);
        g.dragTo(0.5f);
        expect(P, "drag.to_start_value_writes_nothing", log, "");
        g.dragTo(0.75f);
        g.dragTo(0.75f);
        expect(P, "drag.same_bits_written_once", log, "s:a=0.75");
        g.dragTo(7.0f);
        g.dragTo(std::numeric_limits<float>::quiet_NaN());
        expect(P, "drag.clamps_and_refuses_nan", log, "s:a=1");
        g.endDrag();
        expect(P, "drag.end", log, "e:a u0");
        P.eq("drag.not_dragging", g.dragging(), 0);
        P.eq("drag.param_null", g.dragParam() == nullptr, 1);
        g.endDrag();
        g.dragTo(0.1f);
        expect(P, "drag.end_and_move_without_drag_do_nothing", log, "");
        P.eq("drag.port_discipline", a.clean(), 1);
    }

    // ---- drag against a parameter that quantises what it stores: compared with the last write, not value01() --------
    {
        EventLog log;
        FakeHost host(log);
        FakePort q("q", log, 0.0f, 0.25f);
        GestureController g(host);
        g.beginDrag(q);
        g.dragTo(0.3f);                                  // stored as 0.25
        g.dragTo(0.3f);                                  // same bits as the last write: nothing
        g.dragTo(0.25f);                                 // new bits: written although the stored value is equal
        g.endDrag();
        expect(P, "drag.quantising_port", log, "b:q u1 s:q=0.3 s:q=0.25 e:q u0");
        P.eq("drag.quantising_port_discipline", q.clean(), 1);
    }

    // ---- a new drag ends the previous one; a tap on the dragged port writes inside the drag's gesture ---------------
    {
        EventLog log;
        FakeHost host(log);
        FakePort a("a", log, 0.0f), b("b", log, 0.0f);
        GestureController g(host);
        g.beginDrag(a);
        g.beginDrag(b);
        expect(P, "drag.replaces_previous", log, "b:a u1 e:a u0 b:b u1");
        g.tap(b, 0.5f);
        expect(P, "drag.tap_on_dragged_port_inside_gesture", log, "s:b=0.5");
        g.tap(b, 0.5f);
        expect(P, "drag.tap_on_dragged_port_unchanged", log, "");
        g.dragTo(0.5f);
        expect(P, "drag.after_tap_same_value_writes_nothing", log, "");
        g.endDrag();
        expect(P, "drag.end_after_tap", log, "e:b u0");
        P.eq("drag.replace_discipline", a.clean() && b.clean(), 1);
    }

    // ---- wheel: one gesture per burst, closed kWheelIdle after its last write ---------------------------------------
    {
        EventLog log;
        FakeHost host(log);
        FakePort a("a", log, 0.5f), b("b", log, 0.5f);
        GestureController g(host);
        g.wheelTo(a, 0.525f, 10.0);
        g.wheelTo(a, 0.55f, 10.25);
        expect(P, "wheel.one_gesture_per_burst", log, "b:a s:a=0.525 s:a=0.55");
        P.eq("wheel.wheeling", g.wheeling() && g.wheelParam() == &a, 1);
        g.poll(10.5);
        g.poll(10.74);
        expect(P, "wheel.open_before_idle", log, "");
        g.poll(10.75);
        expect(P, "wheel.closed_at_idle", log, "e:a");
        P.eq("wheel.not_wheeling", g.wheeling() || g.wheelParam() != nullptr, 0);
        g.poll(20.0);
        expect(P, "wheel.poll_without_burst", log, "");

        g.wheelTo(a, 0.6f, 30.0);
        g.wheelTo(a, 0.6f, 30.4);                        // unchanged: no write, the burst is not extended
        g.poll(30.45);
        expect(P, "wheel.unchanged_notch_does_not_extend", log, "b:a s:a=0.6");
        g.poll(30.5);
        expect(P, "wheel.idle_from_last_write", log, "e:a");

        g.wheelTo(a, 0.65f, 40.0);
        g.wheelTo(b, 0.45f, 40.1);
        expect(P, "wheel.other_port_ends_burst", log, "b:a s:a=0.65 e:a b:b s:b=0.45");
        g.wheelTo(b, 2.0f, 40.2);
        g.wheelTo(b, std::numeric_limits<float>::quiet_NaN(), 40.3);
        expect(P, "wheel.clamps_and_refuses_nan", log, "s:b=1");
        g.closeAll();
        expect(P, "wheel.close_all", log, "e:b");
        P.eq("wheel.discipline", a.clean() && b.clean(), 1);
    }

    // ---- one open gesture per port: drag and tap on the wheel's port end the burst first; wheel on the dragged port
    //      writes inside the drag -------------------------------------------------------------------------------------
    {
        EventLog log;
        FakeHost host(log);
        FakePort a("a", log, 0.0f);
        GestureController g(host);
        g.wheelTo(a, 0.1f, 1.0);
        g.beginDrag(a);
        expect(P, "mixed.drag_ends_wheel_burst", log, "b:a s:a=0.1 e:a b:a u1");
        g.wheelTo(a, 0.2f, 1.1);
        expect(P, "mixed.wheel_on_dragged_port_inside_drag", log, "s:a=0.2");
        P.eq("mixed.no_burst_opened", g.wheeling(), 0);
        g.endDrag();
        g.wheelTo(a, 0.3f, 2.0);
        g.tap(a, 0.9f);
        expect(P, "mixed.tap_ends_wheel_burst", log, "e:a u0 b:a s:a=0.3 e:a b:a s:a=0.9 e:a");
        P.eq("mixed.discipline", a.clean(), 1);
    }

    // ---- tapMany: one bracket per changed parameter inside one host batch; nothing at all when nothing changes ------
    {
        EventLog log;
        FakeHost host(log);
        FakePort a("a", log, 0.0f), b("b", log, 0.5f), c("c", log, 0.25f);
        GestureController g(host);
        const std::pair<funkgui::ParamPort*, float> writes[] = {
            { &a, 0.1f }, { nullptr, 0.3f }, { &c, 0.25f }, { &b, 0.2f } };
        g.tapMany(writes);
        expect(P, "tap_many.batch", log, "B b:a s:a=0.1 e:a b:b s:b=0.2 e:b E");
        P.eq("tap_many.batch_closed", host.depth, 0);
        g.tapMany(writes);
        expect(P, "tap_many.unchanged_no_batch", log, "");
        const std::pair<funkgui::ParamPort*, float> none[] = { { nullptr, 0.5f },
                                                               { &a, std::numeric_limits<float>::quiet_NaN() } };
        g.tapMany(none);
        g.tapMany({});
        expect(P, "tap_many.null_nan_and_empty_do_nothing", log, "");
        g.beginDrag(a);
        const std::pair<funkgui::ParamPort*, float> withDrag[] = { { &a, 0.6f }, { &b, 0.7f } };
        g.tapMany(withDrag);
        g.endDrag();
        expect(P, "tap_many.dragged_port_inside_drag", log, "b:a u1 B s:a=0.6 b:b s:b=0.7 e:b E e:a u0");
        P.eq("tap_many.discipline", a.clean() && b.clean() && c.clean(), 1);
    }

    // ---- closeAll and the destructor end every open gesture (the host closing the editor mid-gesture) ---------------
    {
        EventLog log;
        FakeHost host(log);
        FakePort a("a", log, 0.0f), b("b", log, 0.0f);
        {
            GestureController g(host);
            g.beginDrag(a);
            g.wheelTo(b, 0.5f, 0.0);
            g.closeAll();
            expect(P, "close_all.drag_then_wheel", log, "b:a u1 b:b s:b=0.5 e:a u0 e:b");
            g.closeAll();
            expect(P, "close_all.twice_is_nothing", log, "");
            g.beginDrag(a);
            g.wheelTo(b, 0.75f, 1.0);
            log.take();
        }
        expect(P, "destructor.closes_all", log, "e:a u0 e:b");
        P.eq("destructor.discipline", a.clean() && b.clean(), 1);
    }

    // ---- ValueModel::writeDetent's default: a tap of the detent's host01; refused out of range, display-only and
    //      for locked / derived / n/a ---------------------------------------------------------------------------------
    {
        EventLog log;
        FakeHost host(log);
        FakePort a("a", log, 0.0f);
        GestureController g(host);
        FakeModel m(&a);
        m.writeDetent(1, g);
        expect(P, "detent.writes_host01", log, "b:a s:a=0.5 e:a");
        m.writeDetent(1, g);
        expect(P, "detent.unchanged_writes_nothing", log, "");
        m.writeDetent(3, g);
        m.writeDetent(-1, g);
        expect(P, "detent.out_of_range_writes_nothing", log, "");
        for (const auto s : { funkgui::ValueState::locked, funkgui::ValueState::derived, funkgui::ValueState::na })
        {
            m.state = s;
            m.writeDetent(2, g);
        }
        expect(P, "detent.locked_derived_na_refuse", log, "");
        FakeModel display(nullptr);
        display.writeDetent(2, g);
        expect(P, "detent.display_only_writes_nothing", log, "");
        P.eq("detent.discipline", a.clean(), 1);
    }

    return P.finish();
}

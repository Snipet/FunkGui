// FUNKGUI_TEST name=fg.ruleslider.input timeout=600 gpu=0
//
// fg.ruleslider.input: RuleSlider and AttachedWord input rules (widgets/RuleSlider.h, AttachedWord.h; 02 §5.4, §5.5,
// §8.1–§8.4, §8.9; F §3.4–§3.5) against a recording fake port and host, driven through the widgets' own entry points.
// Each case compares the exact event sequence the host would see (b:<id> beginGesture, s:<id>=<v> setValue01,
// e:<id> endGesture, u1/u0 setUnboundedDrag, menu:<id> host menu): continuous drags in track space (240 px per track,
// Shift 1200, Cmd 6000, re-anchored on a modifier toggle, soft notches stick within ±4 px); stepped drags (pitch
// clamp(240/(n-1), 24, 64), commit at 0.5·p + 6 px, one gesture, only detent values, an end stop, the ghost caret);
// detent-label clicks (3 px slop, relative drag past it); the wheel (HR's continuous rule, one detent per discrete
// event, kWheelNotch accumulation, one burst); keys (§8.9) and double-click (the Mode default); index-space a11y;
// hybrid end cells (18 px of travel past the edge, keys, wheel and a11y crossing one cell at a time); locked, derived
// and n/a refusing every write while the host menu still opens; the §8.3 detent-label fit rule on its worked cases;
// the tag that moves to the detent line beside a word; the label flash and the Mode-switch landing; spec lines;
// accessibility items; and the word's arm / commit / drag-off / disabled / hidden rules. Spec rows only.

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Ease.h>
#include <funkgui/core/Theme.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/params/GestureController.h>
#include <funkgui/params/ParamPort.h>
#include <funkgui/test/Harness.h>
#include <funkgui/text/FontService.h>
#include <funkgui/widgets/AttachedWord.h>
#include <funkgui/widgets/RuleSlider.h>
#include <funkgui/widgets/ValueModel.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace T = funkgui::test;
using funkgui::A11yAction;
using funkgui::A11yItem;
using funkgui::Detent;
using funkgui::GestureController;
using funkgui::Key;
using funkgui::KeyEvent;
using funkgui::Mods;
using funkgui::PointerEvent;
using funkgui::RuleSlider;
using funkgui::SlotGeom;
using funkgui::SlotSize;
using funkgui::ValueState;
using funkgui::ValueView;
using funkgui::WheelEvent;

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

    class FakePort final : public funkgui::ParamPort
    {
    public:
        FakePort(const char* id, EventLog& log, float value = 0.0f) : id_(id), log_(log), value_(value) {}

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
                outside_ = true;
            value_ = v;
            log_.add(std::string("s:") + id_ + "=" + num(v));
        }

        void endGesture() override
        {
            if (open_ == 0)
                unmatched_ = true;
            else
                --open_;
            log_.add(std::string("e:") + id_);
        }

        const char* id() const override { return id_; }
        void* native() const override { return nullptr; }

        void set(float v) { value_ = v; }                 // a host or automation change: no event
        bool clean() const { return open_ == 0 && !nested_ && !unmatched_ && !outside_; }

    private:
        const char* id_;
        EventLog&   log_;
        float       value_;
        int         open_ = 0;
        bool        nested_ = false, unmatched_ = false, outside_ = false;
    };

    class FakeHost final : public funkgui::HostServices
    {
    public:
        explicit FakeHost(EventLog& log) : log_(log) {}

        void   setUnboundedDrag(bool on) override { log_.add(on ? "u1" : "u0"); }
        void   showParamMenu(funkgui::ParamPort& p, float, float) override { log_.add(std::string("menu:") + p.id()); }
        void   nudgeFullRate() override {}
        double nowSeconds() const override { return 0.0; }
        void   beginBatch() override { log_.add("B"); }
        void   endBatch() override { log_.add("E"); }

    private:
        EventLog& log_;
    };

    // A slot as a product describes it. The continuous track spans host01 [rangeLo, rangeHi]; hybrid end cells and
    // detents carry their own host01; the view is recomputed from the port on every call, as FCompressor's SlotModel
    // does from its parameter.
    struct Spec
    {
        ValueState state = ValueState::continuous;
        const char* label = "THRESHOLD";
        const char* aka = nullptr;
        const char* tag = nullptr;
        const char* reason = nullptr;
        std::vector<Detent> detents, lo, hi;
        std::vector<float> notches;
        float rangeLo = 0.0f, rangeHi = 1.0f;
        float def = 0.25f;
        float fixedTrack = 0.4f;                          // locked / derived / n/a track
        bool  bipolar = false, clamped = false;
    };

    class Model final : public funkgui::ValueModel
    {
    public:
        Model(Spec s, FakePort* p) : spec(std::move(s)), port_(p) {}

        uint64_t key() const override
        {
            uint64_t k = rev * 0x9E3779B97F4A7C15ull;
            if (port_ != nullptr)
                k ^= std::bit_cast<uint32_t>(port_->value01());
            return k;
        }

        void view(ValueView& v) const override
        {
            const float h = port_ != nullptr ? port_->value01() : 0.0f;
            v.state = spec.state;
            v.label = spec.label;
            v.aka = spec.aka;
            v.tag = spec.tag;
            v.reason = spec.reason;
            v.bipolar = spec.bipolar;
            v.clamped = spec.clamped;
            v.trackDefault = trackOf(spec.def);
            v.nNotches = static_cast<int>(spec.notches.size());
            v.notches = spec.notches.empty() ? nullptr : spec.notches.data();
            if (spec.state == ValueState::stepped)
            {
                v.nDetents = static_cast<int>(spec.detents.size());
                v.detents = spec.detents.data();
                int best = 0;
                for (int i = 1; i < v.nDetents; ++i)
                    if (std::fabs(spec.detents[static_cast<size_t>(i)].host01 - h)
                        < std::fabs(spec.detents[static_cast<size_t>(best)].host01 - h))
                        best = i;
                v.detent = best;
                v.track = (static_cast<float>(best) + 0.5f) / static_cast<float>(v.nDetents);
                std::snprintf(v.text.value, sizeof v.text.value, "%s", spec.detents[static_cast<size_t>(best)].label);
                return;
            }
            if (spec.state == ValueState::continuous)
            {
                v.nEndLo = static_cast<int>(spec.lo.size());
                v.endLo = spec.lo.empty() ? nullptr : spec.lo.data();
                v.nEndHi = static_cast<int>(spec.hi.size());
                v.endHi = spec.hi.empty() ? nullptr : spec.hi.data();
                for (size_t i = 0; i < spec.lo.size(); ++i)
                    if (funkgui::ease::sameBits(spec.lo[i].host01, h))
                        v.activeEnd = -static_cast<int>(i) - 1;
                for (size_t i = 0; i < spec.hi.size(); ++i)
                    if (funkgui::ease::sameBits(spec.hi[i].host01, h))
                        v.activeEnd = static_cast<int>(i) + 1;
                v.track = trackOf(h);
                v.atDefault = funkgui::ease::sameBits(h, spec.def);
                if (v.activeEnd != 0)
                {
                    std::snprintf(v.text.value, sizeof v.text.value, "OFF");
                    std::snprintf(v.text.spoken, sizeof v.text.spoken, "Off");
                }
                else
                {
                    std::snprintf(v.text.value, sizeof v.text.value, "%.2f", static_cast<double>(h));
                    std::snprintf(v.text.unit, sizeof v.text.unit, "DB");
                }
                return;
            }
            v.track = spec.fixedTrack;
            std::snprintf(v.text.value, sizeof v.text.value, "10");
            std::snprintf(v.text.unit, sizeof v.text.unit, "MS");
            std::snprintf(v.text.sub, sizeof v.text.sub, "T4 CELL");
        }

        funkgui::ParamPort* port() override { return port_; }
        float host01FromTrack(float t) const override { return spec.rangeLo + t * (spec.rangeHi - spec.rangeLo); }
        float defaultHost01() const override { return spec.def; }

        Spec     spec;
        uint64_t rev = 1;

    private:
        float trackOf(float h) const
        {
            const float t = (h - spec.rangeLo) / (spec.rangeHi - spec.rangeLo);
            return t <= 0.0f ? 0.0f : (t >= 1.0f ? 1.0f : t);
        }

        FakePort* port_;
    };

    class Toggle final : public funkgui::WordModel
    {
    public:
        explicit Toggle(FakePort& p) : port_(p) {}

        bool on() const override { return port_.value01() > 0.5f; }
        bool enabled() const override { return enabledFlag; }
        const char* reason() const override { return "AUTO MAKEUP IS FIXED IN THIS MODE"; }
        void set(bool v, GestureController& g) override { g.tap(port_, v ? 1.0f : 0.0f); }
        funkgui::ParamPort* port() override { return &port_; }
        bool visible() const override { return visibleFlag; }

        bool enabledFlag = true, visibleFlag = true;

    private:
        FakePort& port_;
    };

    void expect(T::Probe& P, std::string_view key, EventLog& log, std::string_view want)
    {
        const std::string got = log.take();
        if (got != want)
            std::printf("INFO     %.*s: got \"%s\", want \"%.*s\"\n", static_cast<int>(key.size()), key.data(),
                        got.c_str(), static_cast<int>(want.size()), want.data());
        P.eq(key, got == want, 1);
    }

    PointerEvent at(float x, float y, Mods m = {}, int clicks = 1)
    {
        PointerEvent e;
        e.x = x;
        e.y = y;
        e.mods = m;
        e.clicks = clicks;
        e.popup = false;
        return e;
    }

    PointerEvent popupAt(float x, float y)
    {
        PointerEvent e = at(x, y);
        e.popup = true;
        return e;
    }

    WheelEvent wheelOf(float dy, bool smooth = false, Mods m = {}, float dx = 0.0f, bool reversed = false)
    {
        WheelEvent w;
        w.x = 60.0f;
        w.y = 120.0f;
        w.dx = dx;
        w.dy = dy;
        w.smooth = smooth;
        w.reversed = reversed;
        w.mods = m;
        return w;
    }

    KeyEvent keyOf(Key k, Mods m = {})
    {
        KeyEvent e;
        e.key = k;
        e.mods = m;
        return e;
    }

    Mods shift()
    {
        Mods m;
        m.shift = true;
        return m;
    }

    Mods cmd()
    {
        Mods m;
        m.cmd = true;
        return m;
    }

    // The standard slot: x 40, top 100, w 112, primary. Track y 164, detent line y 146, hit {34, 94, 124, 80}.
    constexpr SlotGeom kGeom{ 40.0f, 100.0f, 112.0f, SlotSize::primary };
    constexpr float kY = 120.0f;                          // a row inside the hit, clear of the detent labels

    std::vector<Detent> ratio3()
    {
        return { { 0.0f, "2", "2 to 1" }, { 0.5f, "4", "4 to 1" }, { 1.0f, "10", "10 to 1" } };
    }

    std::vector<Detent> labels(std::initializer_list<const char*> names)
    {
        std::vector<Detent> d;
        const float n = static_cast<float>(names.size());
        float i = 0.0f;
        for (const char* s : names)
        {
            d.push_back({ n > 1.0f ? i / (n - 1.0f) : 0.0f, s, s });
            i += 1.0f;
        }
        return d;
    }

    Spec stepped(std::vector<Detent> d, const char* label = "RATIO")
    {
        Spec s;
        s.state = ValueState::stepped;
        s.label = label;
        s.detents = std::move(d);
        s.def = s.detents.empty() ? 0.0f : s.detents[s.detents.size() / 2].host01;
        return s;
    }

    Spec hybridSpec()
    {
        Spec s;
        s.label = "ATTACK";
        s.rangeLo = 0.1f;
        s.rangeHi = 1.0f;
        s.def = 0.5f;
        s.lo = { { 0.0f, "OFF", "Off" } };
        return s;
    }

    // Counts of one frame's primitives per tag, drawn at dpi 1.
    struct Frame
    {
        std::vector<funkgui::Prim> prims;

        int count(funkgui::Tag t) const
        {
            int n = 0;
            for (const auto& p : prims)
                n += p.tag == t ? 1 : 0;
            return n;
        }

        // The centre x of the n-th primitive with tag t (-1 when absent).
        float centreX(funkgui::Tag t, int nth = 0) const
        {
            for (const auto& p : prims)
                if (p.tag == t && nth-- == 0)
                    return 0.5f * (p.x0 + p.x1);
            return -1.0f;
        }

        uint32_t colour(funkgui::Tag t) const
        {
            for (const auto& p : prims)
                if (p.tag == t)
                    return p.c0;
            return 0;
        }
    };

    Frame drawOf(const RuleSlider& s, bool ring = false)
    {
        static funkgui::Canvas canvas(funkgui::FontService::get().atlas());
        funkgui::FrameInfo fi;
        fi.logicalW = 400;
        fi.logicalH = 300;
        fi.dpi = 1.0f;
        canvas.begin(fi);
        s.draw(canvas, funkgui::Theme::graphite(), ring);
        return Frame{ canvas.end().prims };
    }

    uint32_t packed(funkgui::Col c)
    {
        return static_cast<uint32_t>(c.r) | static_cast<uint32_t>(c.g) << 8 | static_cast<uint32_t>(c.b) << 16
             | static_cast<uint32_t>(c.a) << 24;
    }

    std::string spec(const RuleSlider& s, size_t n = 256)
    {
        std::vector<char> buf(n, 'x');
        s.specLine(buf.data(), n);
        return buf.data();
    }

    // Ticks until settled (at most `limit`); the tick count.
    int settleFrames(RuleSlider& s, int limit = 600)
    {
        int n = 0;
        while (!s.settled() && n < limit)
        {
            s.tick(1.0f / 60.0f, false, false, false);
            ++n;
        }
        return n;
    }
}

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;      // the atlas bakes through JUCE's font stack
    T::Probe P("fg.ruleslider.input", "", argc, argv);
    funkgui::FontService::get().atlas();
    P.eq("font.ok", funkgui::FontService::get().ok(), 1);
    P.near("wheel_notch", RuleSlider::kWheelNotch, 0.10, 1e-7);

    // ---- continuous drag: 240 px per track, up also increases, Shift 1200, Cmd 6000, re-anchored on a toggle -------
    {
        EventLog log;
        FakeHost host(log);
        FakePort p("p", log, 0.5f);
        Model m(Spec{}, &p);
        RuleSlider s(m, kGeom, 10);
        GestureController g(host);
        s.pointerDown(at(60, kY), g, host);
        expect(P, "cont.down_begins_drag", log, "b:p u1");
        s.pointerDrag(at(84, kY), g);
        expect(P, "cont.24px_is_0.1", log, "s:p=0.6");
        s.pointerDrag(at(84, kY - 24), g);
        expect(P, "cont.up_increases", log, "s:p=0.7");
        s.pointerDrag(at(84, kY - 24, shift()), g);
        const std::string reanchor = log.take();
        P.eq("cont.shift_toggle_no_jump", reanchor.empty() || std::fabs(p.value01() - 0.7f) < 1e-6f, 1);
        s.pointerDrag(at(96, kY - 24, shift()), g);
        P.near("cont.shift_12px_is_0.01", p.value01(), 0.71, 1e-5);
        s.pointerDrag(at(96, kY - 24, cmd()), g);
        s.pointerDrag(at(156, kY - 24, cmd()), g);
        P.near("cont.cmd_60px_is_0.01", p.value01(), 0.72, 1e-5);
        s.pointerDrag(at(96, kY - 24), g);          // modifiers released: re-anchored, then 240 px per track
        s.pointerDrag(at(600, kY - 24), g);
        P.near("cont.clamps_at_1", p.value01(), 1.0, 0.0);
        log.take();
        s.pointerUp(at(600, kY), g);
        expect(P, "cont.up_ends_drag", log, "e:p u0");
        P.eq("cont.discipline", p.clean(), 1);
    }

    // ---- soft notches stick within ±4 px (02 §8.1) ------------------------------------------------------------------
    {
        EventLog log;
        FakeHost host(log);
        FakePort p("p", log, 0.2f);
        Spec sp;
        sp.notches = { 0.25f, 0.75f };
        Model m(sp, &p);
        RuleSlider s(m, kGeom, 10);
        GestureController g(host);
        s.pointerDown(at(60, kY), g, host);
        s.pointerDrag(at(60 + 14.4f, kY), g);            // t 0.26: 1.1 px from the notch
        P.eq("soft.sticks", funkgui::ease::sameBits(p.value01(), 0.25f), 1);
        s.pointerDrag(at(60 + 21.6f, kY), g);            // t 0.29: 4.5 px away
        P.near("soft.released", p.value01(), 0.29, 1e-5);
        s.pointerUp(at(80, kY), g);
        P.eq("soft.discipline", p.clean(), 1);
    }

    // ---- stepped drag: p = clamp(240/(n-1), 24, 64), commit at 0.5p + 6, one gesture, detent values only ------------
    {
        EventLog log;
        FakeHost host(log);
        FakePort p("p", log, 0.0f);
        Model m(stepped(ratio3()), &p);
        RuleSlider s(m, kGeom, 10);
        GestureController g(host);
        s.pointerDown(at(60, kY), g, host);
        expect(P, "step.down_begins_drag", log, "b:p u1");
        s.pointerDrag(at(97, kY), g);
        expect(P, "step.37px_holds", log, "");
        s.pointerDrag(at(98, kY), g);
        expect(P, "step.38px_commits", log, "s:p=0.5");
        const Frame mid = drawOf(s);
        P.eq("step.ghost_while_dragging", mid.count(funkgui::tags::slotCaret), 2);
        s.pointerDrag(at(162, kY), g);
        expect(P, "step.next_at_pitch_64", log, "s:p=1");
        s.pointerDrag(at(362, kY), g);
        expect(P, "step.end_stop", log, "");
        s.pointerDrag(at(325, kY), g);
        expect(P, "step.end_travel_not_banked_37", log, "");
        s.pointerDrag(at(324, kY), g);
        expect(P, "step.end_travel_not_banked_38", log, "s:p=0.5");
        s.pointerUp(at(324, kY), g);
        expect(P, "step.up_ends_drag", log, "e:p u0");
        P.eq("step.no_ghost_after_up", drawOf(s).count(funkgui::tags::slotCaret), 1);
        P.eq("step.discipline", p.clean(), 1);

        // hysteresis: up at +38 from detent 0, back down only at +26 (12 px band)
        p.set(0.0f);
        s.pointerDown(at(60, kY), g, host);
        s.pointerDrag(at(98, kY), g);
        s.pointerDrag(at(87, kY), g);
        s.pointerDrag(at(86, kY), g);
        s.pointerUp(at(86, kY), g);
        expect(P, "step.hysteresis", log, "b:p u1 s:p=0.5 s:p=0 e:p u0");

        // the ghost follows the raw travel: 20 px of a 64 px pitch is 0.31 of a 37.3 px cell
        s.tick(1.0f, false, false, false);
        s.pointerDown(at(60, kY), g, host);
        s.pointerDrag(at(80, kY), g);
        const Frame ghost = drawOf(s);
        P.near("step.ghost_offset", ghost.centreX(funkgui::tags::slotCaret, 0) - ghost.centreX(funkgui::tags::slotCaret, 1),
               20.0 * (112.0 / 3.0) / 64.0, 1.0);
        s.pointerUp(at(80, kY), g);
        log.take();
    }
    {
        // the pitch clamps: n = 5 -> 60 (commit 36), n = 21 -> 24 (commit 18), n = 2 -> 64 (commit 38)
        struct Case { int n; float holds, commits; const char* key; };
        const Case cases[] = { { 5, 35.0f, 36.0f, "step.pitch_n5" }, { 21, 17.0f, 18.0f, "step.pitch_n21" },
                               { 2, 37.0f, 38.0f, "step.pitch_n2" } };
        for (const Case& c : cases)
        {
            EventLog log;
            FakeHost host(log);
            FakePort p("p", log, 0.0f);
            std::vector<Detent> d;
            for (int i = 0; i < c.n; ++i)
                d.push_back({ static_cast<float>(i) / static_cast<float>(c.n - 1), "", "" });
            Model m(stepped(d), &p);
            RuleSlider s(m, kGeom, 10);
            GestureController g(host);
            s.pointerDown(at(60, kY), g, host);
            s.pointerDrag(at(60 + c.holds, kY), g);
            const bool held = log.take() == "b:p u1";
            s.pointerDrag(at(60 + c.commits, kY), g);
            const bool committed = log.take() == "s:p=" + num(d[1].host01);
            s.pointerUp(at(60, kY), g);
            P.eq(c.key, held && committed, 1);
        }
    }

    // ---- detent-label click: arms on down, commits on up via writeDetent, 3 px slop, then a relative drag ----------
    {
        EventLog log;
        FakeHost host(log);
        FakePort p("p", log, 0.0f);
        Model m(stepped(ratio3()), &p);
        RuleSlider s(m, kGeom, 10);
        GestureController g(host);
        P.eq("label.drawn", s.detentLabelsDrawn(), 1);
        P.eq("label.at_10", s.detentLabelAt({ 133.0f, 150.0f }), 2);
        P.eq("label.none_on_track_row", s.detentLabelAt({ 133.0f, kY }), -1);
        P.eq("label.cursor_hand", static_cast<int>(s.cursorAt({ 133.0f, 150.0f })),
             static_cast<int>(funkgui::Cursor::pointingHand));
        P.eq("label.cursor_leftright", static_cast<int>(s.cursorAt({ 60.0f, kY })),
             static_cast<int>(funkgui::Cursor::leftRight));
        s.pointerDown(at(133, 150), g, host);
        expect(P, "label.down_arms_only", log, "");
        s.pointerUp(at(133, 150), g);
        expect(P, "label.click_taps", log, "b:p s:p=1 e:p");
        s.pointerDown(at(133, 150), g, host);
        s.pointerUp(at(133, 150), g);
        expect(P, "label.active_writes_nothing", log, "");
        s.pointerDown(at(58, 150), g, host);
        s.pointerDrag(at(60, 151), g);
        s.pointerUp(at(60, 151), g);
        expect(P, "label.within_slop_is_a_click", log, "b:p s:p=0 e:p");
        s.pointerDown(at(96, 150), g, host);
        s.pointerDrag(at(136, 150), g);
        s.pointerUp(at(136, 150), g);
        expect(P, "label.past_slop_is_a_relative_drag", log, "b:p u1 s:p=0.5 e:p u0");
        P.eq("label.discipline", p.clean(), 1);
    }

    // ---- wheel: continuous in track space (0.025, Shift 0.005, non-smooth x4, dx when dy is 0), one burst ----------
    {
        EventLog log;
        FakeHost host(log);
        FakePort p("p", log, 0.5f);
        Model m(Spec{}, &p);
        RuleSlider s(m, kGeom, 10);
        GestureController g(host);
        P.eq("wheel.cont_handled", s.wheel(wheelOf(0.25f), g, 1.0), 1);
        s.wheel(wheelOf(0.25f), g, 1.1);
        expect(P, "wheel.cont_one_burst", log, "b:p s:p=0.525 s:p=0.55");
        g.poll(1.59);
        expect(P, "wheel.cont_open_before_idle", log, "");
        g.poll(1.6);
        expect(P, "wheel.cont_closed_at_idle", log, "e:p");
        s.wheel(wheelOf(0.25f, false, shift()), g, 2.0);
        P.near("wheel.cont_shift", p.value01(), 0.555, 1e-6);
        s.wheel(wheelOf(0.1f, true), g, 2.1);
        P.near("wheel.cont_smooth_x1", p.value01(), 0.5575, 1e-6);
        s.wheel(wheelOf(0.25f, false, {}, 0.0f, true), g, 2.2);
        P.near("wheel.cont_reversed", p.value01(), 0.5325, 1e-6);
        s.wheel(wheelOf(0.0f, false, shift(), 0.25f), g, 2.3);
        P.near("wheel.cont_horizontal", p.value01(), 0.5375, 1e-6);
        g.closeAll();
        P.eq("wheel.cont_discipline", p.clean(), 1);
    }
    {
        // stepped: one detent per discrete event; smooth deltas accumulate to kWheelNotch; a new burst starts at 0
        EventLog log;
        FakeHost host(log);
        FakePort p("p", log, 0.0f);
        Model m(stepped(ratio3()), &p);
        RuleSlider s(m, kGeom, 10);
        GestureController g(host);
        s.wheel(wheelOf(0.01f), g, 1.0);
        expect(P, "wheel.step_one_per_event", log, "b:p s:p=0.5");
        s.wheel(wheelOf(0.04f, true), g, 1.1);
        s.wheel(wheelOf(0.04f, true), g, 1.2);
        expect(P, "wheel.step_smooth_accumulates", log, "");
        s.wheel(wheelOf(0.04f, true), g, 1.3);
        expect(P, "wheel.step_smooth_notch", log, "s:p=1");
        s.wheel(wheelOf(-0.01f), g, 1.4);
        expect(P, "wheel.step_down", log, "s:p=0.5");
        g.poll(2.0);
        expect(P, "wheel.step_burst_closed", log, "e:p");
        s.wheel(wheelOf(0.09f, true), g, 3.0);           // 0.02 was left over: a new burst forgets it
        expect(P, "wheel.step_new_burst_resets", log, "");
        s.wheel(wheelOf(std::numeric_limits<float>::quiet_NaN(), true), g, 3.1);
        s.wheel(wheelOf(std::numeric_limits<float>::infinity(), true), g, 3.2);
        s.wheel(wheelOf(-1.0e30f, true), g, 3.3);
        g.closeAll();
        expect(P, "wheel.step_non_finite_and_huge", log, "b:p s:p=0 e:p");
        P.eq("wheel.step_discipline", p.clean(), 1);
    }
    {
        // the view turns locked in the middle of a drag (a Mode switch): the drag ends, nothing more is written
        EventLog log;
        FakeHost host(log);
        FakePort p("p", log, 0.0f);
        Model m(stepped(ratio3()), &p);
        RuleSlider s(m, kGeom, 10);
        GestureController g(host);
        s.pointerDown(at(60, kY), g, host);
        s.pointerDrag(at(98, kY), g);
        m.spec.state = ValueState::locked;
        ++m.rev;
        s.pointerDrag(at(200, kY), g);
        s.pointerUp(at(200, kY), g);
        expect(P, "step.locked_mid_drag_ends_it", log, "b:p u1 s:p=0.5 e:p u0");
        s.pointerDown(at(60, kY), g, host);
        s.pointerDown(at(60, kY), g, host);             // a second down without an up
        expect(P, "step.locked_down_refused", log, "");
        P.eq("step.locked_mid_drag_discipline", p.clean(), 1);
    }

    // ---- keys (02 §8.9): continuous ±0.01 / 0.001 / 0.1, Home/End, Delete = Mode default; stepped one detent --------
    {
        EventLog log;
        FakeHost host(log);
        FakePort p("p", log, 0.5f);
        Model m(Spec{}, &p);
        RuleSlider s(m, kGeom, 10);
        GestureController g(host);
        P.eq("keys.handled", s.key(keyOf(Key::right), g), 1);
        expect(P, "keys.cont_right", log, "b:p s:p=0.51 e:p");
        s.key(keyOf(Key::up, shift()), g);
        P.near("keys.cont_shift_up", p.value01(), 0.511, 1e-6);
        s.key(keyOf(Key::pageDown), g);
        P.near("keys.cont_page_down", p.value01(), 0.411, 1e-6);
        log.take();
        s.key(keyOf(Key::home), g);
        s.key(keyOf(Key::end), g);
        s.key(keyOf(Key::del), g);
        expect(P, "keys.cont_home_end_delete", log, "b:p s:p=0 e:p b:p s:p=1 e:p b:p s:p=0.25 e:p");
        const bool passed = !s.key(keyOf(Key::escape), g) && !s.key(keyOf(Key::tab), g)
                         && !s.key(keyOf(Key::enter), g) && !s.key(keyOf(Key::space), g)
                         && !s.key(keyOf(Key::character), g);
        P.eq("keys.others_not_consumed", passed, 1);
        expect(P, "keys.others_write_nothing", log, "");
        P.eq("keys.cont_discipline", p.clean(), 1);
    }
    {
        EventLog log;
        FakeHost host(log);
        FakePort p("p", log, 0.0f);
        Spec sp = stepped(ratio3());
        sp.def = 0.0f;
        Model m(sp, &p);
        RuleSlider s(m, kGeom, 10);
        GestureController g(host);
        s.key(keyOf(Key::right), g);
        s.key(keyOf(Key::up), g);
        s.key(keyOf(Key::right), g);
        expect(P, "keys.step_arrows_one_detent", log, "b:p s:p=0.5 e:p b:p s:p=1 e:p");
        s.key(keyOf(Key::pageDown), g);
        s.key(keyOf(Key::home), g);
        s.key(keyOf(Key::end), g);
        s.key(keyOf(Key::backspace), g);
        expect(P, "keys.step_page_home_end_default", log, "b:p s:p=0.5 e:p b:p s:p=0 e:p b:p s:p=1 e:p b:p s:p=0 e:p");
        P.eq("keys.step_discipline", p.clean(), 1);
    }

    // ---- double-click writes the Mode default, inside the drag the second down opened ------------------------------
    {
        EventLog log;
        FakeHost host(log);
        FakePort p("p", log, 0.5f);
        Model m(Spec{}, &p);
        RuleSlider s(m, kGeom, 10);
        GestureController g(host);
        s.pointerDown(at(60, kY), g, host);
        s.pointerUp(at(60, kY), g);
        s.pointerDown(at(60, kY, {}, 2), g, host);
        s.doubleClick(g);
        s.pointerUp(at(60, kY, {}, 2), g);
        expect(P, "dbl.cont_default", log, "b:p u1 e:p u0 b:p u1 s:p=0.25 e:p u0");

        FakePort q("q", log, 0.0f);
        Model ms(stepped(ratio3()), &q);
        RuleSlider st(ms, kGeom, 11);
        st.pointerDown(at(133, 150), g, host);
        st.pointerUp(at(133, 150), g);
        st.pointerDown(at(133, 150, {}, 2), g, host);
        st.doubleClick(g);
        st.pointerUp(at(133, 150, {}, 2), g);
        expect(P, "dbl.step_label_then_default", log, "b:q s:q=1 e:q b:q u1 s:q=0.5 e:q u0");
        P.eq("dbl.discipline", p.clean() && q.clean(), 1);
    }

    // ---- locked, derived and n/a refuse every write; the popup still opens the host menu ---------------------------
    for (const ValueState st : { ValueState::locked, ValueState::derived, ValueState::na })
    {
        const std::string k = st == ValueState::locked ? "locked" : (st == ValueState::derived ? "derived" : "na");
        EventLog log;
        FakeHost host(log);
        FakePort p("p", log, 0.5f);
        Spec sp;
        sp.state = st;
        sp.reason = "FIXED BY THE CIRCUIT";
        Model m(sp, &p);
        RuleSlider s(m, kGeom, 10);
        GestureController g(host);
        s.pointerDown(at(60, kY), g, host);
        s.pointerDrag(at(160, kY), g);
        s.pointerUp(at(160, kY), g);
        s.doubleClick(g);
        const bool wheeled = s.wheel(wheelOf(1.0f), g, 1.0);
        bool keysConsumed = true;
        for (const Key key : { Key::up, Key::down, Key::home, Key::end, Key::pageUp, Key::del })
            keysConsumed = keysConsumed && s.key(keyOf(key), g);
        s.a11yAction(A11yAction::setValue, 0.9, g);
        s.a11yAction(A11yAction::increment, 0, g);
        s.a11yAction(A11yAction::decrement, 0, g);
        g.closeAll();
        expect(P, "refuse." + k + ".no_writes", log, "");
        P.eq("refuse." + k + ".wheel_not_consumed", wheeled, 0);
        P.eq("refuse." + k + ".keys_consumed", keysConsumed, 1);
        P.eq("refuse." + k + ".cursor_normal", static_cast<int>(s.cursorAt({ 60.0f, kY })),
             static_cast<int>(funkgui::Cursor::normal));
        s.pointerDown(popupAt(60, kY), g, host);
        s.a11yAction(A11yAction::showMenu, 0, g);
        expect(P, "refuse." + k + ".menu_still_works", log, "menu:p menu:p");
        char line[128];
        s.specLine(line, sizeof line);
        P.eq("refuse." + k + ".spec_line_reason", std::string(line) == "THRESHOLD   FIXED BY THE CIRCUIT", 1);
    }
    {
        // the popup on a live slot: the menu and nothing else; display-only slots write nothing
        EventLog log;
        FakeHost host(log);
        FakePort p("p", log, 0.5f);
        Model m(Spec{}, &p);
        RuleSlider s(m, kGeom, 10);
        GestureController g(host);
        s.pointerDown(popupAt(60, kY), g, host);
        s.pointerDrag(at(160, kY), g);
        s.pointerUp(at(160, kY), g);
        expect(P, "popup.menu_never_a_write", log, "menu:p");
        Model display(Spec{}, nullptr);
        RuleSlider d(display, kGeom, 11);
        d.pointerDown(at(60, kY), g, host);
        d.pointerDrag(at(160, kY), g);
        d.pointerUp(at(160, kY), g);
        d.wheel(wheelOf(1.0f), g, 1.0);
        d.key(keyOf(Key::up), g);
        d.doubleClick(g);
        d.a11yAction(A11yAction::setValue, 0.9, g);
        d.pointerDown(popupAt(60, kY), g, host);
        expect(P, "display_only.writes_nothing", log, "");
    }

    // ---- hybrid end cells: 18 px of travel past the edge enters the cell; keys, wheel and a11y cross one at a time --
    {
        EventLog log;
        FakeHost host(log);
        FakePort p("p", log, 0.145f);                    // t 0.05 in [0.1, 1.0]
        Model m(hybridSpec(), &p);
        RuleSlider s(m, kGeom, 10);
        GestureController g(host);
        s.pointerDown(at(100, kY), g, host);
        s.pointerDrag(at(88, kY), g);                     // t 0: the range's edge
        s.pointerDrag(at(71, kY), g);                     // 17 px past
        expect(P, "hybrid.drag_to_edge", log, "b:p u1 s:p=0.1");
        s.pointerDrag(at(70, kY), g);                     // 18 px past: the OFF cell
        expect(P, "hybrid.drag_enters_cell", log, "s:p=0");
        s.pointerDrag(at(81, kY), g);                     // 17 px back from the cell's own position
        expect(P, "hybrid.cell_holds", log, "");
        s.pointerDrag(at(82, kY), g);
        expect(P, "hybrid.drag_leaves_cell_at_edge", log, "s:p=0.1");
        s.pointerUp(at(82, kY), g);
        expect(P, "hybrid.drag_one_gesture", log, "e:p u0");

        p.set(0.0f);                                      // a drag that starts in the cell
        s.pointerDown(at(100, kY), g, host);
        s.pointerDrag(at(117, kY), g);
        s.pointerDrag(at(118, kY), g);
        s.pointerDrag(at(148, kY), g);                    // the range starts one 24 px pitch from the cell
        s.pointerUp(at(148, kY), g);
        expect(P, "hybrid.drag_from_cell", log, "b:p u1 s:p=0.1 s:p=0.19 e:p u0");

        p.set(0.1f);
        s.key(keyOf(Key::left), g);
        s.key(keyOf(Key::left), g);
        s.key(keyOf(Key::right), g);
        s.key(keyOf(Key::right), g);
        expect(P, "hybrid.keys_cross_one_cell", log, "b:p s:p=0 e:p b:p s:p=0.1 e:p b:p s:p=0.109 e:p");
        s.key(keyOf(Key::home), g);
        s.key(keyOf(Key::end), g);
        expect(P, "hybrid.home_is_the_outermost_cell", log, "b:p s:p=0 e:p b:p s:p=1 e:p");

        p.set(0.1f);
        s.wheel(wheelOf(-0.25f), g, 10.0);
        s.wheel(wheelOf(-0.25f), g, 10.1);
        s.wheel(wheelOf(0.25f), g, 10.2);
        g.poll(11.0);
        expect(P, "hybrid.wheel_one_notch_crosses", log, "b:p s:p=0 s:p=0.1 e:p");
        s.wheel(wheelOf(-0.05f, true), g, 12.0);
        expect(P, "hybrid.wheel_smooth_accumulates_at_edge", log, "");
        s.wheel(wheelOf(-0.05f, true), g, 12.1);
        g.poll(13.0);
        expect(P, "hybrid.wheel_smooth_crosses", log, "b:p s:p=0 e:p");

        p.set(0.1f);
        s.a11yAction(A11yAction::decrement, 0, g);
        A11yItem inCell;
        s.tick(0.0f, false, false, false);
        s.accessibility(inCell);
        s.a11yAction(A11yAction::increment, 0, g);
        s.a11yAction(A11yAction::setValue, 0.5, g);
        expect(P, "hybrid.a11y_crosses", log, "b:p s:p=0 e:p b:p s:p=0.1 e:p b:p s:p=0.55 e:p");
        P.eq("hybrid.a11y_cell_value", inCell.value == "Off" && inCell.v == 0.0, 1);
        P.eq("hybrid.a11y_help", inCell.help == "end steps: Off", 1);
        const Frame f = drawOf(s);
        P.eq("hybrid.cell_tick", f.count(funkgui::tags::detentTick), 1);
        P.eq("hybrid.cell_label_glyphs", f.count(funkgui::tags::detentLabel), 3);
        P.eq("hybrid.discipline", p.clean(), 1);
    }

    // ---- accessibility: roles, index space, read-only, disabled, descriptions ---------------------------------------
    {
        EventLog log;
        FakeHost host(log);
        FakePort p("p", log, 0.5f);
        Model m(stepped(ratio3()), &p);
        RuleSlider s(m, kGeom, 10);
        GestureController g(host);
        A11yItem it;
        s.accessibility(it);
        P.eq("a11y.step_role", static_cast<int>(it.role), static_cast<int>(funkgui::A11yRole::slider));
        P.eq("a11y.step_index_space", it.lo == 0.0 && it.hi == 2.0 && it.step == 1.0 && it.v == 1.0, 1);
        P.eq("a11y.step_value", it.value == "4 to 1", 1);
        P.eq("a11y.step_help", it.help == "3 steps: 2, 4, 10", 1);
        s.a11yAction(A11yAction::increment, 0, g);
        s.a11yAction(A11yAction::decrement, 0, g);
        s.a11yAction(A11yAction::setValue, 0.4, g);
        s.a11yAction(A11yAction::setValue, 1.6, g);
        s.a11yAction(A11yAction::press, 0, g);
        expect(P, "a11y.step_writes_detents", log, "b:p s:p=1 e:p b:p s:p=0.5 e:p b:p s:p=0 e:p b:p s:p=1 e:p");

        Spec cs;
        cs.label = "INPUT";
        cs.aka = "THRESHOLD";
        Model mc(cs, &p);
        RuleSlider c(mc, kGeom, 11);
        c.accessibility(it);
        P.eq("a11y.cont_track_space", it.lo == 0.0 && it.hi == 1.0 && std::fabs(it.step - 0.01) < 1e-9
                                      && std::fabs(it.v - 1.0) < 1e-9, 1);
        P.eq("a11y.renamed_description", it.description == "controls threshold" && it.title == "INPUT", 1);
        c.a11yAction(A11yAction::setValue, 0.25, g);
        expect(P, "a11y.cont_set_value", log, "b:p s:p=0.25 e:p");

        Spec ls;
        ls.state = ValueState::locked;
        ls.label = "ATTACK";
        ls.reason = "T4 CELL";
        Model ml(ls, &p);
        RuleSlider l(ml, kGeom, 12);
        l.accessibility(it);
        P.eq("a11y.locked", it.readOnly && !it.enabled && it.help == "T4 CELL" && it.value == "10 ms, fixed", 1);
        ls.state = ValueState::derived;
        Model md(ls, &p);
        RuleSlider d(md, kGeom, 13);
        d.accessibility(it);
        P.eq("a11y.derived", it.readOnly && it.enabled && it.role == funkgui::A11yRole::slider, 1);
        ls.state = ValueState::na;
        ls.label = "RANGE";
        Model mn(ls, &p);
        RuleSlider n(mn, kGeom, 14);
        n.accessibility(it);
        P.eq("a11y.na", it.role == funkgui::A11yRole::staticText && !it.enabled && it.title == "RANGE, not applicable",
             1);
        P.eq("a11y.id_and_bounds", it.id == 14 && it.bounds.x == 34.0f && it.bounds.w == 124.0f && it.bounds.h == 80.0f,
             1);
        l.a11yAction(A11yAction::setValue, 0.9, g);
        expect(P, "a11y.locked_set_value_refused", log, "");
    }

    // ---- the detent-label fit rule, 02 §8.3's worked cases (w = 112, kMicro) ---------------------------------------
    {
        const auto& atlas = funkgui::FontService::get().atlas();
        struct Case { const char* key; std::vector<Detent> d; int labels; };
        std::vector<Detent> drive;
        static const char* driveNames[21] = { "\xE2\x88\x92" "20", "\xE2\x88\x92" "19", "\xE2\x88\x92" "18",
            "\xE2\x88\x92" "17", "\xE2\x88\x92" "16", "\xE2\x88\x92" "15", "\xE2\x88\x92" "14", "\xE2\x88\x92" "13",
            "\xE2\x88\x92" "12", "\xE2\x88\x92" "11", "\xE2\x88\x92" "10", "\xE2\x88\x92" "9", "\xE2\x88\x92" "8",
            "\xE2\x88\x92" "7", "\xE2\x88\x92" "6", "\xE2\x88\x92" "5", "\xE2\x88\x92" "4", "\xE2\x88\x92" "3",
            "\xE2\x88\x92" "2", "\xE2\x88\x92" "1", "0" };
        for (int i = 0; i < 21; ++i)
            drive.push_back({ static_cast<float>(i) / 20.0f, driveNames[i], driveNames[i] });
        const Case cases[] = {
            { "fit.bus_g_ratio", labels({ "2", "4", "10" }), 1 },
            { "fit.fet_ratio", labels({ "4", "8", "12", "20", "ALL" }), 1 },
            { "fit.bus25_ratio", labels({ "1.5", "2", "3", "4", "6", "10", "\xE2\x88\x9E" }), 1 },
            { "fit.bus_g_attack", labels({ ".1", ".3", "1", "3", "10", "30" }), 1 },
            { "fit.bus25_attack", labels({ ".03", ".1", ".3", "1", "3", "10", "30" }), 1 },
            { "fit.bus_g_release", labels({ ".1", ".3", ".6", "1.2", "AUTO" }), 1 },
            { "fit.opto_ratio", labels({ "COMP", "LIMIT" }), 1 },
            { "fit.clean_voice", labels({ "OFF", "TUBE", "DIODE", "BRIGHT" }), 0 },
            { "fit.clean_detect", labels({ "PEAK", "RMS", "PK+RMS" }), 1 },
            { "fit.clean_stereo", labels({ "ST", "M/S", "MID", "SIDE", "M>S", "S>M" }), 0 },
            { "fit.mu67_schpf", labels({ "OFF", "50", "100", "200", "350" }), 1 },
            { "fit.mu67_drive", drive, 0 },
            { "fit.diode_release_s", labels({ ".1", ".4", ".8", "1.5", "A1", "A2" }), 1 },
            { "fit.diode_release_ms", labels({ "100", "400", "800", "1500", "A1", "A2" }), 0 },
            { "fit.bus25_link", labels({ "IND", "50", "60", "70", "80", "90", "100" }), 1 },
        };
        for (const Case& c : cases)
            P.eq(c.key, RuleSlider::detentLabelsFit(atlas, c.d.data(), static_cast<int>(c.d.size()), 112.0f),
                 c.labels);
        P.eq("fit.empty", RuleSlider::detentLabelsFit(atlas, nullptr, 0, 112.0f), 0);

        // what the slider draws: labels or ticks only; ticks omitted below 4 px cells
        EventLog log;
        FakePort p("p", log, 0.0f);
        Model voice(stepped(labels({ "OFF", "TUBE", "DIODE", "BRIGHT" }), "VOICE"), &p);
        RuleSlider sv(voice, kGeom, 10);
        const Frame fv = drawOf(sv);
        P.eq("draw.ticks_only_no_labels", fv.count(funkgui::tags::detentLabel), 0);
        P.eq("draw.ticks_only_ticks", fv.count(funkgui::tags::detentTick), 4);
        P.eq("draw.ticks_only_no_label_hit", sv.detentLabelAt({ 54.0f, 150.0f }), -1);
        Model dr(stepped(drive, "DRIVE"), &p);
        RuleSlider sd(dr, kGeom, 11);
        P.eq("draw.drive_21_ticks", drawOf(sd).count(funkgui::tags::detentTick), 21);
        std::vector<Detent> thirty;
        for (int i = 0; i < 30; ++i)
            thirty.push_back({ static_cast<float>(i) / 29.0f, "", "" });
        Model t30(stepped(thirty, "DENSE"), &p);
        RuleSlider s30(t30, kGeom, 12);
        P.eq("draw.no_ticks_below_4px", drawOf(s30).count(funkgui::tags::detentTick), 0);
        Model r3(stepped(ratio3()), &p);
        RuleSlider s3(r3, kGeom, 13);
        const Frame f3 = drawOf(s3, true);
        P.eq("draw.ratio_label_glyphs", f3.count(funkgui::tags::detentLabel), 4);
        P.eq("draw.ratio_ticks", f3.count(funkgui::tags::detentTick), 3);
        P.eq("draw.focus_ring_hairlines", f3.count(funkgui::tags::focusRing), 4);
    }

    // ---- states draw what 02 §8.1 says: dotted locked track + notch, hollow derived caret, nothing for n/a ----------
    {
        EventLog log;
        FakePort p("p", log, 0.5f);
        Spec sp;
        sp.state = ValueState::locked;
        Model ml(sp, &p);
        RuleSlider l(ml, kGeom, 10);
        const Frame fl = drawOf(l);
        P.eq("draw.locked_dots", fl.count(funkgui::tags::slotTrack), 38);
        P.eq("draw.locked_notch", fl.count(funkgui::tags::slotNotch), 1);
        P.eq("draw.locked_no_caret", fl.count(funkgui::tags::slotCaret), 0);
        sp.clamped = true;
        Model mlc(sp, &p);
        RuleSlider lc(mlc, kGeom, 11);
        P.eq("draw.locked_clamped_no_notch", drawOf(lc).count(funkgui::tags::slotNotch), 0);
        sp.state = ValueState::derived;
        sp.clamped = false;
        Model md(sp, &p);
        RuleSlider d(md, kGeom, 12);
        const Frame fd = drawOf(d);
        P.eq("draw.derived_hollow_caret", fd.count(funkgui::tags::slotCaret), 1);
        P.eq("draw.derived_hairline", fd.count(funkgui::tags::slotTrack), 1);
        sp.state = ValueState::na;
        Model mn(sp, &p);
        RuleSlider n(mn, kGeom, 13);
        const Frame fn = drawOf(n);
        P.eq("draw.na_no_track", fn.count(funkgui::tags::slotTrack) + fn.count(funkgui::tags::slotCaret), 0);
        P.eq("draw.na_dash", fn.count(funkgui::tags::slotValue), 1);
        P.eq("draw.na_ink16", fn.colour(funkgui::tags::slotValue) == packed(funkgui::Theme::graphite().ink16), 1);
    }

    // ---- the word: carved out of the slot; a tag moves to the detent line and needs room there ---------------------
    {
        EventLog log;
        FakeHost host(log);
        FakePort p("p", log, 0.0f), w("w", log, 0.0f);
        Spec sp = stepped(labels({ "OFF", "50", "100", "200", "350" }), "SC HPF");
        sp.tag = "+";
        Model m(sp, &p);
        RuleSlider s(m, kGeom, 10);
        P.eq("tag.no_word_labels", s.detentLabelsDrawn(), 1);
        Toggle t(w);
        funkgui::AttachedWord word(t, kGeom, "LISTEN", 11);
        s.setWord(&word);
        P.eq("tag.word_moves_tag_ticks_only", s.detentLabelsDrawn(), 0);
        P.eq("word.carved_out", s.contains({ 140.0f, 100.0f }), 0);
        P.eq("word.slot_still_hit", s.contains({ 60.0f, kY }), 1);
        t.visibleFlag = false;
        s.tick(0.0f, false, false, false);
        P.eq("tag.hidden_word_labels_back", s.detentLabelsDrawn(), 1);
        P.eq("word.hidden_not_carved", s.contains({ 140.0f, 100.0f }), 1);
    }
    {
        EventLog log;
        FakeHost host(log);
        FakePort w("w", log, 0.0f);
        Toggle t(w);
        GestureController g(host);
        funkgui::AttachedWord word(t, kGeom, "AUTO", 20);
        const funkgui::Rect h = word.hit();
        P.eq("word.hit", h.x == 118.0f && h.y == 95.0f && h.w == 38.0f && h.h == 18.0f, 1);
        word.pointerDown(at(140, 100), g);
        P.eq("word.armed_on_down", word.armed(), 1);
        expect(P, "word.down_writes_nothing", log, "");
        word.pointerUp(at(140, 100), g);
        expect(P, "word.commit_on_up_inside", log, "b:w s:w=1 e:w");
        word.pointerDown(at(140, 100), g);
        word.pointerDrag(at(60, 120));
        word.pointerDrag(at(140, 100));
        word.pointerUp(at(140, 100), g);
        expect(P, "word.drag_off_cancels", log, "");
        P.eq("word.key_return", word.key(keyOf(Key::enter), g), 1);
        word.key(keyOf(Key::space), g);
        expect(P, "word.keys_toggle", log, "b:w s:w=0 e:w b:w s:w=1 e:w");
        P.eq("word.key_other", word.key(keyOf(Key::up), g), 0);
        P.eq("word.a11y_other_id", word.a11yAction(99, A11yAction::press, g), 0);
        word.a11yAction(20, A11yAction::toggle, g);
        expect(P, "word.a11y_toggle", log, "b:w s:w=0 e:w");
        word.pointerDown(popupAt(140, 100), g);
        word.pointerUp(popupAt(140, 100), g);
        expect(P, "word.popup_menu", log, "menu:w");
        std::vector<A11yItem> items;
        word.accessibility(items);
        P.eq("word.a11y_toggle_button", items.size() == 1 && items[0].role == funkgui::A11yRole::toggleButton
                                        && items[0].checkable && !items[0].checked && items[0].enabled, 1);
        P.eq("word.cursor", static_cast<int>(word.cursorAt({ 140.0f, 100.0f })),
             static_cast<int>(funkgui::Cursor::pointingHand));

        t.enabledFlag = false;                            // locked: disabled with the reason
        word.pointerDown(at(140, 100), g);
        word.pointerUp(at(140, 100), g);
        word.key(keyOf(Key::enter), g);
        word.a11yAction(20, A11yAction::press, g);
        expect(P, "word.disabled_refuses", log, "");
        P.eq("word.disabled_reason", word.reason() != nullptr
                                     && std::strcmp(word.reason(), "AUTO MAKEUP IS FIXED IN THIS MODE") == 0, 1);
        items.clear();
        word.accessibility(items);
        P.eq("word.disabled_a11y", items.size() == 1 && !items[0].enabled
                                   && items[0].help == "AUTO MAKEUP IS FIXED IN THIS MODE", 1);
        P.eq("word.disabled_cursor", static_cast<int>(word.cursorAt({ 140.0f, 100.0f })),
             static_cast<int>(funkgui::Cursor::normal));

        t.enabledFlag = true;                             // n/a: hidden
        t.visibleFlag = false;
        items.clear();
        word.accessibility(items);
        word.pointerDown(at(140, 100), g);
        word.pointerUp(at(140, 100), g);
        const bool keyed = word.key(keyOf(Key::enter), g);
        expect(P, "word.hidden_refuses", log, "");
        P.eq("word.hidden_no_a11y", items.empty() && !keyed && !word.visible(), 1);
        P.eq("word.discipline", w.clean(), 1);

        funkgui::AttachedWord plain(static_cast<funkgui::ToggleModel&>(t), kGeom, "AUTO", 21);
        P.eq("word.plain_toggle_model_always_visible", plain.visible(), 1);
    }

    // ---- label flash (0.6 s, then ease back) and the Mode-switch landing (the caret eases instead of snapping) ------
    {
        EventLog log;
        FakePort p("p", log, 0.1f);
        Model m(Spec{}, &p);
        RuleSlider s(m, kGeom, 10);
        P.eq("settle.fresh_is_settled", s.settled(), 1);
        s.flashLabel(0.6f);
        P.eq("flash.not_settled", s.settled(), 0);
        s.tick(1.0f / 60.0f, false, false, false);
        P.eq("flash.label_ink100", drawOf(s).colour(funkgui::tags::slotLabel)
                                       == packed(funkgui::Theme::graphite().ink100), 1);
        const int frames = settleFrames(s);
        P.in("flash.frames", frames, 90, 110);

        p.set(0.9f);                                      // a big jump snaps (HR) ...
        P.eq("settle.new_view_pending", s.settled(), 0);
        s.tick(1.0f / 60.0f, false, false, false);
        P.near("landing.plain_jump_snaps", drawOf(s).centreX(funkgui::tags::slotCaret), 40.0 + 0.9 * 112.0, 1e-3);
        p.set(0.1f);                                      // ... but a Mode switch's move eases (02 §8.7)
        s.flashLabel(0.0f);
        s.tick(1.0f / 60.0f, false, false, false);
        const float x1 = drawOf(s).centreX(funkgui::tags::slotCaret);
        P.eq("landing.eases", x1 > 60.0f && x1 < 140.0f, 1);
        P.in("landing.frames", settleFrames(s), 20, 120);
        P.near("landing.arrives", drawOf(s).centreX(funkgui::tags::slotCaret), 40.0 + 0.1 * 112.0, 1e-3);
        P.eq("landing.no_flash_at_0", drawOf(s).colour(funkgui::tags::slotLabel)
                                          != packed(funkgui::Theme::graphite().ink100), 1);

        // hover: 90 ms in (snaps within 1e-3), 160 ms out
        int in = 0;
        s.tick(1.0f / 60.0f, true, false, false);
        for (in = 1; !s.settled() && in < 600; ++in)
            s.tick(1.0f / 60.0f, true, false, false);
        P.in("hover.in_frames", in, 30, 50);
    }

    // ---- spec lines ---------------------------------------------------------------------------------------------------
    {
        EventLog log;
        FakePort p("p", log, 0.0f);
        Model st(stepped(ratio3()), &p);
        RuleSlider s(st, kGeom, 10);
        P.eq("spec.stepped", spec(s) == "RATIO   STEPS 2 \xC2\xB7 4 \xC2\xB7 10   DRAG / WHEEL / ARROWS STEP   CLICK A STEP"
                                        "   DBL-CLICK RESET", 1);
        Spec rs;
        rs.label = "INPUT";
        rs.aka = "THRESHOLD";
        rs.reason = "DRIVES A FIXED \xE2\x88\x92" "12 DBFS THRESHOLD";
        Model mr(rs, &p);
        RuleSlider r(mr, kGeom, 11);
        P.eq("spec.renamed", spec(r) == "INPUT (THRESHOLD)   DRIVES A FIXED \xE2\x88\x92" "12 DBFS THRESHOLD   DRAG"
                                        "   SHIFT FINE   DBL-CLICK RESET", 1);
        Spec xs;
        xs.label = "MIX";
        xs.tag = "+";
        Model mx(xs, &p);
        RuleSlider x(mx, kGeom, 12);
        P.eq("spec.extension", spec(x) == "MIX   EXTENSION \xE2\x80\x94 NOT ON THE ORIGINAL UNIT \xE2\x80\x94 NEUTRAL AT "
                                          "DEFAULT   DRAG   SHIFT FINE   DBL-CLICK RESET", 1);
        P.eq("spec.truncates_on_codepoints", spec(x, 18) == "MIX   EXTENSION ", 1);
        Model mh(hybridSpec(), &p);
        RuleSlider h(mh, kGeom, 13);
        P.eq("spec.hybrid", spec(h) == "ATTACK   END STEPS OFF   DRAG   SHIFT FINE   DBL-CLICK RESET", 1);
    }

    return P.finish();
}

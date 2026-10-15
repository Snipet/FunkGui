// FUNKGUI_TEST name=fg.dial.input timeout=600 gpu=0
//
// fg.dial.input: the Dial (widgets/Dial.h; FSynth docs/design/02-funkgui-and-ui.md §4.1, §14.1; FC 02 §5.4, §8.1,
// §8.9) against a recording fake port and host, driven through the widget's own entry points: fg.ruleslider.input's
// case list re-run on a dial. Each case compares the exact event sequence the host would see (b:<id> beginGesture,
// s:<id>=<v> setValue01, e:<id> endGesture, u1/u0 setUnboundedDrag, menu:<id> host menu): continuous drags on the
// travel dx - dy (240 px per sweep, Shift 1200, Cmd 6000, re-anchored on a modifier toggle, soft notches sticking
// within 4 px of a sweep's 240, inclusive, whatever the modifiers); stepped drags (pitch clamp(240/(n-1), 24, 64),
// commit at 0.5·p + 6 px, one gesture, only detent values, an end stop, the ghost pointer); the wheel (one detent per
// discrete event, kWheelNotch accumulation, one burst); every key of FC 02 §8.9 and double-click (the Mode default);
// index-space a11y; hybrid end cells on both sides (18 px of travel past the edge, inclusive, keys, wheel and a11y
// crossing one cell at a time, a modifier toggle re-anchoring on the cell, the 20-degree cells past each end of the
// sweep, at most two a side); locked, derived and n/a refusing every write while the host menu still opens; the
// geometry of DialGeom; what each state draws (§14.1: the track arc, the value arc from 7:30 or 12:00 and, with the
// hover fill, held to the sweep while the pointer eases out of an end cell, the pointer and its tau, the notch,
// detent ticks and the 4 px rule, the dotted locked track and its notch, the hollow pointer in its LINKED box, nothing
// for n/a; the label, value and sub lines; compact dials); the cursor, valueBox and takesTypedKeys; the label flash and
// the Mode-switch landing; spec lines and accessibility items. Spec rows only. No JUCE: a HeadlessGuiScope bakes the
// atlas where there is JUCE, and the test also runs without it and as wasm32.

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Ease.h>
#include <funkgui/core/Flatten.h>
#include <funkgui/core/Theme.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/params/GestureController.h>
#include <funkgui/params/ParamPort.h>
#include <funkgui/test/Harness.h>
#include <funkgui/text/FontService.h>
#include <funkgui/widgets/Dial.h>
#include <funkgui/widgets/ValueModel.h>

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
using funkgui::Dial;
using funkgui::DialGeom;
using funkgui::DialSize;
using funkgui::GestureController;
using funkgui::Key;
using funkgui::KeyEvent;
using funkgui::Mods;
using funkgui::Point;
using funkgui::PointerEvent;
using funkgui::ValueState;
using funkgui::ValueView;
using funkgui::WheelEvent;
namespace tags = funkgui::tags;

namespace
{
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kStart = 0.75f * kPi;                 // the dial's own arithmetic: 135 degrees + t * 270
    constexpr float kSweep = 1.5f * kPi;

    float angleOf(float t) { return kStart + t * kSweep; }

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

    // A dial as a product describes it: the continuous track spans host01 [rangeLo, rangeHi]; end cells and detents
    // carry their own host01; the view is recomputed from the port on every call.
    struct Spec
    {
        ValueState state = ValueState::continuous;
        const char* label = "VALUE";
        const char* aka = nullptr;
        const char* tag = nullptr;
        const char* reason = nullptr;
        std::vector<Detent> detents, lo, hi;
        std::vector<float> notches;
        float rangeLo = 0.0f, rangeHi = 1.0f;
        float def = 0.25f;
        float fixedTrack = 0.4f;                          // locked / derived / n/a track
        bool  bipolar = false, clamped = false, live = false;
        bool  detentUnknown = false;                      // stepped: detent -1, track fixedTrack (the dial reads it)
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
            v.live = spec.live;
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
                v.detent = spec.detentUnknown ? -1 : best;
                v.track = spec.detentUnknown ? spec.fixedTrack
                                             : (static_cast<float>(best) + 0.5f) / static_cast<float>(v.nDetents);
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
        }

        funkgui::ParamPort* port() override { return port_; }
        float host01FromTrack(float t) const override { return spec.rangeLo + t * (spec.rangeHi - spec.rangeLo); }
        float defaultHost01() const override { return spec.def; }

        // The product's detent write, logged as w:<i> when asked, then the default tap.
        void writeDetent(int i, GestureController& g) override
        {
            if (detentLog != nullptr)
                detentLog->add("w:" + std::to_string(i));
            funkgui::ValueModel::writeDetent(i, g);
        }

        Spec      spec;
        uint64_t  rev = 1;
        EventLog* detentLog = nullptr;

    private:
        float trackOf(float h) const
        {
            const float t = (h - spec.rangeLo) / (spec.rangeHi - spec.rangeLo);
            return t <= 0.0f ? 0.0f : (t >= 1.0f ? 1.0f : t);
        }

        FakePort* port_;
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

    // The standard dial: x 40, top 100, primary. Hit {42, 100, 56, 76}, centre (70, 134), track r 15, ring r 20.
    constexpr DialGeom kGeom{ 40.0f, 100.0f, DialSize::primary };
    constexpr Point kCentre{ 70.0f, 134.0f };
    constexpr float kY = 120.0f;                          // a row inside the hit

    std::vector<Detent> lmh()
    {
        return { { 0.0f, "LO", "low" }, { 0.5f, "MID", "middle" }, { 1.0f, "HI", "high" } };
    }

    std::vector<Detent> evenly(int n)
    {
        std::vector<Detent> d;
        for (int i = 0; i < n; ++i)
            d.push_back({ n > 1 ? static_cast<float>(i) / static_cast<float>(n - 1) : 0.0f, "", "" });
        return d;
    }

    Spec stepped(std::vector<Detent> d, const char* label = "STEPS")
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
        s.label = "HYBRID";
        s.rangeLo = 0.1f;
        s.rangeHi = 1.0f;
        s.def = 0.5f;
        s.lo = { { 0.0f, "OFF", "Off" } };
        return s;
    }

    // One high cell: MAX at host 1, past the range 0 .. 0.9 (host 0.9 is t 1 exactly).
    Spec hybridHiSpec()
    {
        Spec s;
        s.label = "HYBRID";
        s.rangeLo = 0.0f;
        s.rangeHi = 0.9f;
        s.def = 0.5f;
        s.hi = { { 1.0f, "MAX", "Max" } };
        return s;
    }

    // One frame's primitives, drawn at dpi 1 unless asked.
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

        const funkgui::Prim* nth(funkgui::Tag t, int n = 0) const
        {
            for (const auto& p : prims)
                if (p.tag == t && n-- == 0)
                    return &p;
            return nullptr;
        }

        const funkgui::Prim* last(funkgui::Tag t) const
        {
            const funkgui::Prim* r = nullptr;
            for (const auto& p : prims)
                if (p.tag == t)
                    r = &p;
            return r;
        }

        uint32_t colour(funkgui::Tag t, int n = 0) const
        {
            const funkgui::Prim* p = nth(t, n);
            return p != nullptr ? p->c0 : 0;
        }
    };

    Frame drawOf(const Dial& d, bool ring = false, float dpi = 1.0f)
    {
        static funkgui::Canvas canvas(funkgui::FontService::get().atlas());
        funkgui::FrameInfo fi;
        fi.logicalW = 400;
        fi.logicalH = 300;
        fi.dpi = dpi;
        canvas.begin(fi);
        d.draw(canvas, funkgui::Theme::graphite(), ring);
        return Frame{ canvas.end().prims };
    }

    uint32_t packed(funkgui::Col c)
    {
        return static_cast<uint32_t>(c.r) | static_cast<uint32_t>(c.g) << 8 | static_cast<uint32_t>(c.b) << 16
             | static_cast<uint32_t>(c.a) << 24;
    }

    Point mid(const funkgui::Prim& p) { return { 0.5f * (p.x0 + p.x1), 0.5f * (p.y0 + p.y1) }; }
    Point endA(const funkgui::Prim& p) { const Point m = mid(p); return { m.x + p.d1[0], m.y + p.d1[1] }; }
    Point endB(const funkgui::Prim& p) { const Point m = mid(p); return { m.x + p.d1[2], m.y + p.d1[3] }; }
    float dist(Point a, Point b) { return std::hypot(a.x - b.x, a.y - b.y); }

    // The dial angle of p seen from the centre, in [90, 450) degrees (as radians): the bottom of the gap is the cut,
    // so a low end cell reads below 135 degrees and a high one above 405.
    float dialAngle(Point c, Point p)
    {
        float a = std::atan2(p.y - c.y, p.x - c.x);
        while (a < 0.5f * kPi)
            a += 2.0f * kPi;
        while (a >= 2.5f * kPi)
            a -= 2.0f * kPi;
        return a;
    }

    // The track position the pointer shows (its segment's angle), or NaN without a pointer.
    float pointerTrack(const Frame& f, Point c = kCentre)
    {
        const funkgui::Prim* p = f.last(tags::dialPointer);
        if (p == nullptr)
            return std::numeric_limits<float>::quiet_NaN();
        return (dialAngle(c, mid(*p)) - kStart) / kSweep;
    }

    // Whether some endpoint of a primitive with tag t lies within tol of q.
    bool hasEnd(const Frame& f, funkgui::Tag t, Point q, float tol = 1e-3f)
    {
        for (const auto& p : f.prims)
            if (p.tag == t && (dist(endA(p), q) <= tol || dist(endB(p), q) <= tol))
                return true;
        return false;
    }

    Point onCircle(Point c, float r, float a)
    {
        return { c.x + r * static_cast<float>(std::cos(static_cast<double>(a))),
                 c.y + r * static_cast<float>(std::sin(static_cast<double>(a))) };
    }

    // Whether every endpoint of every primitive with tag t lies on the sweep, 7:30 .. 4:30 (135 .. 405 degrees).
    bool onSweep(const Frame& f, funkgui::Tag t)
    {
        for (const auto& p : f.prims)
            if (p.tag == t)
                for (const Point q : { endA(p), endB(p) })
                {
                    const float a = dialAngle(kCentre, q);
                    if (a < kStart - 1e-4f || a > kStart + kSweep + 1e-4f)
                        return false;
                }
        return true;
    }

    // The hover fill's pieces: DIAL_TRACK segments 4 px wide (the track's own are 2 px).
    std::vector<funkgui::Prim> fillOf(const Frame& f)
    {
        std::vector<funkgui::Prim> out;
        for (const auto& p : f.prims)
            if (p.tag == tags::dialTrack && p.d2[0] == 2.0f)
                out.push_back(p);
        return out;
    }

    bool anyEndAt(const std::vector<funkgui::Prim>& v, Point q)
    {
        for (const auto& p : v)
            if (dist(endA(p), q) <= 1e-3f || dist(endB(p), q) <= 1e-3f)
                return true;
        return false;
    }

    // The angle a primitive's midpoint sits at, or -1 without one.
    float angleAt(const funkgui::Prim* p, Point c = kCentre) { return p != nullptr ? dialAngle(c, mid(*p)) : -1.0f; }

    std::string spec(const Dial& d, size_t n = 256)
    {
        std::vector<char> buf(n, 'x');
        d.specLine(buf.data(), n);
        return buf.data();
    }

    int settleFrames(Dial& d, int limit = 600)
    {
        int n = 0;
        while (!d.settled() && n < limit)
        {
            d.tick(1.0f / 60.0f, false, false, false);
            ++n;
        }
        return n;
    }

    bool sameRect(funkgui::Rect a, funkgui::Rect b)
    {
        return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
    }
}

int main(int argc, char** argv)
{
    const funkgui::HeadlessGuiScope gui;                 // JUCE's GUI side, when there is JUCE: the atlas bakes there
    T::Probe P("fg.dial.input", "", argc, argv);
    funkgui::FontService::get().atlas();
    P.eq("font.ok", funkgui::FontService::get().ok(), 1);

    // ---- DialGeom and the constants (FSynth 02 §4.1) ----------------------------------------------------------------
    {
        P.eq("geom.primary", kGeom.w() == 60.0f && kGeom.h() == 80.0f && kGeom.radius() == 15.0f
                             && kGeom.ringRadius() == 20.0f && kGeom.centre().x == 70.0f && kGeom.centre().y == 134.0f
                             && kGeom.labelTop() == 100.0f && kGeom.valueTop() == 156.0f && kGeom.subTop() == 168.0f,
             1);
        P.eq("geom.primary_hit", sameRect(kGeom.hit(), { 42.0f, 100.0f, 56.0f, 76.0f }), 1);
        constexpr DialGeom c{ 10.0f, 20.0f, DialSize::compact };
        P.eq("geom.compact", c.w() == 54.0f && c.h() == 52.0f && c.radius() == 11.0f && c.ringRadius() == 16.0f
                             && c.centre().x == 37.0f && c.centre().y == 40.0f && c.labelTop() == 58.0f, 1);
        P.eq("geom.compact_hit", sameRect(c.hit(), { 12.0f, 20.0f, 50.0f, 48.0f }), 1);
        P.eq("geom.constants", Dial::kSweepDeg == 270.0f && Dial::kEndCellDeg == 20.0f && Dial::kDragPx == 240.0f
                               && Dial::kFinePx == 1200.0f && Dial::kUltraPx == 6000.0f, 1);
    }

    // ---- continuous drag: dx - dy, 240 px per sweep, Shift 1200, Cmd 6000, re-anchored on a toggle -----------------
    {
        EventLog log;
        FakeHost host(log);
        FakePort p("p", log, 0.5f);
        Model m(Spec{}, &p);
        Dial d(m, kGeom, 10);
        GestureController g(host);
        d.pointerDown(at(60, kY), g, host);
        expect(P, "cont.down_begins_drag", log, "b:p u1");
        d.pointerDrag(at(84, kY), g);
        expect(P, "cont.right_24px_is_0.1", log, "s:p=0.6");
        d.pointerDrag(at(84, kY - 24), g);
        expect(P, "cont.up_24px_is_0.1", log, "s:p=0.7");
        d.pointerDrag(at(84, kY - 24, shift()), g);
        const std::string reanchor = log.take();
        P.eq("cont.shift_toggle_no_jump", reanchor.empty() || std::fabs(p.value01() - 0.7f) < 1e-6f, 1);
        d.pointerDrag(at(96, kY - 24, shift()), g);
        P.near("cont.shift_12px_is_0.01", p.value01(), 0.71, 1e-5);
        d.pointerDrag(at(96, kY - 24, cmd()), g);
        d.pointerDrag(at(96, kY - 84, cmd()), g);         // 60 px up
        P.near("cont.cmd_60px_is_0.01", p.value01(), 0.72, 1e-5);
        d.pointerDrag(at(96, kY - 84), g);                // modifiers released: re-anchored, then 240 px per sweep
        d.pointerDrag(at(96, kY - 132), g);               // 48 px up
        P.near("cont.released_48px_is_0.2", p.value01(), 0.92, 1e-5);
        d.pointerDrag(at(72, kY - 108), g);               // 24 left and 24 down: -48 px
        P.near("cont.left_and_down_decrease", p.value01(), 0.72, 1e-5);
        d.pointerDrag(at(600, kY), g);
        P.near("cont.clamps_at_1", p.value01(), 1.0, 0.0);
        log.take();
        d.pointerUp(at(600, kY), g);
        expect(P, "cont.up_ends_drag", log, "e:p u0");
        P.eq("cont.discipline", p.clean(), 1);
    }

    // ---- soft notches stick within 4 px of 240 ----------------------------------------------------------------------
    {
        EventLog log;
        FakeHost host(log);
        FakePort p("p", log, 0.2f);
        Spec sp;
        sp.notches = { 0.25f, 0.75f };
        Model m(sp, &p);
        Dial d(m, kGeom, 10);
        GestureController g(host);
        d.pointerDown(at(60, kY), g, host);
        d.pointerDrag(at(60 + 14.4f, kY), g);             // t 0.26: 2.4 px of travel from the notch
        P.eq("soft.sticks", funkgui::ease::sameBits(p.value01(), 0.25f), 1);
        d.pointerDrag(at(60 + 21.6f, kY), g);             // t 0.29: 9.6 px away
        P.near("soft.released", p.value01(), 0.29, 1e-5);
        d.pointerDrag(at(60 + 15.9f, kY), g);             // 3.9 px past the notch: inside the 4
        P.eq("soft.sticks_at_3.9px", funkgui::ease::sameBits(p.value01(), 0.25f), 1);
        d.pointerDrag(at(60 + 16.1f, kY), g);             // 4.1 px past it: outside
        P.near("soft.released_at_4.1px", p.value01(), 0.25 + 4.1 / 240.0, 1e-5);
        d.pointerDrag(at(60 + 8.1f, kY), g);              // 3.9 px before it
        P.eq("soft.sticks_at_3.9px_before", funkgui::ease::sameBits(p.value01(), 0.25f), 1);
        d.pointerDrag(at(60 + 7.9f, kY), g);              // 4.1 px before it
        P.near("soft.released_at_4.1px_before", p.value01(), 0.25 - 4.1 / 240.0, 1e-5);
        d.pointerUp(at(80, kY), g);
        P.eq("soft.discipline", p.clean(), 1);
        P.eq("soft.notch_marks", drawOf(d).count(tags::dialNotch), 3);   // the default and two soft notches
    }
    {
        // the 4 px radius is inclusive: from t 0 a notch at float(1/60) is 4 px of a 240 px sweep exactly
        EventLog log;
        FakeHost host(log);
        FakePort p("p", log, 0.0f);
        const float n = 1.0f / 60.0f;
        Spec sp;
        sp.notches = { n };
        Model m(sp, &p);
        Dial d(m, kGeom, 10);
        GestureController g(host);
        P.eq("soft.premise_exactly_4px", std::fabs(0.0f - n) * Dial::kDragPx == 4.0f, 1);
        d.pointerDown(at(60, kY), g, host);
        d.pointerDrag(at(50, kY), g);                     // t 0 (clamped)
        P.eq("soft.sticks_at_exactly_4px", funkgui::ease::sameBits(p.value01(), n), 1);
        d.pointerUp(at(50, kY), g);
        P.eq("soft.exact_discipline", p.clean(), 1);
    }
    {
        // whatever the modifiers, the 4 px are of a 240 px sweep: under Shift (1200 px) a notch holds 0.0167 of track
        // either side, 20 px of travel; under Cmd (6000 px) 100 px
        EventLog log;
        FakeHost host(log);
        FakePort p("p", log, 0.24f);
        Spec sp;
        sp.notches = { 0.25f };
        Model m(sp, &p);
        Dial d(m, kGeom, 10);
        GestureController g(host);
        d.pointerDown(at(60, kY, shift()), g, host);
        d.pointerDrag(at(84, kY, shift()), g);            // 24 px of 1200: t 0.26, 2.4 px of a 240 px sweep
        P.eq("soft.shift_sticks", funkgui::ease::sameBits(p.value01(), 0.25f), 1);
        d.pointerDrag(at(96, kY, shift()), g);            // 36 px: t 0.27, 4.8 px of 240
        P.near("soft.shift_leaves", p.value01(), 0.27, 1e-5);
        d.pointerUp(at(96, kY), g);
        p.set(0.24f);
        d.pointerDown(at(60, kY, cmd()), g, host);
        d.pointerDrag(at(180, kY, cmd()), g);             // 120 px of 6000: t 0.26
        P.eq("soft.cmd_sticks", funkgui::ease::sameBits(p.value01(), 0.25f), 1);
        d.pointerDrag(at(240, kY, cmd()), g);             // 180 px: t 0.27
        P.near("soft.cmd_leaves", p.value01(), 0.27, 1e-5);
        d.pointerUp(at(240, kY), g);
        P.eq("soft.modifiers_discipline", p.clean(), 1);
    }

    // ---- stepped drag: p = clamp(240/(n-1), 24, 64), commit at 0.5p + 6, one gesture, detent values only ------------
    {
        EventLog log;
        FakeHost host(log);
        FakePort p("p", log, 0.0f);
        Model m(stepped(lmh()), &p);
        Dial d(m, kGeom, 10);
        GestureController g(host);
        d.pointerDown(at(60, kY), g, host);
        expect(P, "step.down_begins_drag", log, "b:p u1");
        d.pointerDrag(at(97, kY), g);
        expect(P, "step.37px_holds", log, "");
        d.pointerDrag(at(98, kY), g);
        expect(P, "step.38px_commits", log, "s:p=0.5");
        P.eq("step.ghost_while_dragging", drawOf(d).count(tags::dialPointer), 2);
        d.pointerDrag(at(98, kY - 64), g);
        expect(P, "step.next_at_pitch_64_up", log, "s:p=1");
        d.pointerDrag(at(298, kY - 64), g);
        expect(P, "step.end_stop", log, "");
        d.pointerDrag(at(261, kY - 64), g);
        expect(P, "step.end_travel_not_banked_37", log, "");
        d.pointerDrag(at(260, kY - 64), g);
        expect(P, "step.end_travel_not_banked_38", log, "s:p=0.5");
        d.pointerUp(at(260, kY), g);
        expect(P, "step.up_ends_drag", log, "e:p u0");
        P.eq("step.no_ghost_after_up", drawOf(d).count(tags::dialPointer), 1);
        P.eq("step.discipline", p.clean(), 1);

        // hysteresis: up at +38 from detent 0, back down only at +26 (12 px band)
        p.set(0.0f);
        d.pointerDown(at(60, kY), g, host);
        d.pointerDrag(at(98, kY), g);
        d.pointerDrag(at(87, kY), g);
        d.pointerDrag(at(86, kY), g);
        d.pointerUp(at(86, kY), g);
        expect(P, "step.hysteresis", log, "b:p u1 s:p=0.5 s:p=0 e:p u0");

        // the ghost follows the raw travel: 20 px of a 64 px pitch is 0.3125 of a cell, a third of the sweep each
        d.tick(1.0f, false, false, false);
        d.pointerDown(at(60, kY), g, host);
        d.pointerDrag(at(80, kY), g);
        const Frame ghost = drawOf(d);
        const funkgui::Prim* gp = ghost.nth(tags::dialPointer, 0);
        const funkgui::Prim* pp = ghost.nth(tags::dialPointer, 1);
        const bool both = gp != nullptr && pp != nullptr;
        P.near("step.ghost_offset", both ? dialAngle(kCentre, mid(*gp)) - dialAngle(kCentre, mid(*pp)) : 0.0,
               (20.0 / 64.0) / 3.0 * 1.5 * 3.14159265358979, 1e-3);
        P.near("step.pointer_on_detent_0", both ? (dialAngle(kCentre, mid(*pp)) - kStart) / kSweep : -1.0, 1.0 / 6.0,
               1e-4);
        P.eq("step.ghost_is_1px_ink32", both && gp->d2[0] == 0.5f
                                         && gp->c0 == packed(funkgui::Theme::graphite().ink32), 1);
        d.pointerUp(at(80, kY), g);
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
            const std::vector<Detent> dd = evenly(c.n);
            Model m(stepped(dd), &p);
            Dial d(m, kGeom, 10);
            GestureController g(host);
            d.pointerDown(at(60, kY), g, host);
            d.pointerDrag(at(60, kY - c.holds), g);
            const bool held = log.take() == "b:p u1";
            d.pointerDrag(at(60, kY - c.commits), g);
            const bool committed = log.take() == "s:p=" + num(dd[1].host01);
            d.pointerUp(at(60, kY), g);
            P.eq(c.key, held && committed, 1);
        }
    }
    {
        // the ghost's offset scales with the detent count: on 5 detents (pitch 60) 20 px is a third of a cell, and a
        // cell is a fifth of the sweep
        EventLog log;
        FakeHost host(log);
        FakePort p("p", log, 0.0f);
        Model m(stepped(evenly(5)), &p);
        Dial d(m, kGeom, 10);
        GestureController g(host);
        d.pointerDown(at(60, kY), g, host);
        d.pointerDrag(at(80, kY), g);
        const Frame ghost = drawOf(d);
        const funkgui::Prim* gp = ghost.nth(tags::dialPointer, 0);
        const funkgui::Prim* pp = ghost.nth(tags::dialPointer, 1);
        P.near("step.ghost_offset_n5", gp != nullptr && pp != nullptr ? angleAt(gp) - angleAt(pp) : 0.0,
               (20.0 / 60.0) / 5.0 * 1.5 * 3.14159265358979, 1e-3);
        d.pointerUp(at(80, kY), g);
        expect(P, "step.ghost_n5_no_commit", log, "b:p u1 e:p u0");
    }

    // ---- wheel: continuous in track space (0.025, Shift 0.005, non-smooth x4, dx when dy is 0), one burst -----------
    {
        EventLog log;
        FakeHost host(log);
        FakePort p("p", log, 0.5f);
        Model m(Spec{}, &p);
        Dial d(m, kGeom, 10);
        GestureController g(host);
        P.eq("wheel.cont_handled", d.wheel(wheelOf(0.25f), g, 1.0), 1);
        d.wheel(wheelOf(0.25f), g, 1.1);
        expect(P, "wheel.cont_one_burst", log, "b:p s:p=0.525 s:p=0.55");
        g.poll(1.59);
        expect(P, "wheel.cont_open_before_idle", log, "");
        g.poll(1.6);
        expect(P, "wheel.cont_closed_at_idle", log, "e:p");
        d.wheel(wheelOf(0.25f, false, shift()), g, 2.0);
        P.near("wheel.cont_shift", p.value01(), 0.555, 1e-6);
        d.wheel(wheelOf(0.1f, true), g, 2.1);
        P.near("wheel.cont_smooth_x1", p.value01(), 0.5575, 1e-6);
        d.wheel(wheelOf(0.25f, false, {}, 0.0f, true), g, 2.2);
        P.near("wheel.cont_reversed", p.value01(), 0.5325, 1e-6);
        d.wheel(wheelOf(0.0f, false, shift(), 0.25f), g, 2.3);
        P.near("wheel.cont_horizontal", p.value01(), 0.5375, 1e-6);
        g.closeAll();
        P.eq("wheel.cont_discipline", p.clean(), 1);
    }
    {
        EventLog log;
        FakeHost host(log);
        FakePort p("p", log, 0.0f);
        Model m(stepped(lmh()), &p);
        Dial d(m, kGeom, 10);
        GestureController g(host);
        d.wheel(wheelOf(0.01f), g, 1.0);
        expect(P, "wheel.step_one_per_event", log, "b:p s:p=0.5");
        d.wheel(wheelOf(0.04f, true), g, 1.1);
        d.wheel(wheelOf(0.04f, true), g, 1.2);
        expect(P, "wheel.step_smooth_accumulates", log, "");
        d.wheel(wheelOf(0.04f, true), g, 1.3);
        expect(P, "wheel.step_smooth_notch", log, "s:p=1");
        d.wheel(wheelOf(-0.01f), g, 1.4);
        expect(P, "wheel.step_down", log, "s:p=0.5");
        g.poll(2.0);
        expect(P, "wheel.step_burst_closed", log, "e:p");
        d.wheel(wheelOf(0.09f, true), g, 3.0);           // 0.02 was left over: a new burst forgets it
        expect(P, "wheel.step_new_burst_resets", log, "");
        d.wheel(wheelOf(std::numeric_limits<float>::quiet_NaN(), true), g, 3.1);
        d.wheel(wheelOf(std::numeric_limits<float>::infinity(), true), g, 3.2);
        d.wheel(wheelOf(-1.0e30f, true), g, 3.3);
        g.closeAll();
        expect(P, "wheel.step_non_finite_and_huge", log, "b:p s:p=0 e:p");
        P.eq("wheel.step_discipline", p.clean(), 1);
    }
    {
        // the view turns locked in the middle of a drag (a Mode switch): the drag ends, nothing more is written
        EventLog log;
        FakeHost host(log);
        FakePort p("p", log, 0.0f);
        Model m(stepped(lmh()), &p);
        Dial d(m, kGeom, 10);
        GestureController g(host);
        d.pointerDown(at(60, kY), g, host);
        d.pointerDrag(at(98, kY), g);
        m.spec.state = ValueState::locked;
        ++m.rev;
        d.pointerDrag(at(200, kY), g);
        d.pointerUp(at(200, kY), g);
        expect(P, "step.locked_mid_drag_ends_it", log, "b:p u1 s:p=0.5 e:p u0");
        d.pointerDown(at(60, kY), g, host);
        d.pointerDown(at(60, kY), g, host);             // a second down without an up
        expect(P, "step.locked_down_refused", log, "");
        P.eq("step.locked_mid_drag_discipline", p.clean(), 1);
    }
    {
        // a drag closed elsewhere (closeAll) is not resumed by the next pointer event
        EventLog log;
        FakeHost host(log);
        FakePort p("p", log, 0.5f);
        Model m(Spec{}, &p);
        Dial d(m, kGeom, 10);
        GestureController g(host);
        d.pointerDown(at(60, kY), g, host);
        g.closeAll();
        d.pointerDrag(at(84, kY), g);
        d.pointerUp(at(84, kY), g);
        expect(P, "cont.closed_elsewhere", log, "b:p u1 e:p u0");
    }

    // ---- keys (FC 02 §8.9): continuous ±0.01 / 0.001 / 0.1, Home/End, Delete = Mode default; stepped one detent ------
    {
        EventLog log;
        FakeHost host(log);
        FakePort p("p", log, 0.5f);
        Model m(Spec{}, &p);
        Dial d(m, kGeom, 10);
        GestureController g(host);
        P.eq("keys.handled", d.key(keyOf(Key::right), g), 1);
        expect(P, "keys.cont_right", log, "b:p s:p=0.51 e:p");
        d.key(keyOf(Key::up, shift()), g);
        P.near("keys.cont_shift_up", p.value01(), 0.511, 1e-6);
        d.key(keyOf(Key::down), g);
        P.near("keys.cont_down", p.value01(), 0.501, 1e-6);
        d.key(keyOf(Key::left, shift()), g);
        P.near("keys.cont_shift_left", p.value01(), 0.5, 1e-6);
        d.key(keyOf(Key::pageUp), g);
        P.near("keys.cont_page_up", p.value01(), 0.6, 1e-6);
        d.key(keyOf(Key::pageDown), g);
        d.key(keyOf(Key::pageDown), g);
        P.near("keys.cont_page_down", p.value01(), 0.4, 1e-6);
        log.take();
        d.key(keyOf(Key::home), g);
        d.key(keyOf(Key::end), g);
        d.key(keyOf(Key::del), g);
        expect(P, "keys.cont_home_end_delete", log, "b:p s:p=0 e:p b:p s:p=1 e:p b:p s:p=0.25 e:p");
        d.key(keyOf(Key::end), g);
        d.key(keyOf(Key::backspace), g);
        expect(P, "keys.cont_backspace_default", log, "b:p s:p=1 e:p b:p s:p=0.25 e:p");
        d.key(keyOf(Key::del), g);
        expect(P, "keys.cont_unchanged_writes_nothing", log, "");
        const bool passed = !d.key(keyOf(Key::escape), g) && !d.key(keyOf(Key::tab), g)
                         && !d.key(keyOf(Key::enter), g) && !d.key(keyOf(Key::space), g)
                         && !d.key(keyOf(Key::character), g);
        P.eq("keys.others_not_consumed", passed, 1);
        expect(P, "keys.others_write_nothing", log, "");
        P.eq("keys.cont_discipline", p.clean(), 1);
    }
    {
        EventLog log;
        FakeHost host(log);
        FakePort p("p", log, 0.0f);
        Spec sp = stepped(lmh());
        sp.def = 0.0f;
        Model m(sp, &p);
        Dial d(m, kGeom, 10);
        GestureController g(host);
        d.key(keyOf(Key::right), g);
        d.key(keyOf(Key::up), g);
        d.key(keyOf(Key::right), g);
        expect(P, "keys.step_arrows_one_detent", log, "b:p s:p=0.5 e:p b:p s:p=1 e:p");
        d.key(keyOf(Key::pageDown), g);
        d.key(keyOf(Key::home), g);
        d.key(keyOf(Key::end), g);
        d.key(keyOf(Key::backspace), g);
        expect(P, "keys.step_page_home_end_default", log, "b:p s:p=0.5 e:p b:p s:p=0 e:p b:p s:p=1 e:p b:p s:p=0 e:p");
        d.key(keyOf(Key::left), g);
        d.key(keyOf(Key::down), g);
        d.key(keyOf(Key::pageUp), g);
        d.key(keyOf(Key::del), g);
        expect(P, "keys.step_left_down_page_up_delete", log, "b:p s:p=0.5 e:p b:p s:p=0 e:p");
        P.eq("keys.step_discipline", p.clean(), 1);
    }

    // ---- discrete detent writes go through the product's writeDetent, and never rewrite the current detent ---------
    {
        EventLog log;
        FakeHost host(log);
        FakePort p("p", log, 0.0f);
        Model m(stepped(lmh()), &p);
        m.detentLog = &log;
        Dial d(m, kGeom, 10);
        GestureController g(host);
        d.key(keyOf(Key::home), g);
        d.key(keyOf(Key::left), g);
        d.a11yAction(A11yAction::setValue, 0.0, g);
        d.wheel(wheelOf(-0.25f), g, 1.0);
        g.closeAll();
        expect(P, "detent.current_never_rewritten", log, "");
        d.key(keyOf(Key::end), g);
        d.a11yAction(A11yAction::decrement, 0, g);
        expect(P, "detent.product_write", log, "w:2 b:p s:p=1 e:p w:1 b:p s:p=0.5 e:p");
        d.pointerDown(at(60, kY), g, host);
        d.pointerDrag(at(60, kY + 38), g);
        d.pointerUp(at(60, kY + 38), g);
        expect(P, "detent.drag_writes_in_its_gesture", log, "b:p u1 s:p=0 e:p u0");
        d.wheel(wheelOf(0.25f), g, 5.0);
        g.closeAll();
        expect(P, "detent.wheel_writes_in_its_burst", log, "b:p s:p=0.5 e:p");
        P.eq("detent.discipline", p.clean(), 1);
    }

    // ---- double-click writes the Mode default, inside the drag the second down opened ------------------------------
    {
        EventLog log;
        FakeHost host(log);
        FakePort p("p", log, 0.5f);
        Model m(Spec{}, &p);
        Dial d(m, kGeom, 10);
        GestureController g(host);
        d.pointerDown(at(60, kY), g, host);
        d.pointerUp(at(60, kY), g);
        d.pointerDown(at(60, kY, {}, 2), g, host);
        d.doubleClick(g);
        d.pointerUp(at(60, kY, {}, 2), g);
        expect(P, "dbl.cont_default", log, "b:p u1 e:p u0 b:p u1 s:p=0.25 e:p u0");

        FakePort q("q", log, 1.0f);
        Model ms(stepped(lmh()), &q);
        Dial st(ms, kGeom, 11);
        st.doubleClick(g);
        expect(P, "dbl.step_default", log, "b:q s:q=0.5 e:q");
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
        sp.reason = "HELD BY THE FIXTURE";
        Model m(sp, &p);
        Dial d(m, kGeom, 10);
        GestureController g(host);
        d.pointerDown(at(60, kY), g, host);
        d.pointerDrag(at(160, kY), g);
        d.pointerUp(at(160, kY), g);
        d.doubleClick(g);
        const bool wheeled = d.wheel(wheelOf(1.0f), g, 1.0);
        bool keysConsumed = true;
        for (const Key key : { Key::up, Key::down, Key::left, Key::right, Key::home, Key::end, Key::pageUp,
                               Key::pageDown, Key::del, Key::backspace })
            keysConsumed = keysConsumed && d.key(keyOf(key), g);
        d.a11yAction(A11yAction::setValue, 0.9, g);
        d.a11yAction(A11yAction::increment, 0, g);
        d.a11yAction(A11yAction::decrement, 0, g);
        g.closeAll();
        expect(P, "refuse." + k + ".no_writes", log, "");
        P.eq("refuse." + k + ".wheel_not_consumed", wheeled, 0);
        P.eq("refuse." + k + ".keys_consumed", keysConsumed, 1);
        P.eq("refuse." + k + ".cursor_normal", static_cast<int>(d.cursorAt({ 60.0f, kY })),
             static_cast<int>(funkgui::Cursor::normal));
        P.eq("refuse." + k + ".no_typed_keys", d.takesTypedKeys(), 0);
        d.pointerDown(popupAt(60, kY), g, host);
        d.a11yAction(A11yAction::showMenu, 0, g);
        expect(P, "refuse." + k + ".menu_still_works", log, "menu:p menu:p");
        P.eq("refuse." + k + ".spec_line_reason", spec(d) == "VALUE   HELD BY THE FIXTURE", 1);
    }
    {
        // the popup on a live dial: the menu and nothing else; display-only dials write nothing
        EventLog log;
        FakeHost host(log);
        FakePort p("p", log, 0.5f);
        Model m(Spec{}, &p);
        Dial d(m, kGeom, 10);
        GestureController g(host);
        d.pointerDown(popupAt(60, kY), g, host);
        d.pointerDrag(at(160, kY), g);
        d.pointerUp(at(160, kY), g);
        expect(P, "popup.menu_never_a_write", log, "menu:p");
        Model display(Spec{}, nullptr);
        Dial dd(display, kGeom, 11);
        dd.pointerDown(at(60, kY), g, host);
        dd.pointerDrag(at(160, kY), g);
        dd.pointerUp(at(160, kY), g);
        dd.wheel(wheelOf(1.0f), g, 1.0);
        dd.key(keyOf(Key::up), g);
        dd.doubleClick(g);
        dd.a11yAction(A11yAction::setValue, 0.9, g);
        dd.pointerDown(popupAt(60, kY), g, host);
        expect(P, "display_only.writes_nothing", log, "");
        P.eq("display_only.cursor_normal", static_cast<int>(dd.cursorAt({ 60.0f, kY })),
             static_cast<int>(funkgui::Cursor::normal));
        P.eq("display_only.no_typed_keys", dd.takesTypedKeys(), 0);
    }

    // ---- hybrid end cells: 18 px of travel past the edge enters the cell; keys, wheel and a11y cross one at a time --
    {
        EventLog log;
        FakeHost host(log);
        FakePort p("p", log, 0.145f);                    // t 0.05 in [0.1, 1.0]
        Model m(hybridSpec(), &p);
        Dial d(m, kGeom, 10);
        GestureController g(host);
        d.pointerDown(at(100, kY), g, host);
        d.pointerDrag(at(88, kY), g);                     // t 0: the range's edge
        d.pointerDrag(at(71, kY), g);                     // 17 px past
        expect(P, "hybrid.drag_to_edge", log, "b:p u1 s:p=0.1");
        d.pointerDrag(at(70, kY), g);                     // 18 px past: the OFF cell
        expect(P, "hybrid.drag_enters_cell", log, "s:p=0");
        d.pointerDrag(at(81, kY), g);                     // 17 px back from the cell's own position
        expect(P, "hybrid.cell_holds", log, "");
        d.pointerDrag(at(82, kY), g);
        expect(P, "hybrid.drag_leaves_cell_at_edge", log, "s:p=0.1");
        d.pointerUp(at(82, kY), g);
        expect(P, "hybrid.drag_one_gesture", log, "e:p u0");

        p.set(0.0f);                                      // a drag that starts in the cell
        d.pointerDown(at(100, kY), g, host);
        d.pointerDrag(at(100, kY - 17), g);
        d.pointerDrag(at(100, kY - 18), g);
        d.pointerDrag(at(100, kY - 48), g);               // the range starts one 24 px pitch from the cell
        d.pointerUp(at(100, kY - 48), g);
        expect(P, "hybrid.drag_from_cell", log, "b:p u1 s:p=0.1 s:p=0.19 e:p u0");

        p.set(0.1f);
        d.key(keyOf(Key::left), g);
        d.key(keyOf(Key::left), g);
        d.key(keyOf(Key::right), g);
        d.key(keyOf(Key::right), g);
        expect(P, "hybrid.keys_cross_one_cell", log, "b:p s:p=0 e:p b:p s:p=0.1 e:p b:p s:p=0.109 e:p");
        d.key(keyOf(Key::home), g);
        d.key(keyOf(Key::end), g);
        expect(P, "hybrid.home_is_the_outermost_cell", log, "b:p s:p=0 e:p b:p s:p=1 e:p");

        p.set(0.1f);
        d.wheel(wheelOf(-0.25f), g, 10.0);
        d.wheel(wheelOf(-0.25f), g, 10.1);
        d.wheel(wheelOf(0.25f), g, 10.2);
        g.poll(11.0);
        expect(P, "hybrid.wheel_one_notch_crosses", log, "b:p s:p=0 s:p=0.1 e:p");
        d.wheel(wheelOf(-0.05f, true), g, 12.0);
        expect(P, "hybrid.wheel_smooth_accumulates_at_edge", log, "");
        d.wheel(wheelOf(-0.05f, true), g, 12.1);
        g.poll(13.0);
        expect(P, "hybrid.wheel_smooth_crosses", log, "b:p s:p=0 e:p");

        p.set(0.1f);
        d.a11yAction(A11yAction::decrement, 0, g);
        A11yItem inCell;
        d.tick(1.0f, false, false, false);
        d.accessibility(inCell);
        const Frame f = drawOf(d);
        for (int i = 0; i < 60; ++i)
            d.tick(1.0f / 60.0f, true, false, false);
        P.eq("hybrid.no_hover_fill_in_a_cell", drawOf(d).count(tags::dialTrack),
             funkgui::geom::arcSegments(15.0f, kStart, kStart + kSweep, 1.0f, funkgui::geom::kArcDevicePx));
        d.a11yAction(A11yAction::increment, 0, g);
        d.a11yAction(A11yAction::setValue, 0.5, g);
        expect(P, "hybrid.a11y_crosses", log, "b:p s:p=0 e:p b:p s:p=0.1 e:p b:p s:p=0.55 e:p");
        P.eq("hybrid.a11y_cell_value", inCell.value == "Off" && inCell.v == 0.0, 1);
        P.eq("hybrid.a11y_help", inCell.help == "end steps: Off", 1);
        P.eq("hybrid.cell_tick", f.count(tags::detentTick), 1);
        P.eq("hybrid.no_value_arc_in_a_cell", f.count(tags::dialArc), 0);
        // the low cell is centred 10 degrees before the sweep's start (20-degree cells), its tick ink100
        P.near("hybrid.low_cell_angle", pointerTrack(f) * kSweep + kStart, kStart - 10.0 * 3.14159265358979 / 180.0,
               1e-4);
        P.eq("hybrid.cell_tick_active_ink100", f.colour(tags::detentTick) == packed(funkgui::Theme::graphite().ink100),
             1);
        P.eq("hybrid.discipline", p.clean(), 1);
    }
    {
        // two cells a side: endLo[0] is the outermost low cell, endHi[0] the innermost high one
        EventLog log;
        FakeHost host(log);
        FakePort p("p", log, 1.0f);
        Spec sp;
        sp.label = "CELLS";
        sp.rangeLo = 0.2f;
        sp.rangeHi = 0.8f;
        sp.def = 0.5f;
        sp.lo = { { 0.0f, "L2", "low two" }, { 0.1f, "L1", "low one" } };
        sp.hi = { { 0.9f, "H1", "high one" }, { 1.0f, "H2", "high two" } };
        Model m(sp, &p);
        Dial d(m, kGeom, 10);
        const double deg = 3.14159265358979 / 180.0;
        const Frame top = drawOf(d);
        P.near("hybrid2.h2_outermost", pointerTrack(top) * kSweep + kStart, kStart + kSweep + 30.0 * deg, 1e-4);
        P.eq("hybrid2.four_cell_ticks", top.count(tags::detentTick), 4);
        p.set(0.9f);
        d.tick(1.0f, false, false, false);
        P.near("hybrid2.h1_innermost", pointerTrack(drawOf(d)) * kSweep + kStart, kStart + kSweep + 10.0 * deg, 1e-4);
        p.set(0.0f);
        d.tick(1.0f, false, false, false);
        P.near("hybrid2.l2_outermost", pointerTrack(drawOf(d)) * kSweep + kStart, kStart - 30.0 * deg, 1e-4);
        p.set(0.1f);
        d.tick(1.0f, false, false, false);
        P.near("hybrid2.l1_innermost", pointerTrack(drawOf(d)) * kSweep + kStart, kStart - 10.0 * deg, 1e-4);
        GestureController g(host);
        d.key(keyOf(Key::left), g);
        d.key(keyOf(Key::left), g);
        p.set(0.8f);
        d.key(keyOf(Key::right), g);
        d.key(keyOf(Key::right), g);
        d.key(keyOf(Key::right), g);
        expect(P, "hybrid2.keys_walk_the_cells", log, "b:p s:p=0 e:p b:p s:p=0.9 e:p b:p s:p=1 e:p");
        P.eq("hybrid2.spec", spec(d) == "CELLS   END STEPS L2 \xC2\xB7 L1 \xC2\xB7 H1 \xC2\xB7 H2   DRAG   SHIFT FINE   "
                                        "DBL-CLICK RESET", 1);
        // two cells a side is the most a Dial takes (Dial.h): the cell ticks sit at 105, 125, 415 and 435 degrees, and
        // the two outermost 20-degree cells leave 10 degrees clear at 6:00
        const Frame cells = drawOf(d);
        float tick[4] = { -1.0f, -1.0f, -1.0f, -1.0f };
        for (int i = 0; i < 4; ++i)
            tick[i] = angleAt(cells.nth(tags::detentTick, i));
        const float deg1 = static_cast<float>(deg);
        P.eq("hybrid2.max_two_a_side_ticks", std::fabs(tick[0] - 105.0f * deg1) < 1e-4f
                                             && std::fabs(tick[1] - 125.0f * deg1) < 1e-4f
                                             && std::fabs(tick[2] - 415.0f * deg1) < 1e-4f
                                             && std::fabs(tick[3] - 435.0f * deg1) < 1e-4f, 1);
        P.near("hybrid2.max_two_a_side_clear_at_6_00",
               (static_cast<double>(tick[0]) - 10.0 * deg + 2.0 * 3.14159265358979) - (tick[3] + 10.0 * deg),
               10.0 * deg, 1e-4);
    }
    {
        // the low edge's commit is inclusive: from t 0 exactly (host 0.1), 17 px of travel past it holds, 18 enters
        EventLog log;
        FakeHost host(log);
        FakePort p("p", log, 0.1f);
        Model m(hybridSpec(), &p);
        Dial d(m, kGeom, 10);
        GestureController g(host);
        d.pointerDown(at(100, kY), g, host);
        d.pointerDrag(at(83, kY), g);
        expect(P, "hybrid.low_edge_17px_holds", log, "b:p u1");
        d.pointerDrag(at(82, kY), g);
        expect(P, "hybrid.low_edge_18px_enters", log, "s:p=0");
        d.pointerUp(at(82, kY), g);
        expect(P, "hybrid.low_edge_one_gesture", log, "e:p u0");
    }
    {
        // a single high cell (MAX, host 1, past the range 0 .. 0.9): the low side's rules past 4:30
        EventLog log;
        FakeHost host(log);
        FakePort p("p", log, 0.9f);                      // t 1 exactly
        Model m(hybridHiSpec(), &p);
        Dial d(m, kGeom, 10);
        GestureController g(host);
        d.pointerDown(at(100, kY), g, host);
        d.pointerDrag(at(117, kY), g);                    // 17 px past the high edge
        expect(P, "hybrid_hi.drag_17px_holds", log, "b:p u1");
        d.pointerDrag(at(118, kY), g);                    // 18 px: the commit, inclusive
        expect(P, "hybrid_hi.drag_18px_enters", log, "s:p=1");
        d.pointerDrag(at(107, kY), g);                    // 17 px back from the cell's own position (edge + 24)
        expect(P, "hybrid_hi.drag_17px_back_holds", log, "");
        d.pointerDrag(at(106, kY), g);                    // 18 px back: the range's high edge
        expect(P, "hybrid_hi.drag_18px_back_leaves", log, "s:p=0.9");
        d.pointerUp(at(106, kY), g);
        expect(P, "hybrid_hi.drag_one_gesture", log, "e:p u0");

        // a Shift toggle inside the cell re-anchors on the cell: 1 px holds, then 18 px back leaves it
        p.set(1.0f);
        d.pointerDown(at(100, kY), g, host);
        d.pointerDrag(at(101, kY, shift()), g);
        expect(P, "hybrid_hi.shift_in_the_cell_holds", log, "b:p u1");
        d.pointerDrag(at(83, kY, shift()), g);            // 17 px back from the cell
        expect(P, "hybrid_hi.shift_17px_back_holds", log, "");
        d.pointerDrag(at(82, kY, shift()), g);            // 18 px back
        expect(P, "hybrid_hi.shift_18px_back_leaves", log, "s:p=0.9");
        d.pointerDrag(at(70, kY, shift()), g);            // 6 px under the edge at 1200 px per sweep
        P.near("hybrid_hi.shift_fine_in_the_range", p.value01(), 0.9 * (1.0 - 6.0 / 1200.0), 1e-6);
        d.pointerUp(at(70, kY), g);
        log.take();
        // and a Cmd toggle the same way
        p.set(1.0f);
        d.pointerDown(at(100, kY), g, host);
        d.pointerDrag(at(101, kY, cmd()), g);
        d.pointerDrag(at(83, kY, cmd()), g);
        expect(P, "hybrid_hi.cmd_in_the_cell_holds", log, "b:p u1");
        d.pointerDrag(at(82, kY, cmd()), g);
        expect(P, "hybrid_hi.cmd_18px_back_leaves", log, "s:p=0.9");
        d.pointerUp(at(82, kY), g);
        log.take();

        p.set(0.9f);
        d.key(keyOf(Key::up), g);
        d.key(keyOf(Key::up), g);                         // already the outermost cell: nothing
        d.key(keyOf(Key::down), g);
        expect(P, "hybrid_hi.keys_cross_the_high_edge", log, "b:p s:p=1 e:p b:p s:p=0.9 e:p");
        d.key(keyOf(Key::home), g);
        d.key(keyOf(Key::end), g);
        expect(P, "hybrid_hi.home_end", log, "b:p s:p=0 e:p b:p s:p=1 e:p");

        p.set(0.9f);
        d.wheel(wheelOf(0.25f), g, 20.0);                // one discrete notch at the high edge: into MAX
        d.wheel(wheelOf(0.25f), g, 20.1);                // the outermost cell: nothing
        d.wheel(wheelOf(-0.25f), g, 20.2);
        g.poll(21.0);
        expect(P, "hybrid_hi.wheel_one_notch_crosses", log, "b:p s:p=1 s:p=0.9 e:p");

        p.set(0.9f);
        d.a11yAction(A11yAction::increment, 0, g);
        A11yItem inCell;
        d.tick(1.0f, false, false, false);
        d.accessibility(inCell);
        const Frame f = drawOf(d);
        d.a11yAction(A11yAction::decrement, 0, g);
        expect(P, "hybrid_hi.a11y_crosses", log, "b:p s:p=1 e:p b:p s:p=0.9 e:p");
        P.eq("hybrid_hi.a11y_v_1_in_the_cell", inCell.v == 1.0 && inCell.help == "end steps: Max", 1);
        P.near("hybrid_hi.cell_angle", pointerTrack(f) * kSweep + kStart,
               kStart + kSweep + 10.0 * 3.14159265358979 / 180.0, 1e-4);
        P.eq("hybrid_hi.no_value_arc_in_the_cell", f.count(tags::dialArc), 0);
        P.eq("hybrid_hi.discipline", p.clean(), 1);
    }
    {
        // leaving an end cell, the pointer eases back across the gap; the hover fill and the value arc stay on the
        // sweep (FSynth 02 §14.1: the value arc runs from the sweep start to the value)
        EventLog log;
        FakeHost host(log);
        FakePort p("p", log, 0.0f);                       // OFF
        Model m(hybridSpec(), &p);
        Dial d(m, kGeom, 10);
        GestureController g(host);
        for (int i = 0; i < 60; ++i)
            d.tick(1.0f / 60.0f, true, false, false);
        d.key(keyOf(Key::right), g);                      // to the range's low edge, t 0
        d.tick(1.0f / 60.0f, true, false, false);
        const Frame lo = drawOf(d);
        P.eq("hybrid.leaving_low_cell_pointer_in_the_gap", pointerTrack(lo) < 0.0f, 1);
        P.eq("hybrid.leaving_low_cell_arcs_on_the_sweep", onSweep(lo, tags::dialArc) && onSweep(lo, tags::dialTrack),
             1);
        d.a11yAction(A11yAction::decrement, 0, g);        // back in OFF, then into the range at t 0.02
        settleFrames(d);
        d.a11yAction(A11yAction::setValue, 0.02, g);
        d.tick(1.0f / 60.0f, true, false, false);
        const Frame lo2 = drawOf(d);
        P.eq("hybrid.leaving_low_cell_into_the_range", pointerTrack(lo2) < 0.0f && onSweep(lo2, tags::dialArc)
                                                       && onSweep(lo2, tags::dialTrack), 1);
        settleFrames(d);
        const Frame lo3 = drawOf(d);
        P.eq("hybrid.arrived_arc_from_7_30", hasEnd(lo3, tags::dialArc, onCircle(kCentre, 15.0f, kStart))
                                             && hasEnd(lo3, tags::dialArc, onCircle(kCentre, 15.0f, angleOf(0.02f))),
             1);

        FakePort q("q", log, 1.0f);                       // MAX
        Model mh(hybridHiSpec(), &q);
        Dial h(mh, kGeom, 11);
        for (int i = 0; i < 60; ++i)
            h.tick(1.0f / 60.0f, true, false, false);
        h.key(keyOf(Key::left), g);                       // to the range's high edge, t 1
        h.tick(1.0f / 60.0f, true, false, false);
        const Frame hi = drawOf(h);
        P.eq("hybrid.leaving_high_cell_pointer_in_the_gap", pointerTrack(hi) > 1.0f, 1);
        P.eq("hybrid.leaving_high_cell_arcs_on_the_sweep", onSweep(hi, tags::dialArc) && onSweep(hi, tags::dialTrack),
             1);
        P.eq("hybrid.leaving_high_cell_arc_to_4_30",
             hasEnd(hi, tags::dialArc, onCircle(kCentre, 15.0f, kStart + kSweep)), 1);
        log.take();
    }

    // ---- accessibility: roles, index space, read-only, disabled, descriptions ---------------------------------------
    {
        EventLog log;
        FakeHost host(log);
        FakePort p("p", log, 0.5f);
        Model m(stepped(lmh()), &p);
        Dial d(m, kGeom, 10);
        GestureController g(host);
        A11yItem it;
        d.accessibility(it);
        P.eq("a11y.step_role", static_cast<int>(it.role), static_cast<int>(funkgui::A11yRole::slider));
        P.eq("a11y.step_index_space", it.lo == 0.0 && it.hi == 2.0 && it.step == 1.0 && it.v == 1.0, 1);
        P.eq("a11y.step_value", it.value == "middle", 1);
        P.eq("a11y.step_help", it.help == "3 steps: LO, MID, HI", 1);
        P.eq("a11y.no_modulation_description", it.description.empty(), 1);
        d.a11yAction(A11yAction::increment, 0, g);
        d.a11yAction(A11yAction::decrement, 0, g);
        d.a11yAction(A11yAction::setValue, 0.4, g);
        d.a11yAction(A11yAction::setValue, 1.6, g);
        d.a11yAction(A11yAction::press, 0, g);
        d.a11yAction(A11yAction::setValue, std::numeric_limits<double>::quiet_NaN(), g);
        expect(P, "a11y.step_writes_detents", log, "b:p s:p=1 e:p b:p s:p=0.5 e:p b:p s:p=0 e:p b:p s:p=1 e:p");

        Spec cs;
        cs.label = "OUTER";
        cs.aka = "INNER";
        Model mc(cs, &p);
        Dial c(mc, kGeom, 11);
        c.accessibility(it);
        P.eq("a11y.cont_track_space", it.lo == 0.0 && it.hi == 1.0 && std::fabs(it.step - 0.01) < 1e-9
                                      && std::fabs(it.v - 1.0) < 1e-9 && it.value == "1.00 db", 1);
        P.eq("a11y.renamed_description", it.description == "controls inner" && it.title == "OUTER", 1);
        c.a11yAction(A11yAction::setValue, 0.25, g);
        c.a11yAction(A11yAction::increment, 0, g);
        expect(P, "a11y.cont_set_value_and_increment", log, "b:p s:p=0.25 e:p b:p s:p=0.26 e:p");

        Spec ls;
        ls.state = ValueState::locked;
        ls.label = "LOCKED";
        ls.reason = "HELD BY THE FIXTURE";
        Model ml(ls, &p);
        Dial l(ml, kGeom, 12);
        l.accessibility(it);
        P.eq("a11y.locked", it.readOnly && !it.enabled && it.help == "HELD BY THE FIXTURE"
                            && it.value == "10 ms, fixed", 1);
        ls.state = ValueState::derived;
        Model md(ls, &p);
        Dial dv(md, kGeom, 13);
        dv.accessibility(it);
        P.eq("a11y.derived", it.readOnly && it.enabled && it.role == funkgui::A11yRole::slider, 1);
        ls.state = ValueState::na;
        ls.label = "UNUSED";
        Model mn(ls, &p);
        Dial n(mn, kGeom, 14);
        n.accessibility(it);
        P.eq("a11y.na", it.role == funkgui::A11yRole::staticText && !it.enabled && it.title == "UNUSED, not applicable",
             1);
        P.eq("a11y.id_and_bounds", it.id == 14 && sameRect(it.bounds, { 42.0f, 100.0f, 56.0f, 76.0f }), 1);
        l.a11yAction(A11yAction::setValue, 0.9, g);
        expect(P, "a11y.locked_set_value_refused", log, "");
        P.eq("a11y.discipline", p.clean(), 1);
    }

    // ---- what each state draws (FSynth 02 §14.1) ------------------------------------------------------------------
    const auto& th = funkgui::Theme::graphite();
    const float a0 = kStart, a1 = kStart + kSweep;
    const int trackSegs = funkgui::geom::arcSegments(15.0f, a0, a1, 1.0f, funkgui::geom::kArcDevicePx);
    {
        EventLog log;
        FakePort p("p", log, 0.5f);
        Model m(Spec{}, &p);
        Dial d(m, kGeom, 10);
        const Frame f = drawOf(d);
        P.eq("draw.cont_track_arc", f.count(tags::dialTrack), trackSegs);
        P.eq("draw.cont_track_ink16", f.colour(tags::dialTrack) == packed(th.ink16), 1);
        P.eq("draw.cont_value_arc", f.count(tags::dialArc),
             funkgui::geom::arcSegments(15.0f, a0, angleOf(0.5f), 1.0f, funkgui::geom::kArcDevicePx));
        P.eq("draw.cont_value_arc_ink70", f.colour(tags::dialArc) == packed(th.ink70), 1);
        P.eq("draw.cont_value_arc_from_7_30", hasEnd(f, tags::dialArc, onCircle(kCentre, 15.0f, a0)), 1);
        P.eq("draw.cont_value_arc_to_12_00", hasEnd(f, tags::dialArc, { 70.0f, 119.0f }), 1);
        P.eq("draw.cont_one_notch", f.count(tags::dialNotch), 1);
        const funkgui::Prim* notch = f.nth(tags::dialNotch);
        P.eq("draw.cont_notch_1x3_at_r18_on_the_default",
             notch != nullptr && notch->d2[0] == 0.5f
                 && std::fabs(dist(endA(*notch), kCentre) - 17.0f) < 1e-3f
                 && std::fabs(dist(endB(*notch), kCentre) - 19.0f) < 1e-3f
                 && std::fabs(dialAngle(kCentre, mid(*notch)) - angleOf(0.25f)) < 1e-4f
                 && notch->c0 == packed(th.ink32), 1);
        P.eq("draw.cont_pointer", f.count(tags::dialPointer), 1);
        const funkgui::Prim* ptr = f.nth(tags::dialPointer);
        P.eq("draw.cont_pointer_2px_r9_to_r17_at_12_00",
             ptr != nullptr && ptr->d2[0] == 1.0f && std::fabs(mid(*ptr).x - 70.0f) < 1e-4f
                 && std::fabs(endA(*ptr).y - 124.0f) < 1e-4f && std::fabs(endB(*ptr).y - 118.0f) < 1e-4f
                 && ptr->c0 == packed(th.ink100), 1);
        P.eq("draw.cont_no_band", f.count(tags::modBand) + f.count(tags::modDot), 0);
        P.eq("draw.cont_label_glyphs", f.count(tags::slotLabel), 5);
        P.eq("draw.cont_value_and_unit_glyphs", f.count(tags::slotValue), 6);   // "0.50" "DB"
        P.eq("draw.cont_no_sub", f.count(tags::slotSub), 0);
        P.eq("draw.cont_value_ink100", f.colour(tags::slotValue) == packed(th.ink100), 1);
        const funkgui::Prim* g0 = f.nth(tags::slotValue);
        const funkgui::Prim* g5 = f.nth(tags::slotValue, 5);
        P.near("draw.cont_value_line_centred",
               g0 != nullptr && g5 != nullptr ? 0.5 * (static_cast<double>(g0->x0) + g5->x1) : 0.0, 70.0, 1.5);
        P.near("draw.pointer_at_0_is_7_30", [&] {
            p.set(0.0f);
            d.tick(1.0f, false, false, false);
            return angleAt(drawOf(d).nth(tags::dialPointer));
        }(), a0, 1e-4);
        P.near("draw.pointer_at_1_is_4_30", [&] {
            p.set(1.0f);
            d.tick(1.0f, false, false, false);
            return angleAt(drawOf(d).nth(tags::dialPointer));
        }(), a1, 1e-4);
        P.eq("draw.focus_ring_hairlines", drawOf(d, true).count(tags::focusRing), 4);
        {
            // the ring sits on the box {42, 98, 56, 80} reduced by 1: top y 99, bottom y 177, left x 43, right x 97
            const Frame fr = drawOf(d, true);
            const auto edge = [&](int i) {
                const funkgui::Prim* q = fr.nth(tags::focusRing, i);
                return q != nullptr ? Point{ mid(*q).x - q->d0[2], mid(*q).y - q->d0[3] } : Point{ -1.0f, -1.0f };
            };
            P.eq("draw.focus_ring_on_the_box", edge(0).y == 99.0f && edge(1).y == 177.0f && edge(2).x == 43.0f
                                               && edge(3).x == 97.0f && edge(0).x == 43.0f, 1);
        }
        p.set(0.25f);
        d.tick(1.0f, false, false, false);
        P.eq("draw.at_default_value_ink32", drawOf(d).colour(tags::slotValue) == packed(th.ink32), 1);
        P.eq("draw.no_value_arc_at_0", [&] {
            p.set(0.0f);
            d.tick(1.0f, false, false, false);
            return drawOf(d).count(tags::dialArc);
        }(), 0);

        // hover: the accentDim fill under the track, the value arc and the value in accent
        p.set(0.5f);
        d.tick(1.0f, false, false, false);
        for (int i = 0; i < 120; ++i)
            d.tick(1.0f / 60.0f, true, false, false);
        const Frame hv = drawOf(d);
        const int fillSegs = funkgui::geom::arcSegments(15.0f, a0, angleOf(0.5f), 1.0f, funkgui::geom::kArcDevicePx);
        P.eq("draw.hover_fill_under_the_track", hv.count(tags::dialTrack), fillSegs + trackSegs);
        P.eq("draw.hover_fill_4px_accentdim", hv.nth(tags::dialTrack) != nullptr
                                              && hv.nth(tags::dialTrack)->d2[0] == 2.0f
                                              && hv.colour(tags::dialTrack) == packed(th.accentDim), 1);
        P.eq("draw.hover_value_arc_accent", hv.colour(tags::dialArc) == packed(th.accent), 1);
        P.eq("draw.hover_value_accent", hv.colour(tags::slotValue) == packed(th.accent), 1);
        P.eq("draw.hover_label_ink70", hv.colour(tags::slotLabel) == packed(th.ink70), 1);
        d.tick(1.0f / 60.0f, false, false, true);
        P.eq("draw.always_chrome_track_ink32", drawOf(d).colour(tags::dialTrack, fillSegs) == packed(th.ink32), 1);
    }
    {
        // bipolar: the value arc and the fill run from 12:00
        EventLog log;
        FakePort p("p", log, 0.25f);
        Spec sp;
        sp.bipolar = true;
        sp.def = 0.5f;
        Model m(sp, &p);
        Dial d(m, kGeom, 10);
        const Frame f = drawOf(d);
        P.eq("draw.bipolar_arc_from_12_00", hasEnd(f, tags::dialArc, { 70.0f, 119.0f }), 1);
        P.eq("draw.bipolar_arc_to_value", hasEnd(f, tags::dialArc, onCircle(kCentre, 15.0f, angleOf(0.25f))), 1);
        P.eq("draw.bipolar_arc_not_from_7_30", hasEnd(f, tags::dialArc, onCircle(kCentre, 15.0f, a0)), 0);
        P.eq("draw.bipolar_arc_count", f.count(tags::dialArc),
             funkgui::geom::arcSegments(15.0f, angleOf(0.25f), angleOf(0.5f), 1.0f, funkgui::geom::kArcDevicePx));
        p.set(0.5f);
        d.tick(1.0f, false, false, false);
        P.eq("draw.bipolar_no_arc_at_12_00", drawOf(d).count(tags::dialArc), 0);
    }
    {
        // stepped: ticks at the cell centres while a cell is >= 4 px of arc, none below; no value arc
        EventLog log;
        FakePort p("p", log, 0.5f);
        Model m3(stepped(lmh()), &p);
        Dial d3(m3, kGeom, 10);
        const Frame f3 = drawOf(d3);
        P.eq("draw.step3_ticks", f3.count(tags::detentTick), 3);
        P.eq("draw.step3_no_value_arc", f3.count(tags::dialArc), 0);
        P.eq("draw.step3_no_notch", f3.count(tags::dialNotch), 0);
        P.eq("draw.step3_active_tick_ink100", f3.colour(tags::detentTick, 1) == packed(th.ink100)
                                              && f3.colour(tags::detentTick, 0) == packed(th.ink32), 1);
        P.near("draw.step3_pointer_on_detent_1", pointerTrack(f3), 0.5, 1e-4);
        const funkgui::Prim* t0 = f3.nth(tags::detentTick);
        P.eq("draw.step3_tick_1x5_at_r18_cell_centre",
             t0 != nullptr && std::fabs(dist(endA(*t0), kCentre) - 16.0f) < 1e-3f
                 && std::fabs(dist(endB(*t0), kCentre) - 20.0f) < 1e-3f
                 && std::fabs(dialAngle(kCentre, mid(*t0)) - angleOf(1.0f / 6.0f)) < 1e-4f, 1);
        P.eq("draw.step3_value_glyphs", f3.count(tags::slotValue), 3);
        Model m17(stepped(evenly(17)), &p);
        Dial d17(m17, kGeom, 11);
        P.eq("draw.step17_ticks", drawOf(d17).count(tags::detentTick), 17);   // 15 x 270 deg / 17 = 4.16 px
        Model m18(stepped(evenly(18)), &p);
        Dial d18(m18, kGeom, 12);
        P.eq("draw.step18_no_ticks", drawOf(d18).count(tags::detentTick), 0);   // 3.93 px
        Model m21(stepped(evenly(21)), &p);
        Dial d21(m21, kGeom, 13);
        P.eq("draw.step21_no_ticks", drawOf(d21).count(tags::detentTick), 0);
        // the pointer eases onto a new detent with tau 60 ms (not a snap): one 1/60 s tick is ease::toward's step
        p.set(1.0f);
        d3.tick(1.0f / 60.0f, false, false, false);
        const Frame f1 = drawOf(d3);
        const float moving = pointerTrack(f1);
        P.eq("draw.step_pointer_eases", moving > 0.5f && moving < 5.0f / 6.0f, 1);
        P.near("draw.step_pointer_tau_60ms", angleAt(f1.last(tags::dialPointer)),
               funkgui::ease::toward(angleOf(0.5f), angleOf(5.0f / 6.0f), 1.0f / 60.0f, 0.06f), 1e-5);
        // from there the snap (1e-3 rad) lands it 22 frames later at tau 60 ms (35 at 90 ms)
        P.in("draw.step_pointer_settles", settleFrames(d3), 21, 23);
        P.near("draw.step_pointer_arrives", pointerTrack(drawOf(d3)), 5.0 / 6.0, 1e-4);
    }
    {
        // a stepped view that names no detent (detent -1): the active one is read off the track, floor(t n)
        EventLog log;
        FakePort p("p", log, 0.0f);
        Spec sp = stepped(lmh());
        sp.detentUnknown = true;
        sp.fixedTrack = 0.84f;
        Model m(sp, &p);
        Dial d(m, kGeom, 10);
        A11yItem it;
        d.accessibility(it);
        const Frame f = drawOf(d);
        P.eq("draw.step_active_from_track", it.v == 2.0 && f.colour(tags::detentTick, 2) == packed(th.ink100)
                                            && f.colour(tags::detentTick, 0) == packed(th.ink32), 1);
        P.near("draw.step_active_from_track_pointer", pointerTrack(f), 5.0 / 6.0, 1e-4);
        m.spec.fixedTrack = 1.0f;                         // t 1 is the last detent, not one past it
        ++m.rev;
        d.tick(1.0f, false, false, false);
        d.accessibility(it);
        P.eq("draw.step_active_from_track_at_1", it.v == 2.0, 1);
    }
    {
        // bipolar is a continuous view's: a stepped view with the flag set fills from 7:30 to its active detent
        EventLog log;
        FakePort p("p", log, 0.0f);                       // LO: the fill runs 7:30 .. 9:00, nowhere near 12:00
        Spec sp = stepped(lmh());
        sp.bipolar = true;
        Model m(sp, &p);
        Dial d(m, kGeom, 10);
        for (int i = 0; i < 120; ++i)
            d.tick(1.0f / 60.0f, true, false, false);
        const std::vector<funkgui::Prim> fill = fillOf(drawOf(d));
        P.eq("draw.step_bipolar_fill_from_7_30", !fill.empty() && anyEndAt(fill, onCircle(kCentre, 15.0f, kStart))
                                                 && anyEndAt(fill, onCircle(kCentre, 15.0f, angleOf(1.0f / 6.0f))), 1);
        P.eq("draw.step_bipolar_fill_not_from_12_00", anyEndAt(fill, { 70.0f, 119.0f }), 0);
    }
    {
        // a drag is under the hand: the hover chrome rises with the pointer away from the dial (hovered false)
        EventLog log;
        FakeHost host(log);
        FakePort p("p", log, 0.5f);
        Model m(Spec{}, &p);
        Dial d(m, kGeom, 10);
        GestureController g(host);
        d.pointerDown(at(60, kY), g, host);
        for (int i = 0; i < 60; ++i)
            d.tick(1.0f / 60.0f, false, false, false);
        const Frame f = drawOf(d);
        P.eq("hover.drag_is_under_the_hand", d.settled() && f.colour(tags::slotValue) == packed(th.accent)
                                             && f.colour(tags::dialArc) == packed(th.accent) && !fillOf(f).empty(), 1);
        d.pointerUp(at(60, kY), g);
        d.tick(1.0f / 60.0f, false, false, false);        // the next frame sees the hand gone
        settleFrames(d);
        P.eq("hover.drag_released", drawOf(d).colour(tags::slotValue) == packed(th.ink100), 1);
    }
    {
        // locked: a dotted track (a 1.5 px dot every 3 px of arc) and a 1 x 5 notch; derived: a hollow pointer in its
        // LINKED box; n/a: no track at all
        EventLog log;
        FakePort p("p", log, 0.5f);
        Spec sp;
        sp.state = ValueState::locked;
        sp.tag = "FIXED";
        Model ml(sp, &p);
        Dial l(ml, kGeom, 10);
        const Frame fl = drawOf(l);
        P.eq("draw.locked_dots", fl.count(tags::dialTrack), 24);          // k * 3 < 15 x 4.712
        P.eq("draw.locked_dot_size", fl.nth(tags::dialTrack) != nullptr && fl.nth(tags::dialTrack)->d2[0] == 0.75f, 1);
        P.eq("draw.locked_notch", fl.count(tags::dialNotch), 1);
        const funkgui::Prim* ln = fl.nth(tags::dialNotch);
        P.eq("draw.locked_notch_at_track", std::fabs(angleAt(ln) - angleOf(0.4f)) < 1e-4f, 1);
        // 1 x 5 centred at r 18: the segment runs r 16 .. 20, so with its round ends it spans r 15.5 .. 20.5
        P.eq("draw.locked_notch_1x5_r15.5_to_r20.5", ln != nullptr && ln->d2[0] == 0.5f
                                                     && std::fabs(dist(endA(*ln), kCentre) - 16.0f) < 1e-3f
                                                     && std::fabs(dist(endB(*ln), kCentre) - 20.0f) < 1e-3f
                                                     && ln->c0 == packed(th.ink32), 1);
        P.eq("draw.locked_no_pointer", fl.count(tags::dialPointer) + fl.count(tags::dialArc), 0);
        P.eq("draw.locked_tag_on_sub_line", fl.count(tags::slotSub), 5);
        P.eq("draw.locked_value_ink32", fl.colour(tags::slotValue) == packed(th.ink32), 1);
        sp.clamped = true;
        Model mlc(sp, &p);
        Dial lc(mlc, kGeom, 11);
        P.eq("draw.locked_clamped_no_notch", drawOf(lc).count(tags::dialNotch), 0);
        sp.state = ValueState::derived;
        sp.clamped = false;
        sp.tag = "= VALUE";
        Model md(sp, &p);
        Dial dv(md, kGeom, 12);
        const Frame fd = drawOf(dv);
        P.eq("draw.derived_track_and_box", fd.count(tags::dialTrack), trackSegs + 1);
        const funkgui::Prim* box = fd.nth(tags::dialTrack);
        P.eq("draw.derived_linked_box", box != nullptr && box->d2[2] == 0.0f && box->d1[0] == 4.0f
                                        && box->d2[0] == 1.0f && box->c1 == packed(th.ink32)
                                        && box->c0 == packed(funkgui::premix(th.ground, th.ink16, 0.35f))
                                        && std::fabs(mid(*box).x - 70.0f) < 1e-4f && box->d0[2] == 28.0f
                                        && box->d0[3] == 40.0f && std::fabs(mid(*box).y - 138.0f) < 1e-4f, 1);
        P.eq("draw.derived_hollow_pointer", fd.count(tags::dialPointer), 1);
        const funkgui::Prim* hollow = fd.nth(tags::dialPointer);
        P.eq("draw.derived_hollow_r3_ring_ink52", hollow != nullptr && hollow->d0[2] == 3.0f
                                                  && (hollow->c0 >> 24) == 0 && hollow->c1 == packed(th.ink52)
                                                  && std::fabs(dist(mid(*hollow), kCentre) - 15.0f) < 1e-3f, 1);
        P.eq("draw.derived_not_live", hollow != nullptr && (static_cast<int>(hollow->d2[3] + 0.5f) & 1) == 0, 1);
        P.eq("draw.derived_value_ink52", fd.colour(tags::slotValue) == packed(th.ink52), 1);
        P.eq("draw.derived_tag_glyphs", fd.count(tags::slotSub), 6);   // "= VALUE" without its space
        sp.live = true;
        Model mdl(sp, &p);
        Dial dl(mdl, kGeom, 13);
        const funkgui::Prim* lp = drawOf(dl).nth(tags::dialPointer);
        P.eq("draw.derived_live_pointer", lp != nullptr && (static_cast<int>(lp->d2[3] + 0.5f) & 1) == 1, 1);
        sp.state = ValueState::na;
        sp.live = false;
        Model mn(sp, &p);
        Dial n(mn, kGeom, 14);
        const Frame fn = drawOf(n);
        P.eq("draw.na_nothing", fn.count(tags::dialTrack) + fn.count(tags::dialPointer) + fn.count(tags::dialNotch)
                                    + fn.count(tags::dialArc) + fn.count(tags::slotSub), 0);
        P.eq("draw.na_dash", fn.count(tags::slotValue), 1);
        P.eq("draw.na_ink16", fn.colour(tags::slotValue) == packed(th.ink16)
                              && fn.colour(tags::slotLabel) == packed(th.ink16), 1);
    }
    {
        // the sub line: CLAMPED for a clamped continuous view, else the tag
        EventLog log;
        FakePort p("p", log, 0.5f);
        Spec sp;
        sp.clamped = true;
        Model mc(sp, &p);
        Dial c(mc, kGeom, 10);
        P.eq("draw.sub_clamped", drawOf(c).count(tags::slotSub), 7);
        sp.clamped = false;
        sp.tag = "+";
        Model mx(sp, &p);
        Dial x(mx, kGeom, 11);
        P.eq("draw.sub_extension_tag", drawOf(x).count(tags::slotSub), 1);
    }
    {
        // compact: the label only, at top + 38 in kMicro; the pointer r 5 to r 13; the ring at r 16
        EventLog log;
        FakePort p("p", log, 0.5f);
        Spec sp;
        sp.tag = "+";
        Model m(sp, &p);
        constexpr DialGeom cg{ 40.0f, 100.0f, DialSize::compact };
        Dial d(m, cg, 10);
        const Frame f = drawOf(d);
        P.eq("draw.compact_label_only", f.count(tags::slotLabel) == 5 && f.count(tags::slotValue) == 0
                                        && f.count(tags::slotSub) == 0, 1);
        P.eq("draw.compact_label_below", f.nth(tags::slotLabel) != nullptr && f.nth(tags::slotLabel)->y0 > 130.0f, 1);
        const funkgui::Prim* ptr = f.nth(tags::dialPointer);
        const Point c = cg.centre();
        P.eq("draw.compact_pointer_r5_to_r13", ptr != nullptr && std::fabs(dist(endA(*ptr), c) - 6.0f) < 1e-3f
                                               && std::fabs(dist(endB(*ptr), c) - 12.0f) < 1e-3f, 1);
        P.eq("draw.compact_track_arc", f.count(tags::dialTrack),
             funkgui::geom::arcSegments(11.0f, a0, a1, 1.0f, funkgui::geom::kArcDevicePx));
        P.eq("draw.dpi2_more_pieces", drawOf(d, false, 2.0f).count(tags::dialTrack),
             funkgui::geom::arcSegments(11.0f, a0, a1, 2.0f, funkgui::geom::kArcDevicePx));
        P.eq("geom.compact_value_box", sameRect(d.valueBox(), { 42.0f, 136.0f, 50.0f, 14.0f }), 1);
    }

    // ---- the cursor, valueBox, takesTypedKeys -----------------------------------------------------------------------
    {
        EventLog log;
        FakePort p("p", log, 0.5f);
        Model m(Spec{}, &p);
        Dial d(m, kGeom, 10);
        P.eq("cursor.updown", static_cast<int>(d.cursorAt({ 60.0f, kY })), static_cast<int>(funkgui::Cursor::upDown));
        P.eq("cursor.outside_normal", static_cast<int>(d.cursorAt({ 41.0f, kY })),
             static_cast<int>(funkgui::Cursor::normal));
        P.eq("cursor.ring_without_overlay_updown", static_cast<int>(d.cursorAt({ 70.0f, 114.0f })),
             static_cast<int>(funkgui::Cursor::upDown));
        P.eq("geom.value_box", sameRect(d.valueBox(), { 42.0f, 154.0f, 56.0f, 14.0f }), 1);
        P.eq("typed.continuous", d.takesTypedKeys(), 1);
        Model ms(stepped(lmh()), &p);
        Dial s(ms, kGeom, 11);
        P.eq("typed.stepped", s.takesTypedKeys(), 1);
        P.eq("contains.hit_half_open", d.contains({ 42.0f, 100.0f }) && !d.contains({ 98.0f, 120.0f })
                                       && !d.contains({ 60.0f, 176.0f }) && d.contains({ 97.9f, 175.9f }), 1);
    }

    // ---- label flash (0.6 s, then ease back) and the Mode-switch landing (the pointer eases instead of snapping) ----
    {
        EventLog log;
        FakePort p("p", log, 0.1f);
        Model m(Spec{}, &p);
        Dial d(m, kGeom, 10);
        P.eq("settle.fresh_is_settled", d.settled(), 1);
        d.flashLabel(0.6f);
        P.eq("flash.not_settled", d.settled(), 0);
        d.tick(1.0f / 60.0f, false, false, false);
        P.eq("flash.label_ink100", drawOf(d).colour(tags::slotLabel) == packed(th.ink100), 1);
        P.in("flash.frames", settleFrames(d), 90, 110);

        p.set(0.9f);                                      // a big jump snaps ...
        P.eq("settle.new_view_pending", d.settled(), 0);
        d.tick(1.0f / 60.0f, false, false, false);
        P.near("landing.plain_jump_snaps", pointerTrack(drawOf(d)), 0.9, 1e-4);
        p.set(0.1f);                                      // ... but a Mode switch's move eases (FC 02 §8.7)
        d.flashLabel(0.0f);
        d.tick(1.0f / 60.0f, false, false, false);
        const float t1 = pointerTrack(drawOf(d));
        P.eq("landing.eases", t1 > 0.15f && t1 < 0.85f, 1);
        P.in("landing.frames", settleFrames(d), 20, 120);
        P.near("landing.arrives", pointerTrack(drawOf(d)), 0.1, 1e-4);
        P.eq("landing.no_flash_at_0", drawOf(d).colour(tags::slotLabel) != packed(th.ink100), 1);
        p.set(0.15f);                                     // a small move eases (tau 90 ms)
        d.tick(1.0f / 60.0f, false, false, false);
        const float t2 = pointerTrack(drawOf(d));
        P.eq("ease.small_move_eases", t2 > 0.1f && t2 < 0.15f, 1);
        settleFrames(d);

        int in = 0;
        d.tick(1.0f / 60.0f, true, false, false);
        for (in = 1; !d.settled() && in < 600; ++in)
            d.tick(1.0f / 60.0f, true, false, false);
        P.in("hover.in_frames", in, 30, 50);
        int focusIn = 0;
        settleFrames(d);
        d.tick(1.0f / 60.0f, false, true, false);
        for (focusIn = 1; !d.settled() && focusIn < 600; ++focusIn)
            d.tick(1.0f / 60.0f, false, true, false);
        P.eq("hover.focus_is_under_the_hand", drawOf(d).colour(tags::slotValue) == packed(th.accent), 1);
    }

    // ---- spec lines -------------------------------------------------------------------------------------------------
    {
        EventLog log;
        FakePort p("p", log, 0.0f);
        Model st(stepped(lmh()), &p);
        Dial s(st, kGeom, 10);
        P.eq("spec.stepped", spec(s) == "STEPS   STEPS LO \xC2\xB7 MID \xC2\xB7 HI   DRAG / WHEEL / ARROWS STEP"
                                        "   DBL-CLICK RESET", 1);
        Spec rs;
        rs.label = "OUTER";
        rs.aka = "INNER";
        rs.reason = "DRIVES THE INNER VALUE";
        Model mr(rs, &p);
        Dial r(mr, kGeom, 11);
        P.eq("spec.renamed", spec(r) == "OUTER (INNER)   DRIVES THE INNER VALUE   DRAG   SHIFT FINE"
                                        "   DBL-CLICK RESET", 1);
        Spec xs;
        xs.label = "EXTRA";
        xs.tag = "+";
        Model mx(xs, &p);
        Dial x(mx, kGeom, 12);
        P.eq("spec.extension", spec(x) == "EXTRA   EXTENSION \xE2\x80\x94 NOT ON THE ORIGINAL UNIT \xE2\x80\x94 "
                                          "NEUTRAL AT "
                                          "DEFAULT   DRAG   SHIFT FINE   DBL-CLICK RESET", 1);
        P.eq("spec.truncates_on_codepoints", spec(x, 20) == "EXTRA   EXTENSION ", 1);
        Model mh(hybridSpec(), &p);
        Dial h(mh, kGeom, 13);
        P.eq("spec.hybrid", spec(h) == "HYBRID   END STEPS OFF   DRAG   SHIFT FINE   DBL-CLICK RESET", 1);
        char none[1] = { 'x' };
        h.specLine(none, 1);
        P.eq("spec.one_byte_buffer", none[0] == '\0', 1);
    }

    return P.finish();
}

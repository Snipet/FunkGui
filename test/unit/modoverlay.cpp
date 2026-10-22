// FUNKGUI_TEST name=fg.modoverlay timeout=600 gpu=0
//
// fg.modoverlay: ModOverlay on both widgets (widgets/ModOverlay.h, Dial::setMod, RuleSlider::setMod; FSynth
// docs/design/02-funkgui-and-ui.md §4.1, §14.1, §14.3): the band and the live dot's geometry from lo, hi and live on
// a dial (an arc at ringRadius(), 2 px, from the angle of lo to the angle of hi; a disc of r 2.5 at live's angle) and
// on a RuleSlider (a 2 px band at trackY()+6 from lo to hi on the drawn track, a 1 x 5 live tick); nothing drawn, and
// the frame unchanged primitive for primitive, with no overlay, a null one or one with routes == 0; the inks (signal;
// ink16 when inert, which also hides the live value; the edited route's band in accent over the band while editing);
// the live value as a live primitive (MOD_DOT) and only while liveValid; a 2 px mark for a band of no width; the
// overlay copied at setMod and re-read only when its key changes (settled() false until the next tick); the copy
// sanitised (clamped to 0..1, NaN as 0, pairs in order), on each widget, as each keeps its own; the overlay additive
// (without MOD_BAND and MOD_DOT a modulated control's frame is the plain one's, in all five states); ringContains and
// bandContains; the grab cursor over them; and the a11y description's "modulated by" clause. Spec rows only, each
// guarded so a missing primitive fails its row rather than the probe. No JUCE: a HeadlessGuiScope bakes the atlas
// where there is JUCE, and the test also runs without it and as wasm32.

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
#include <funkgui/widgets/ModOverlay.h>
#include <funkgui/widgets/RuleSlider.h>
#include <funkgui/widgets/ValueModel.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace T = funkgui::test;
using funkgui::A11yItem;
using funkgui::Dial;
using funkgui::DialGeom;
using funkgui::DialSize;
using funkgui::ModOverlay;
using funkgui::Point;
using funkgui::RuleSlider;
using funkgui::SlotGeom;
using funkgui::SlotSize;
using funkgui::ValueState;
using funkgui::ValueView;
namespace tags = funkgui::tags;

namespace
{
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kStart = 0.75f * kPi;                 // the dial's own arithmetic: 135 degrees + t * 270
    constexpr float kSweep = 1.5f * kPi;

    float angleOf(float t) { return kStart + t * kSweep; }

    class Port final : public funkgui::ParamPort
    {
    public:
        explicit Port(float v) : v_(v) {}
        float value01() const override { return v_; }
        float default01() const override { return 0.0f; }
        int   numSteps() const override { return std::numeric_limits<int>::max(); }
        void  beginGesture() override {}
        void  setValue01(float v) override { v_ = v; }
        void  endGesture() override {}
        const char* id() const override { return "p"; }
        void* native() const override { return nullptr; }

    private:
        float v_;
    };

    // A control whose track is its host value; a state, a label, an aka and hybrid low cells as asked.
    class Model final : public funkgui::ValueModel
    {
    public:
        Model(Port* p, ValueState s = ValueState::continuous) : port_(p), state(s) {}

        uint64_t key() const override
        {
            return (port_ != nullptr ? std::bit_cast<uint32_t>(port_->value01()) : 0u) ^ (rev << 40);
        }

        void view(ValueView& v) const override
        {
            v.state = state;
            v.label = "VALUE";
            v.aka = aka;
            v.track = port_ != nullptr ? port_->value01() : 0.5f;
            v.trackDefault = 0.5f;
            std::snprintf(v.text.value, sizeof v.text.value, "%.2f", static_cast<double>(v.track));
            if (state == ValueState::stepped)
            {
                v.nDetents = 5;
                v.detents = kFive;
                v.detent = 2;
                v.track = 0.5f;
            }
            if (lowCell)
            {
                v.nEndLo = 1;
                v.endLo = &kOff;
            }
        }

        funkgui::ParamPort* port() override { return port_; }
        float host01FromTrack(float t) const override { return t; }
        float defaultHost01() const override { return 0.5f; }

        Port*       port_;
        ValueState  state;
        const char* aka = nullptr;
        bool        lowCell = false;
        uint64_t    rev = 0;

        static constexpr funkgui::Detent kFive[5] = { { 0.0f, "1", "1" }, { 0.25f, "2", "2" }, { 0.5f, "3", "3" },
                                                     { 0.75f, "4", "4" }, { 1.0f, "5", "5" } };
        static constexpr funkgui::Detent kOff{ 0.0f, "OFF", "Off" };
    };

    std::vector<funkgui::Prim> frameOf(auto draw, float dpi = 1.0f)
    {
        static funkgui::Canvas canvas(funkgui::FontService::get().atlas());
        funkgui::FrameInfo fi;
        fi.logicalW = 400;
        fi.logicalH = 300;
        fi.dpi = dpi;
        canvas.begin(fi);
        draw(canvas);
        return canvas.end().prims;
    }

    std::vector<funkgui::Prim> drawOf(const Dial& d, float dpi = 1.0f)
    {
        return frameOf([&](funkgui::Canvas& c) { d.draw(c, funkgui::Theme::graphite(), false); }, dpi);
    }

    std::vector<funkgui::Prim> drawOf(const RuleSlider& s)
    {
        return frameOf([&](funkgui::Canvas& c) { s.draw(c, funkgui::Theme::graphite(), false); });
    }

    bool sameBits(float a, float b) { return funkgui::ease::sameBits(a, b); }

    bool samePrim(const funkgui::Prim& a, const funkgui::Prim& b)
    {
        bool same = sameBits(a.x0, b.x0) && sameBits(a.y0, b.y0) && sameBits(a.x1, b.x1) && sameBits(a.y1, b.y1)
                 && a.c0 == b.c0 && a.c1 == b.c1 && a.tag == b.tag;
        for (int i = 0; i < 4; ++i)
            same = same && sameBits(a.d0[i], b.d0[i]) && sameBits(a.d1[i], b.d1[i]) && sameBits(a.d2[i], b.d2[i]);
        return same && sameBits(a.e0[0], b.e0[0]) && sameBits(a.e0[1], b.e0[1]);
    }

    bool sameFrame(const std::vector<funkgui::Prim>& a, const std::vector<funkgui::Prim>& b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (!samePrim(a[i], b[i]))
                return false;
        return true;
    }

    std::vector<funkgui::Prim> tagged(const std::vector<funkgui::Prim>& f, funkgui::Tag t)
    {
        std::vector<funkgui::Prim> out;
        for (const auto& p : f)
            if (p.tag == t)
                out.push_back(p);
        return out;
    }

    uint32_t packed(funkgui::Col c)
    {
        return static_cast<uint32_t>(c.r) | static_cast<uint32_t>(c.g) << 8 | static_cast<uint32_t>(c.b) << 16
             | static_cast<uint32_t>(c.a) << 24;
    }

    bool isLive(const funkgui::Prim& p) { return (static_cast<int>(p.d2[3] + 0.5f) & 1) == 1; }

    Point mid(const funkgui::Prim& p) { return { 0.5f * (p.x0 + p.x1), 0.5f * (p.y0 + p.y1) }; }
    Point endA(const funkgui::Prim& p) { const Point m = mid(p); return { m.x + p.d1[0], m.y + p.d1[1] }; }
    Point endB(const funkgui::Prim& p) { const Point m = mid(p); return { m.x + p.d1[2], m.y + p.d1[3] }; }
    float dist(Point a, Point b) { return std::hypot(a.x - b.x, a.y - b.y); }

    Point onCircle(Point c, float r, float a)
    {
        return { c.x + r * static_cast<float>(std::cos(static_cast<double>(a))),
                 c.y + r * static_cast<float>(std::sin(static_cast<double>(a))) };
    }

    // Whether the run of segments goes from q0 to q1 (its first point at q0, its last at q1), every point at radius r.
    bool arcRun(const std::vector<funkgui::Prim>& segs, Point c, float r, Point q0, Point q1)
    {
        if (segs.empty())
            return false;
        bool onCircleAll = true;
        for (const auto& s : segs)
            onCircleAll = onCircleAll && std::fabs(dist(endA(s), c) - r) < 1e-3f
                       && std::fabs(dist(endB(s), c) - r) < 1e-3f;
        return onCircleAll && dist(endA(segs.front()), q0) < 1e-3f && dist(endB(segs.back()), q1) < 1e-3f;
    }

    // The rectangle an rrect primitive was recorded for.
    funkgui::Rect rectOf(const funkgui::Prim& p)
    {
        const Point m = mid(p);
        return { m.x - p.d0[2], m.y - p.d0[3], 2.0f * p.d0[2], 2.0f * p.d0[3] };
    }

    bool near(funkgui::Rect a, funkgui::Rect b)
    {
        return std::fabs(a.x - b.x) < 1e-4f && std::fabs(a.y - b.y) < 1e-4f && std::fabs(a.w - b.w) < 1e-4f
            && std::fabs(a.h - b.h) < 1e-4f;
    }

    // The first or last primitive of a run, or nullptr when there is none: a missing band fails its row, never the
    // probe. (Used inside one expression: the run is often a temporary.)
    const funkgui::Prim* firstOf(const std::vector<funkgui::Prim>& v) { return v.empty() ? nullptr : &v.front(); }
    const funkgui::Prim* lastOf(const std::vector<funkgui::Prim>& v) { return v.empty() ? nullptr : &v.back(); }
    bool inkIs(const funkgui::Prim* p, funkgui::Col c) { return p != nullptr && p->c0 == packed(c); }
    bool rectIs(const funkgui::Prim* p, funkgui::Rect r) { return p != nullptr && near(rectOf(*p), r); }

    // A frame without the overlay's own primitives (MOD_BAND, MOD_DOT): what the control itself drew.
    std::vector<funkgui::Prim> withoutMod(const std::vector<funkgui::Prim>& f)
    {
        std::vector<funkgui::Prim> out;
        for (const auto& p : f)
            if (p.tag != tags::modBand && p.tag != tags::modDot)
                out.push_back(p);
        return out;
    }

    const char* stateName(ValueState s)
    {
        switch (s)
        {
            case ValueState::continuous: return "continuous";
            case ValueState::stepped:    return "stepped";
            case ValueState::locked:     return "locked";
            case ValueState::derived:    return "derived";
            case ValueState::na:         return "na";
        }
        return "?";
    }

    class Host final : public funkgui::HostServices
    {
    public:
        void   setUnboundedDrag(bool) override {}
        void   showParamMenu(funkgui::ParamPort&, float, float) override {}
        void   nudgeFullRate() override {}
        double nowSeconds() const override { return 0.0; }
        void   beginBatch() override {}
        void   endBatch() override {}
    };

    ModOverlay band(int routes, float lo, float hi, uint64_t key)
    {
        ModOverlay m;
        m.routes = routes;
        m.lo = lo;
        m.hi = hi;
        m.key = key;
        return m;
    }

    constexpr DialGeom kDial{ 40.0f, 100.0f, DialSize::primary };      // centre (70, 134), ring r 20
    constexpr Point kC{ 70.0f, 134.0f };
    constexpr SlotGeom kSlot{ 40.0f, 100.0f, 112.0f, SlotSize::primary };   // track y 164, band y 170..172
}

int main(int argc, char** argv)
{
    const funkgui::HeadlessGuiScope gui;
    T::Probe P("fg.modoverlay", "", argc, argv);
    funkgui::FontService::get().atlas();
    P.eq("font.ok", funkgui::FontService::get().ok(), 1);
    const auto& th = funkgui::Theme::graphite();

    {
        const ModOverlay m;
        P.eq("struct.defaults", m.routes == 0 && m.lo == 0.0f && m.hi == 0.0f && m.live == 0.0f && !m.liveValid
                                && !m.inert && !m.editing && m.editLo == 0.0f && m.editHi == 0.0f && m.key == 0, 1);
    }

    // ---- the six tags (canvas/Tags.h): numbers 14 to 19 after HINT, each with its dump name -------------------------
    {
        const auto is = [](const char* a, const char* b) { return a != nullptr && std::strcmp(a, b) == 0; };
        P.eq("tags.numbers", tags::dialTrack == 14 && tags::dialArc == 15 && tags::dialPointer == 16
                             && tags::dialNotch == 17 && tags::modBand == 18 && tags::modDot == 19, 1);
        P.eq("tags.names", is(funkgui::tagName(tags::dialTrack), "DIAL_TRACK")
                           && is(funkgui::tagName(tags::dialArc), "DIAL_ARC")
                           && is(funkgui::tagName(tags::dialPointer), "DIAL_POINTER")
                           && is(funkgui::tagName(tags::dialNotch), "DIAL_NOTCH")
                           && is(funkgui::tagName(tags::modBand), "MOD_BAND")
                           && is(funkgui::tagName(tags::modDot), "MOD_DOT"), 1);
        P.eq("tags.round_trip", funkgui::tagFromName("DIAL_TRACK") == tags::dialTrack
                                && funkgui::tagFromName("MOD_DOT") == tags::modDot, 1);
        // TextEditor (widgets/TextEditor.h) took 20 to 24 after this card: the first free tag is now 25.
        P.eq("tags.hint_unchanged_next_unnamed", is(funkgui::tagName(tags::hint), "HINT")
                                                 && funkgui::tagName(25) == nullptr, 1);
    }

    // ---- the dial: nothing without routes, the frame unchanged primitive for primitive -----------------------------
    {
        Port p(0.5f);
        Model m(&p);
        Dial plain(m, kDial, 10);
        Dial withNull(m, kDial, 10);
        withNull.setMod(nullptr);
        ModOverlay none = band(0, 0.25f, 0.75f, 1);
        none.liveValid = true;
        none.live = 0.5f;
        Dial noRoutes(m, kDial, 10);
        noRoutes.setMod(&none);
        const auto ref = drawOf(plain);
        P.eq("dial.null_unchanged", sameFrame(drawOf(withNull), ref), 1);
        P.eq("dial.no_routes_unchanged", sameFrame(drawOf(noRoutes), ref), 1);
        P.eq("dial.no_routes_nothing", tagged(drawOf(noRoutes), tags::modBand).size()
                                           + tagged(drawOf(noRoutes), tags::modDot).size(), 0);
        ModOverlay one = band(1, 0.25f, 0.75f, 2);
        Dial back(m, kDial, 10);
        back.setMod(&one);
        back.setMod(nullptr);
        P.eq("dial.set_back_to_null_unchanged", sameFrame(drawOf(back), ref), 1);
        A11yItem a, b;
        plain.accessibility(a);
        noRoutes.accessibility(b);
        P.eq("dial.no_routes_a11y_unchanged", a.description.empty() && b.description.empty(), 1);
    }

    // ---- the dial's band: an arc at r 20 from lo to hi, 2 px, signal; the live dot -----------------------------------
    {
        Port p(0.5f);
        Model m(&p);
        Dial d(m, kDial, 10);
        ModOverlay o = band(1, 0.25f, 0.75f, 1);
        o.liveValid = true;
        o.live = 0.5f;
        d.setMod(&o);
        const auto f = drawOf(d);
        const auto segs = tagged(f, tags::modBand);
        P.eq("dial.band_pieces", static_cast<int>(segs.size()),
             funkgui::geom::arcSegments(20.0f, angleOf(0.25f), angleOf(0.75f), 1.0f, funkgui::geom::kArcDevicePx));
        P.eq("dial.band_arc_lo_to_hi", arcRun(segs, kC, 20.0f, onCircle(kC, 20.0f, angleOf(0.25f)),
                                              onCircle(kC, 20.0f, angleOf(0.75f))), 1);
        P.eq("dial.band_2px_signal", !segs.empty() && segs[0].d2[0] == 1.0f && segs[0].c0 == packed(th.signal)
                                     && !isLive(segs[0]), 1);
        const auto dots = tagged(f, tags::modDot);
        P.eq("dial.dot_one", static_cast<int>(dots.size()), 1);
        P.eq("dial.dot_disc_r2.5_at_live", dots.size() == 1 && dots[0].d0[2] == 2.5f && dots[0].d0[3] == 2.5f
                                           && dist(mid(dots[0]), { 70.0f, 114.0f }) < 1e-4f
                                           && dots[0].c0 == packed(th.signal), 1);
        P.eq("dial.dot_live", dots.size() == 1 && isLive(dots[0]), 1);
        P.eq("dial.band_after_the_pointer", [&] {
            size_t lastPointer = 0, firstBand = f.size();
            for (size_t i = 0; i < f.size(); ++i)
            {
                if (f[i].tag == tags::dialPointer)
                    lastPointer = i;
                if (f[i].tag == tags::modBand && i < firstBand)
                    firstBand = i;
            }
            return firstBand < f.size() && firstBand > lastPointer;   // a band, and after the pointer
        }(), 1);
        P.eq("dial.dpi2_more_pieces", static_cast<int>(tagged(drawOf(d, 2.0f), tags::modBand).size()),
             funkgui::geom::arcSegments(20.0f, angleOf(0.25f), angleOf(0.75f), 2.0f, funkgui::geom::kArcDevicePx));

        // stale: no dot; inert: ink16 and no dot
        o.liveValid = false;
        o.key = 2;
        d.tick(0.0f, false, false, false);
        P.eq("dial.stale_no_dot", tagged(drawOf(d), tags::modDot).empty(), 1);
        P.eq("dial.stale_band_still_drawn", tagged(drawOf(d), tags::modBand).size() == segs.size(), 1);
        o.liveValid = true;
        o.inert = true;
        o.key = 3;
        d.tick(0.0f, false, false, false);
        const auto fi = drawOf(d);
        P.eq("dial.inert_ink16", inkIs(firstOf(tagged(fi, tags::modBand)), th.ink16), 1);
        P.eq("dial.inert_no_dot", tagged(fi, tags::modDot).empty(), 1);

        // editing: the edited route's band in accent over the band
        o.inert = false;
        o.editing = true;
        o.editLo = 0.5f;
        o.editHi = 0.75f;
        o.key = 4;
        d.tick(0.0f, false, false, false);
        const auto fe = tagged(drawOf(d), tags::modBand);
        std::vector<funkgui::Prim> sig, acc;
        for (const auto& s : fe)
            (s.c0 == packed(th.accent) ? acc : sig).push_back(s);
        P.eq("dial.editing_band_then_edit", !sig.empty() && sig.size() == segs.size()
                                            && inkIs(firstOf(fe), th.signal) && inkIs(lastOf(fe), th.accent), 1);
        P.eq("dial.editing_accent_arc", arcRun(acc, kC, 20.0f, onCircle(kC, 20.0f, angleOf(0.5f)),
                                               onCircle(kC, 20.0f, angleOf(0.75f))), 1);
        o.inert = true;
        o.key = 5;
        d.tick(0.0f, false, false, false);
        const auto fie = tagged(drawOf(d), tags::modBand);
        P.eq("dial.inert_editing_accent_over_ink16", inkIs(firstOf(fie), th.ink16) && inkIs(lastOf(fie), th.accent),
             1);

        // a band of no width (a route at depth 0) is one round 2 px mark
        o = band(1, 0.6f, 0.6f, 6);
        d.tick(0.0f, false, false, false);
        const auto z = tagged(drawOf(d), tags::modBand);
        P.eq("dial.zero_width_mark", z.size() == 1 && dist(endA(z[0]), endB(z[0])) < 1e-6f
                                     && dist(endA(z[0]), onCircle(kC, 20.0f, angleOf(0.6f))) < 1e-3f
                                     && z[0].d2[0] == 1.0f, 1);
    }

    // ---- the overlay's key: copied at setMod, re-read only when the key changes ------------------------------------
    {
        Port p(0.5f);
        Model m(&p);
        Dial d(m, kDial, 10);
        ModOverlay o = band(1, 0.25f, 0.5f, 7);
        d.setMod(&o);
        const auto f0 = drawOf(d);
        P.eq("key.settled_after_set", d.settled(), 1);
        o.hi = 1.0f;                                      // a field changed without the key: not seen
        d.tick(1.0f / 60.0f, false, false, false);
        P.eq("key.same_key_not_reread", sameFrame(drawOf(d), f0) && d.settled(), 1);
        o.key = 8;
        P.eq("key.new_key_not_settled", d.settled(), 0);
        P.eq("key.const_draw_keeps_the_copy", sameFrame(drawOf(d), f0), 1);
        d.tick(1.0f / 60.0f, false, false, false);
        P.eq("key.reread_on_tick", d.settled() && !sameFrame(drawOf(d), f0), 1);
        P.eq("key.new_band_to_1", arcRun(tagged(drawOf(d), tags::modBand), kC, 20.0f,
                                         onCircle(kC, 20.0f, angleOf(0.25f)), onCircle(kC, 20.0f, angleOf(1.0f))), 1);
        // an input call re-reads it too (a key the dial does not take still refreshes, as the view does)
        o.lo = 0.0f;
        o.key = 9;
        Host host;
        funkgui::GestureController g(host);
        funkgui::KeyEvent k;
        k.key = funkgui::Key::up;
        d.key(k, g);
        P.eq("key.reread_on_input", arcRun(tagged(drawOf(d), tags::modBand), kC, 20.0f,
                                           onCircle(kC, 20.0f, angleOf(0.0f)), onCircle(kC, 20.0f, angleOf(1.0f))), 1);
        // a different overlay at the same key is read at setMod
        ModOverlay other = band(1, 0.5f, 0.5f, 9);
        d.setMod(&other);
        P.eq("key.set_mod_rereads", tagged(drawOf(d), tags::modBand).size() == 1, 1);
    }

    // ---- the copy is sanitised: clamped to 0..1, NaN as 0, pairs in order -------------------------------------------
    {
        Port p(0.5f);
        Model m(&p);
        Dial d(m, kDial, 10);
        ModOverlay o = band(2, 0.8f, std::numeric_limits<float>::quiet_NaN(), 1);   // hi NaN -> 0, then swapped
        o.liveValid = true;
        o.live = 7.0f;                                    // -> 1
        o.editing = true;
        o.editLo = 2.0f;                                  // -> 1
        o.editHi = -1.0f;                                 // -> 0, then swapped
        d.setMod(&o);
        const auto f = drawOf(d);
        std::vector<funkgui::Prim> sig, acc;
        for (const auto& s : tagged(f, tags::modBand))
            (s.c0 == packed(th.accent) ? acc : sig).push_back(s);
        P.eq("sanitise.band_0_to_0.8", arcRun(sig, kC, 20.0f, onCircle(kC, 20.0f, angleOf(0.0f)),
                                              onCircle(kC, 20.0f, angleOf(0.8f))), 1);
        P.eq("sanitise.edit_0_to_1", arcRun(acc, kC, 20.0f, onCircle(kC, 20.0f, angleOf(0.0f)),
                                            onCircle(kC, 20.0f, angleOf(1.0f))), 1);
        const auto dots = tagged(f, tags::modDot);
        P.eq("sanitise.live_to_1", dots.size() == 1 && dist(mid(dots[0]), onCircle(kC, 20.0f, angleOf(1.0f))) < 1e-4f,
             1);
        o = band(1, 0.75f, 0.25f, 2);                     // lo > hi: swapped
        o.liveValid = true;
        o.live = std::numeric_limits<float>::quiet_NaN(); // -> 0
        d.tick(0.0f, false, false, false);
        const auto f2 = drawOf(d);
        P.eq("sanitise.lo_hi_swapped", arcRun(tagged(f2, tags::modBand), kC, 20.0f, onCircle(kC, 20.0f, angleOf(0.25f)),
                                              onCircle(kC, 20.0f, angleOf(0.75f))), 1);
        const auto dots2 = tagged(f2, tags::modDot);
        P.eq("sanitise.live_nan_to_0",
             dots2.size() == 1 && dist(mid(dots2[0]), onCircle(kC, 20.0f, angleOf(0.0f))) < 1e-4f, 1);
    }

    // ---- ringContains: the annulus radius()+2 .. ringRadius()+4 (17 .. 24 primary, 13 .. 20 compact) ----------------
    {
        Port p(0.5f);
        Model m(&p);
        Dial d(m, kDial, 10);
        P.eq("ring.inner_edge", !d.ringContains({ 70.0f, 134.0f - 16.9f }) && d.ringContains({ 70.0f, 134.0f - 17.0f }),
             1);
        P.eq("ring.outer_edge", d.ringContains({ 70.0f + 23.9f, 134.0f }) && !d.ringContains({ 70.0f + 24.0f, 134.0f }),
             1);
        P.eq("ring.the_whole_annulus", d.ringContains({ 70.0f, 154.0f }) && d.ringContains({ 50.0f, 134.0f }), 1);
        P.eq("ring.not_the_centre", d.ringContains(kC), 0);
        Dial c(m, { 40.0f, 100.0f, DialSize::compact }, 11);   // centre (67, 120)
        P.eq("ring.compact", !c.ringContains({ 67.0f, 120.0f - 12.9f }) && c.ringContains({ 67.0f, 120.0f - 13.0f })
                             && c.ringContains({ 67.0f, 120.0f + 19.9f }) && !c.ringContains({ 67.0f, 120.0f + 20.0f }),
             1);
        ModOverlay o = band(2, 0.25f, 0.75f, 1);
        c.setMod(&o);
        P.eq("ring.compact_band_at_r16", arcRun(tagged(drawOf(c), tags::modBand), { 67.0f, 120.0f }, 16.0f,
                                                onCircle({ 67.0f, 120.0f }, 16.0f, angleOf(0.25f)),
                                                onCircle({ 67.0f, 120.0f }, 16.0f, angleOf(0.75f))), 1);
    }

    // ---- the cursor: grab over the ring with routes on a continuous or stepped dial ---------------------------------
    {
        Port p(0.5f);
        Model m(&p);
        Dial d(m, kDial, 10);
        const auto cur = [](funkgui::Cursor c) { return static_cast<int>(c); };
        P.eq("cursor.ring_without_routes", cur(d.cursorAt({ 70.0f, 114.0f })), cur(funkgui::Cursor::upDown));
        ModOverlay o = band(1, 0.25f, 0.75f, 1);
        d.setMod(&o);
        P.eq("cursor.ring_grab", cur(d.cursorAt({ 70.0f, 114.0f })), cur(funkgui::Cursor::grab));
        P.eq("cursor.centre_updown", cur(d.cursorAt(kC)), cur(funkgui::Cursor::upDown));
        Model ms(&p, ValueState::stepped);
        Dial s(ms, kDial, 11);
        s.setMod(&o);
        P.eq("cursor.stepped_ring_grab", cur(s.cursorAt({ 70.0f, 114.0f })), cur(funkgui::Cursor::grab));
        Model ml(&p, ValueState::locked);
        Dial l(ml, kDial, 12);
        l.setMod(&o);
        P.eq("cursor.locked_ring_normal", cur(l.cursorAt({ 70.0f, 114.0f })), cur(funkgui::Cursor::normal));
        Model mo(nullptr);
        Dial modOnly(mo, kDial, 13);                      // a modulation-only target: display-only, routes editable
        modOnly.setMod(&o);
        P.eq("cursor.display_only_ring_grab", cur(modOnly.cursorAt({ 70.0f, 114.0f })), cur(funkgui::Cursor::grab));
        P.eq("cursor.display_only_centre_normal", cur(modOnly.cursorAt(kC)), cur(funkgui::Cursor::normal));
    }

    // ---- the a11y description -------------------------------------------------------------------------------------
    {
        Port p(0.5f);
        Model m(&p);
        Dial d(m, kDial, 10);
        A11yItem it;
        ModOverlay o = band(1, 0.25f, 0.75f, 1);
        d.setMod(&o);
        d.accessibility(it);
        P.eq("a11y.one_route", it.description == "modulated by 1 route", 1);
        o = band(3, 0.25f, 0.75f, 2);
        d.tick(0.0f, false, false, false);
        d.accessibility(it);
        P.eq("a11y.three_routes", it.description == "modulated by 3 routes", 1);
        o.inert = true;
        o.routes = 2;
        o.key = 3;
        m.aka = "INNER";
        ++m.rev;
        d.tick(0.0f, false, false, false);
        d.accessibility(it);
        P.eq("a11y.aka_and_inert", it.description == "controls inner, modulated by 2 routes, no effect", 1);
    }

    // ---- RuleSlider: nothing without routes, the frame unchanged primitive for primitive -----------------------------
    {
        Port p(0.5f);
        Model m(&p);
        RuleSlider plain(m, kSlot, 10);
        RuleSlider withNull(m, kSlot, 10);
        withNull.setMod(nullptr);
        ModOverlay none = band(0, 0.25f, 0.75f, 1);
        none.liveValid = true;
        RuleSlider noRoutes(m, kSlot, 10);
        noRoutes.setMod(&none);
        const auto ref = drawOf(plain);
        P.eq("rule.null_unchanged", sameFrame(drawOf(withNull), ref), 1);
        P.eq("rule.no_routes_unchanged", sameFrame(drawOf(noRoutes), ref), 1);
        A11yItem a, b;
        plain.accessibility(a);
        noRoutes.accessibility(b);
        P.eq("rule.no_routes_a11y_unchanged", a.description == b.description && a.description.empty(), 1);
        P.eq("rule.no_routes_cursor_unchanged", static_cast<int>(noRoutes.cursorAt({ 60.0f, 170.0f })),
             static_cast<int>(funkgui::Cursor::leftRight));
        P.eq("rule.no_routes_settled", noRoutes.settled(), 1);
    }

    // ---- RuleSlider's band: 2 px at trackY()+6 from lo to hi; a 1 x 5 live tick ---------------------------------------
    {
        Port p(0.5f);
        Model m(&p);
        RuleSlider s(m, kSlot, 10);
        ModOverlay o = band(1, 0.25f, 0.75f, 1);
        o.liveValid = true;
        o.live = 0.5f;
        s.setMod(&o);
        const auto f = drawOf(s);
        const auto bands = tagged(f, tags::modBand);
        P.eq("rule.band_one_rect", static_cast<int>(bands.size()), 1);
        P.eq("rule.band_geometry", bands.size() == 1 && near(rectOf(bands[0]), { 68.0f, 170.0f, 56.0f, 2.0f })
                                   && bands[0].c0 == packed(th.signal) && !isLive(bands[0]), 1);
        const auto ticks = tagged(f, tags::modDot);
        P.eq("rule.live_tick", ticks.size() == 1 && near(rectOf(ticks[0]), { 96.0f, 169.0f, 1.0f, 5.0f })
                               && isLive(ticks[0]) && ticks[0].c0 == packed(th.signal), 1);
        P.eq("rule.band_after_the_caret", lastOf(f) != nullptr && lastOf(f)->tag == tags::modDot, 1);

        o.liveValid = false;
        o.key = 2;
        s.tick(0.0f, false, false, false);
        P.eq("rule.stale_no_tick", tagged(drawOf(s), tags::modDot).empty(), 1);
        o.liveValid = true;
        o.inert = true;
        o.key = 3;
        s.tick(0.0f, false, false, false);
        const auto fi = drawOf(s);
        P.eq("rule.inert_ink16_no_tick", inkIs(firstOf(tagged(fi, tags::modBand)), th.ink16)
                                         && tagged(fi, tags::modDot).empty(), 1);
        o.inert = false;
        o.editing = true;
        o.editLo = 0.5f;
        o.editHi = 0.625f;
        o.key = 4;
        s.tick(0.0f, false, false, false);
        const auto fe = tagged(drawOf(s), tags::modBand);
        P.eq("rule.editing_accent_over_band", fe.size() == 2 && fe[0].c0 == packed(th.signal)
                                              && fe[1].c0 == packed(th.accent)
                                              && near(rectOf(fe[1]), { 96.0f, 170.0f, 14.0f, 2.0f }), 1);
        o = band(1, 0.5f, 0.5f, 5);                       // a route at depth 0: a 2 px mark, centred
        s.tick(0.0f, false, false, false);
        const auto z = tagged(drawOf(s), tags::modBand);
        P.eq("rule.zero_width_mark", z.size() == 1 && near(rectOf(z[0]), { 95.0f, 170.0f, 2.0f, 2.0f }), 1);
        o = band(1, 0.0f, 1.0f, 6);
        s.tick(0.0f, false, false, false);
        P.eq("rule.full_track", rectIs(firstOf(tagged(drawOf(s), tags::modBand)), { 40.0f, 170.0f, 112.0f, 2.0f }), 1);
        o.key = 7;
        P.eq("rule.new_key_not_settled", s.settled(), 0);
        s.tick(0.0f, false, false, false);
        P.eq("rule.settled_after_tick", s.settled(), 1);

        A11yItem it;
        o = band(3, 0.0f, 1.0f, 8);
        s.tick(0.0f, false, false, false);
        s.accessibility(it);
        P.eq("rule.a11y_routes", it.description == "modulated by 3 routes", 1);
        o = band(1, 0.0f, 1.0f, 9);
        o.inert = true;
        m.aka = "INNER";
        ++m.rev;
        s.tick(0.0f, false, false, false);
        s.accessibility(it);
        P.eq("rule.a11y_aka_one_route_inert", it.description == "controls inner, modulated by 1 route, no effect", 1);
    }
    {
        // the slider keeps its own copy of the overlay, sanitised as the dial's (ModOverlay.h); the edited band, the
        // 2 px minimum and the inks as the dial's rows pin them
        Port p(0.5f);
        Model m(&p);
        RuleSlider s(m, kSlot, 10);
        ModOverlay o = band(2, 0.8f, std::numeric_limits<float>::quiet_NaN(), 1);   // hi NaN -> 0, then swapped
        o.liveValid = true;
        o.live = 7.0f;                                    // -> 1
        o.editing = true;
        o.editLo = 2.0f;                                  // -> 1
        o.editHi = -1.0f;                                 // -> 0, then swapped
        s.setMod(&o);
        const auto f = drawOf(s);
        const auto bands = tagged(f, tags::modBand);
        P.eq("rule.sanitise.band_0_to_0.8", bands.size() == 2 && inkIs(&bands[0], th.signal)
                                            && near(rectOf(bands[0]), { 40.0f, 170.0f, 0.8f * 112.0f, 2.0f }), 1);
        P.eq("rule.sanitise.edit_0_to_1", bands.size() == 2 && inkIs(&bands[1], th.accent)
                                          && near(rectOf(bands[1]), { 40.0f, 170.0f, 112.0f, 2.0f }), 1);
        P.eq("rule.sanitise.live_to_1", rectIs(firstOf(tagged(f, tags::modDot)), { 152.0f, 169.0f, 1.0f, 5.0f }), 1);
        o = band(1, 0.75f, 0.25f, 2);                     // lo > hi: swapped
        o.liveValid = true;
        o.live = std::numeric_limits<float>::quiet_NaN(); // -> 0
        s.tick(0.0f, false, false, false);
        const auto f2 = drawOf(s);
        P.eq("rule.sanitise.lo_hi_swapped", rectIs(firstOf(tagged(f2, tags::modBand)), { 68.0f, 170.0f, 56.0f, 2.0f }),
             1);
        P.eq("rule.sanitise.live_nan_to_0", rectIs(firstOf(tagged(f2, tags::modDot)), { 40.0f, 169.0f, 1.0f, 5.0f }),
             1);

        o = band(1, 0.25f, 0.75f, 3);                     // inert and editing: the edited band in accent over ink16
        o.inert = true;
        o.editing = true;
        o.editLo = 0.5f;
        o.editHi = 0.625f;
        s.tick(0.0f, false, false, false);
        const auto fie = tagged(drawOf(s), tags::modBand);
        P.eq("rule.inert_editing_accent_over_ink16", fie.size() == 2 && inkIs(&fie[0], th.ink16)
                                                     && inkIs(&fie[1], th.accent)
                                                     && near(rectOf(fie[1]), { 96.0f, 170.0f, 14.0f, 2.0f }), 1);
        o = band(1, 0.5f, 0.51f, 4);                      // 1.12 px of band: drawn 2 px wide, centred
        s.tick(0.0f, false, false, false);
        P.eq("rule.narrow_band_2px", rectIs(firstOf(tagged(drawOf(s), tags::modBand)),
                                            { 96.56f - 1.0f, 170.0f, 2.0f, 2.0f }), 1);
        o = band(1, 0.25f, 0.75f, 5);                     // an edited route at depth 0: a 2 px accent mark
        o.editing = true;
        o.editLo = 0.5f;
        o.editHi = 0.5f;
        s.tick(0.0f, false, false, false);
        const auto fz = tagged(drawOf(s), tags::modBand);
        P.eq("rule.zero_width_edit_mark", fz.size() == 2 && inkIs(&fz[1], th.accent)
                                          && near(rectOf(fz[1]), { 95.0f, 170.0f, 2.0f, 2.0f }), 1);
    }
    {
        // a hybrid slider: the band runs on the continuous part (x + 16 + 4 .. x + w); a secondary slider's band
        Port p(0.5f);
        Model m(&p);
        m.lowCell = true;
        RuleSlider s(m, kSlot, 10);
        ModOverlay o = band(1, 0.0f, 1.0f, 1);
        o.liveValid = true;
        o.live = 0.0f;
        s.setMod(&o);
        const auto fh = drawOf(s);
        P.eq("rule.hybrid_continuous_part", rectIs(firstOf(tagged(fh, tags::modBand)), { 60.0f, 170.0f, 92.0f, 2.0f }),
             1);
        P.eq("rule.hybrid_live_tick_at_0", rectIs(firstOf(tagged(fh, tags::modDot)), { 60.0f, 169.0f, 1.0f, 5.0f }), 1);
        o.live = 0.5f;
        o.key = 2;
        s.tick(0.0f, false, false, false);
        P.eq("rule.hybrid_live_tick_at_0.5", rectIs(firstOf(tagged(drawOf(s), tags::modDot)),
                                                    { 106.0f, 169.0f, 1.0f, 5.0f }), 1);
        o.liveValid = false;
        o.key = 3;
        s.tick(0.0f, false, false, false);
        Model m2(&p);
        RuleSlider s2(m2, { 40.0f, 100.0f, 112.0f, SlotSize::secondary }, 11);
        s2.setMod(&o);
        P.eq("rule.secondary_track_y", rectIs(firstOf(tagged(drawOf(s2), tags::modBand)),
                                              { 40.0f, 160.0f, 112.0f, 2.0f }), 1);
        Model ms(&p, ValueState::stepped);
        RuleSlider st(ms, kSlot, 12);
        o = band(1, 0.3f, 0.7f, 2);
        st.setMod(&o);
        P.eq("rule.stepped_band_verbatim", rectIs(firstOf(tagged(drawOf(st), tags::modBand)),
                                                  { 40.0f + 0.3f * 112.0f, 170.0f, 0.4f * 112.0f, 2.0f }), 1);
    }

    // ---- the overlay is additive: a modulated control's own primitives are the plain control's, in every state ------
    for (const ValueState st : { ValueState::continuous, ValueState::stepped, ValueState::locked, ValueState::derived,
                                 ValueState::na })
    {
        Port p(0.5f);
        Model m(&p, st);
        ModOverlay o = band(3, 0.25f, 0.75f, 1);
        o.liveValid = true;
        o.live = 0.6f;
        o.editing = true;
        o.editLo = 0.5f;
        o.editHi = 0.6f;
        Dial plain(m, kDial, 10);
        Dial modded(m, kDial, 10);
        modded.setMod(&o);
        RuleSlider rPlain(m, kSlot, 11);
        RuleSlider rModded(m, kSlot, 11);
        rModded.setMod(&o);
        bool dialSame = true, ruleSame = true;
        for (int round = 0; round < 2; ++round)           // at rest, then hovered and focused
        {
            const auto fd = drawOf(modded);
            const auto fr = drawOf(rModded);
            dialSame = dialSame && !tagged(fd, tags::modBand).empty() && sameFrame(withoutMod(fd), drawOf(plain));
            ruleSame = ruleSame && !tagged(fr, tags::modBand).empty() && sameFrame(withoutMod(fr), drawOf(rPlain));
            for (int i = 0; i < 60; ++i)
            {
                plain.tick(1.0f / 60.0f, true, true, false);
                modded.tick(1.0f / 60.0f, true, true, false);
                rPlain.tick(1.0f / 60.0f, true, true, false);
                rModded.tick(1.0f / 60.0f, true, true, false);
            }
        }
        P.eq(std::string("dial.additive_") + stateName(st), dialSame, 1);
        P.eq(std::string("rule.additive_") + stateName(st), ruleSame, 1);
    }

    // ---- bandContains {x, trackY()+3, w, 8} and the grab cursor ------------------------------------------------------
    {
        Port p(0.5f);
        Model m(&p);
        RuleSlider s(m, kSlot, 10);
        P.eq("bandhit.corners", s.bandContains({ 40.0f, 167.0f }) && s.bandContains({ 151.9f, 174.9f })
                                && !s.bandContains({ 39.9f, 170.0f }) && !s.bandContains({ 152.0f, 170.0f })
                                && !s.bandContains({ 60.0f, 166.9f }) && !s.bandContains({ 60.0f, 175.0f }), 1);
        RuleSlider s2(m, { 40.0f, 100.0f, 112.0f, SlotSize::secondary }, 11);
        P.eq("bandhit.secondary", s2.bandContains({ 60.0f, 157.0f }) && !s2.bandContains({ 60.0f, 165.0f }), 1);
        const auto cur = [](funkgui::Cursor c) { return static_cast<int>(c); };
        ModOverlay o = band(1, 0.25f, 0.75f, 1);
        s.setMod(&o);
        P.eq("bandhit.grab", cur(s.cursorAt({ 60.0f, 170.0f })), cur(funkgui::Cursor::grab));
        P.eq("bandhit.grab_below_the_hit", cur(s.cursorAt({ 60.0f, 174.5f })), cur(funkgui::Cursor::grab));
        P.eq("bandhit.track_leftright", cur(s.cursorAt({ 60.0f, 150.0f })), cur(funkgui::Cursor::leftRight));
        Model ml(&p, ValueState::locked);
        RuleSlider l(ml, kSlot, 12);
        l.setMod(&o);
        P.eq("bandhit.locked_normal", cur(l.cursorAt({ 60.0f, 170.0f })), cur(funkgui::Cursor::normal));
    }

    return P.finish();
}

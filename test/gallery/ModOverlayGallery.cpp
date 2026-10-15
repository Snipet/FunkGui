// FUNKGUI_TEST name=fg.gallery.modoverlay timeout=600 gpu=0 exe=FunkGuiGalleryProbe args="--section modoverlay"
//
// The "modoverlay" gallery section (FSynth G2; FSynth docs/design/02-funkgui-and-ui.md §4.2, §14.1, §14.3, §14.4):
// ModOverlay on dials and on RuleSlider's band, with the overlays computed the way a product computes them from its
// routes (a base, each route's interval [min(0, d), max(0, d)] summed and clamped, a stepped role's ends snapped to
// detent centres; the live value scripted, as a probe scripts telemetry). Each control is labelled with what it shows:
//   row 1  LIVE DOT, 1 route and a live dot · STALE, 3 routes and a stale overlay (no dot) · INERT, a locked dial
//          whose band is inert (ink16, no dot) · SNAPPED, stepped, 2 routes, ends snapped to detents, a live dot on a
//          detent · BIPOLAR, 2 routes around 12:00 · 0 ROUTES, an overlay with no routes (nothing drawn) · DEPTH 0,
//          1 route at depth 0 (a 2 px mark)
//   row 2  LIVE TICK, a primary RuleSlider with 1 route and a live tick · EDITED, a secondary RuleSlider with 3
//          routes, route 2's band edited (accent over the signal band) · COMPACT, a compact dial, 2 routes, a live dot
// and a footer with the product's line for a depth edit. States: rest; editing (a press on STALE's ring, held, moved
// 30 px right: the section starts a depth edit on STALE's active route, route 3, 120 px from 0 to 1, so its depth
// goes from +0.10 to +0.35; `editing` draws that route's band in accent over the whole band, and the footer reads
// "ROUTE 3 · DEPTH +0.35 · 3 ROUTES"). The live dots are live primitives, so the fingerprint leaves them out. Every
// value is exact arithmetic on host-normalised values. Its text names the states shown, never a product's Modes or
// parameters: its a11y lines become goldens. No JUCE and no platform code: the section also compiles into the
// gallery app and, under Emscripten, the web gallery and the web host's page. The line above registers the test; the
// tools glob compiles this file into FunkGuiGalleryProbe.

#include "GalleryPanel.h"

#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Ease.h>
#include <funkgui/core/Format.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/params/ParamPort.h>
#include <funkgui/widgets/Dial.h>
#include <funkgui/widgets/ModOverlay.h>
#include <funkgui/widgets/RuleSlider.h>
#include <funkgui/widgets/ValueModel.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace funkgui::gallery
{
    namespace
    {
        // ---- an in-memory parameter and a data-driven ValueModel ----------------------------------------------------

        class MemPort final : public ParamPort
        {
        public:
            MemPort(const char* id, float v) : id_(id), v_(v) {}
            float value01() const override { return v_; }
            float default01() const override { return 0.0f; }
            int   numSteps() const override { return 0x7fffffff; }
            void  beginGesture() override {}
            void  setValue01(float v) override { v_ = v; }
            void  endGesture() override {}
            const char* id() const override { return id_; }
            void* native() const override { return nullptr; }

        private:
            const char* id_;
            float       v_;
        };

        // A control as a product would describe it: host01 maps onto the track one to one, so track space is host
        // space and a route's depth adds to the base directly.
        struct Def
        {
            ValueState  state = ValueState::continuous;
            const char* label = "";
            const char* tag = nullptr;
            const char* reason = nullptr;
            float       lo = 0.0f, hi = 100.0f;          // plain range (continuous)
            bool        plus = false;
            const char* unit = "%";
            std::vector<Detent> detents;
            float       init = 0.5f;                     // host01
            bool        bipolar = false;
            const char* fixedValue = "";                 // locked
            // the routes reaching it
            std::vector<float> depths;                   // each route's depth, -1..1
            bool        overlay = true;                  // false: no overlay at all
            bool        liveValid = false;
            float       live = 0.0f;
            bool        inert = false;
            int         edited = -1;                     // the route whose band is drawn edited (-1: none)
        };

        class Model final : public ValueModel
        {
        public:
            Model(const Def& d, MemPort& p) : d_(d), port_(p) {}

            uint64_t key() const override { return std::bit_cast<uint32_t>(port_.value01()); }

            void view(ValueView& v) const override
            {
                const float h = port_.value01();
                v.state = d_.state;
                v.label = d_.label;
                v.tag = d_.tag;
                v.reason = d_.reason;
                v.bipolar = d_.bipolar;
                v.trackDefault = 0.5f;
                if (d_.state == ValueState::stepped)
                {
                    v.nDetents = static_cast<int>(d_.detents.size());
                    v.detents = d_.detents.data();
                    int best = 0;
                    for (int i = 1; i < v.nDetents; ++i)
                        if (std::fabs(d_.detents[static_cast<size_t>(i)].host01 - h)
                            < std::fabs(d_.detents[static_cast<size_t>(best)].host01 - h))
                            best = i;
                    v.detent = best;
                    v.track = (static_cast<float>(best) + 0.5f) / static_cast<float>(v.nDetents);
                    std::snprintf(v.text.value, sizeof v.text.value, "%s", d_.detents[static_cast<size_t>(best)].label);
                    std::snprintf(v.text.spoken, sizeof v.text.spoken, "%s",
                                  d_.detents[static_cast<size_t>(best)].spoken);
                    return;
                }
                v.track = h;
                v.atDefault = ease::sameBits(h, 0.5f);
                if (d_.state != ValueState::continuous)
                {
                    std::snprintf(v.text.value, sizeof v.text.value, "%s", d_.fixedValue);
                    std::snprintf(v.text.unit, sizeof v.text.unit, "%s", d_.unit);
                    return;
                }
                const float plain = d_.lo + h * (d_.hi - d_.lo);
                char num[20];
                fmt::db(plain, 0, num, sizeof num);
                std::snprintf(v.text.value, sizeof v.text.value, "%s%s", d_.plus && plain > 0.0f ? "+" : "", num);
                std::snprintf(v.text.unit, sizeof v.text.unit, "%s", d_.unit);
            }

            ParamPort* port() override { return &port_; }
            float host01FromTrack(float t) const override { return t; }
            float defaultHost01() const override { return 0.5f; }

        private:
            const Def& d_;
            MemPort&   port_;
        };

        // ---- the overlay, as a product computes it from its routes (FSynth 02 §14.3) ------------------------------

        float clamp01(float v) { return v <= 0.0f ? 0.0f : (v >= 1.0f ? 1.0f : v); }

        // A stepped role snaps a track position to the nearest detent centre, (i + 0.5) / n.
        float snap(float t, int n)
        {
            if (n <= 0)
                return t;
            int i = static_cast<int>(std::floor(t * static_cast<float>(n)));
            i = i < 0 ? 0 : (i > n - 1 ? n - 1 : i);
            return (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
        }

        struct Target
        {
            Def        def;
            MemPort    port;
            Model      model;
            ModOverlay overlay;
            uint64_t   revision = 0;

            Target(const Def& d) : def(d), port(d.label, d.init), model(def, port) { recompute(); }

            int detents() const { return def.state == ValueState::stepped ? static_cast<int>(def.detents.size()) : 0; }

            float base() const
            {
                ValueView v;
                model.view(v);
                return v.track;
            }

            void recompute()
            {
                const float b = base();
                float lo = b, hi = b;
                for (const float d : def.depths)
                {
                    lo += d < 0.0f ? d : 0.0f;
                    hi += d > 0.0f ? d : 0.0f;
                }
                overlay.routes = static_cast<int>(def.depths.size());
                overlay.lo = snap(clamp01(lo), detents());
                overlay.hi = snap(clamp01(hi), detents());
                overlay.live = def.live;
                overlay.liveValid = def.liveValid;
                overlay.inert = def.inert;
                overlay.editing = def.edited >= 0 && def.edited < overlay.routes;
                if (overlay.editing)
                {
                    const float d = def.depths[static_cast<size_t>(def.edited)];
                    overlay.editLo = snap(clamp01(b + (d < 0.0f ? d : 0.0f)), detents());
                    overlay.editHi = snap(clamp01(b + (d > 0.0f ? d : 0.0f)), detents());
                }
                overlay.key = ++revision;
            }
        };

        struct DialTarget : Target
        {
            DialTarget(const Def& d, DialGeom g, uint32_t id) : Target(d), dial(model, g, id)
            {
                dial.setMod(def.overlay ? &overlay : nullptr);
            }
            Dial dial;
        };

        struct SliderTarget : Target
        {
            SliderTarget(const Def& d, SlotGeom g, uint32_t id) : Target(d), slider(model, g, id)
            {
                slider.setMod(def.overlay ? &overlay : nullptr);
            }
            RuleSlider slider;
        };

        std::vector<Detent> steps5()
        {
            return { { 0.0f, "1", "one" }, { 0.25f, "2", "two" }, { 0.5f, "3", "three" }, { 0.75f, "4", "four" },
                     { 1.0f, "5", "five" } };
        }

        constexpr float kLeft = 10.0f, kPitch = 60.0f, kRow1 = 4.0f, kRow2 = 104.0f, kFooter = 186.0f;
        constexpr float colX(int c) { return kLeft + kPitch * static_cast<float>(c); }

        // ---- the section ------------------------------------------------------------------------------------------

        class ModOverlaySection final : public Section
        {
        public:
            ModOverlaySection() : Section(440, 200)
            {
                uint32_t id = 200;
                Def d;
                d = {}; d.label = "LIVE DOT"; d.init = 0.5f; d.depths = { 0.25f }; d.liveValid = true; d.live = 0.625f;
                dial(d, colX(0), kRow1, id++);
                d = {}; d.label = "STALE"; d.init = 0.375f; d.depths = { 0.25f, -0.25f, 0.1f };
                dial(d, colX(1), kRow1, id++);
                d = {}; d.state = ValueState::locked; d.label = "INERT"; d.tag = "FIXED"; d.unit = "MS";
                d.reason = "HELD BY THE FIXTURE"; d.fixedValue = "20"; d.init = 0.4f; d.depths = { 0.3f };
                d.inert = true; d.liveValid = true; d.live = 0.55f;
                dial(d, colX(2), kRow1, id++);
                d = {}; d.state = ValueState::stepped; d.label = "SNAPPED"; d.detents = steps5(); d.init = 0.25f;
                d.depths = { 0.2f, 0.25f }; d.liveValid = true; d.live = 0.5f;
                dial(d, colX(3), kRow1, id++);
                d = {}; d.label = "BIPOLAR"; d.lo = -50.0f; d.hi = 50.0f; d.plus = true; d.bipolar = true; d.init = 0.5f;
                d.depths = { 0.25f, -0.125f }; d.liveValid = true; d.live = 0.625f;
                dial(d, colX(4), kRow1, id++);
                d = {}; d.label = "0 ROUTES"; d.init = 0.25f; d.liveValid = true; d.live = 0.5f;   // no routes
                dial(d, colX(5), kRow1, id++);
                d = {}; d.label = "DEPTH 0"; d.init = 0.75f; d.depths = { 0.0f };
                dial(d, colX(6), kRow1, id++);

                d = {}; d.label = "LIVE TICK"; d.init = 0.5f; d.depths = { -0.375f }; d.liveValid = true; d.live = 0.25f;
                slider(d, { 16.0f, kRow2, 176.0f, SlotSize::primary }, id++);
                d = {}; d.label = "EDITED"; d.init = 0.375f; d.depths = { 0.25f, 0.125f, -0.25f }; d.edited = 1;
                slider(d, { 224.0f, kRow2, 136.0f, SlotSize::secondary }, id++);
                d = {}; d.label = "COMPACT"; d.lo = -50.0f; d.hi = 50.0f; d.plus = true; d.bipolar = true; d.init = 0.5f;
                d.depths = { 0.25f, -0.25f }; d.liveValid = true; d.live = 0.375f;
                dial(d, 380.0f, kRow2, id++, DialSize::compact);
            }

            void tick(float dt) override
            {
                for (size_t i = 0; i < dials_.size(); ++i)
                {
                    Dial& d = dials_[i]->dial;
                    d.tick(dt, hasPointer_ && d.contains(pointer_), false, false);
                }
                for (auto& s : sliders_)
                    s->slider.tick(dt, hasPointer_ && s->slider.contains(pointer_), false, false);
                dirty_ = false;
            }

            bool wantsFullRate() const override
            {
                if (dirty_)
                    return true;
                for (const auto& d : dials_)
                    if (!d->dial.settled())
                        return true;
                for (const auto& s : sliders_)
                    if (!s->slider.settled())
                        return true;
                return false;
            }

            void pointerMove(const PointerEvent& e) override
            {
                pointer_ = { e.x, e.y };
                hasPointer_ = true;
                dirty_ = true;
            }

            void pointerExit() override
            {
                hasPointer_ = false;
                dirty_ = true;
            }

            // A press on a dial's ring with routes starts a depth edit of its active route (the last one); anywhere
            // else on a dial or a slider it is the control's own press.
            void pointerDown(const PointerEvent& e) override
            {
                const Point p{ e.x, e.y };
                for (size_t i = 0; i < dials_.size(); ++i)
                {
                    DialTarget& t = *dials_[i];
                    if (t.overlay.routes > 0 && t.dial.ringContains(p))
                    {
                        editing_ = static_cast<int>(i);
                        editFrom_ = p;
                        active_ = t.overlay.routes - 1;
                        editDepth0_ = t.def.depths[static_cast<size_t>(active_)];
                        t.def.edited = active_;
                        t.recompute();
                        dirty_ = true;
                        return;
                    }
                }
                pressedDial_ = dialAt(p);
                if (pressedDial_ >= 0)
                    dials_[static_cast<size_t>(pressedDial_)]->dial.pointerDown(e, *gestures(), *host());
                else if ((pressedSlider_ = sliderAt(p)) >= 0)
                    sliders_[static_cast<size_t>(pressedSlider_)]->slider.pointerDown(e, *gestures(), *host());
                dirty_ = true;
            }

            void pointerDrag(const PointerEvent& e) override
            {
                pointer_ = { e.x, e.y };
                if (editing_ >= 0)
                {
                    // 120 px of travel (dx - dy) from 0 to 1, as FSynth 02 §14.4.
                    DialTarget& t = *dials_[static_cast<size_t>(editing_)];
                    const float travel = (e.x - editFrom_.x) + (editFrom_.y - e.y);
                    float depth = editDepth0_ + travel / 120.0f;
                    depth = depth < -1.0f ? -1.0f : (depth > 1.0f ? 1.0f : depth);
                    t.def.depths[static_cast<size_t>(active_)] = depth;
                    t.recompute();
                }
                else if (pressedDial_ >= 0)
                    dials_[static_cast<size_t>(pressedDial_)]->dial.pointerDrag(e, *gestures());
                else if (pressedSlider_ >= 0)
                    sliders_[static_cast<size_t>(pressedSlider_)]->slider.pointerDrag(e, *gestures());
                dirty_ = true;
            }

            void pointerUp(const PointerEvent& e) override
            {
                if (editing_ >= 0)
                {
                    DialTarget& t = *dials_[static_cast<size_t>(editing_)];
                    t.def.edited = -1;
                    t.recompute();
                    editing_ = -1;
                }
                else if (pressedDial_ >= 0)
                    dials_[static_cast<size_t>(pressedDial_)]->dial.pointerUp(e, *gestures());
                else if (pressedSlider_ >= 0)
                    sliders_[static_cast<size_t>(pressedSlider_)]->slider.pointerUp(e, *gestures());
                pressedDial_ = pressedSlider_ = -1;
                dirty_ = true;
            }

            Cursor cursor() const override
            {
                if (!hasPointer_)
                    return Cursor::normal;
                if (editing_ >= 0)
                    return Cursor::grabbing;
                const int d = dialAt(pointer_);
                if (d >= 0)
                    return dials_[static_cast<size_t>(d)]->dial.cursorAt(pointer_);
                for (const auto& s : sliders_)
                    if (s->slider.contains(pointer_) || s->slider.bandContains(pointer_))
                        return s->slider.cursorAt(pointer_);
                return Cursor::normal;
            }

            void accessibility(std::vector<A11yItem>& out) const override
            {
                for (const auto& d : dials_)
                {
                    A11yItem item;
                    d->dial.accessibility(item);
                    out.push_back(item);
                }
                for (const auto& s : sliders_)
                {
                    A11yItem item;
                    s->slider.accessibility(item);
                    out.push_back(item);
                }
            }

            void draw(Canvas& c, const Theme& th) override
            {
                for (const auto& d : dials_)
                    d->dial.draw(c, th, false);
                for (const auto& s : sliders_)
                    s->slider.draw(c, th, false);
                if (editing_ >= 0)
                {
                    const DialTarget& t = *dials_[static_cast<size_t>(editing_)];
                    char depth[20], line[96];
                    fmt::db(t.def.depths[static_cast<size_t>(active_)], 2, depth, sizeof depth);
                    const bool plus = t.def.depths[static_cast<size_t>(active_)] > 0.0f;
                    std::snprintf(line, sizeof line, "ROUTE %d \xC2\xB7 DEPTH %s%s \xC2\xB7 %d ROUTES", active_ + 1,
                                  plus ? "+" : "", depth, t.overlay.routes);
                    const Canvas::Scope s(c, tags::hint, false);
                    c.text(line, kLeft, kFooter, type::kMicro, th.ink32);
                }
            }

        private:
            void dial(const Def& d, float x, float top, uint32_t id, DialSize size = DialSize::primary)
            {
                dials_.push_back(std::make_unique<DialTarget>(d, DialGeom{ x, top, size }, id));
            }

            void slider(const Def& d, SlotGeom g, uint32_t id)
            {
                sliders_.push_back(std::make_unique<SliderTarget>(d, g, id));
            }

            int dialAt(Point p) const
            {
                for (size_t i = 0; i < dials_.size(); ++i)
                    if (dials_[i]->dial.contains(p))
                        return static_cast<int>(i);
                return -1;
            }

            int sliderAt(Point p) const
            {
                for (size_t i = 0; i < sliders_.size(); ++i)
                    if (sliders_[i]->slider.contains(p))
                        return static_cast<int>(i);
                return -1;
            }

            std::vector<std::unique_ptr<DialTarget>>   dials_;
            std::vector<std::unique_ptr<SliderTarget>> sliders_;
            Point pointer_{}, editFrom_{};
            bool  hasPointer_ = false, dirty_ = false;
            int   pressedDial_ = -1, pressedSlider_ = -1;
            int   editing_ = -1, active_ = -1;           // the dial whose depth is being edited, and its route
            float editDepth0_ = 0.0f;
        };

        // STALE's ring: its centre is (colX(1) + 30, kRow1 + 34) and the ring's radius 20, so (x, kRow1 + 14) is on it.
        const Registration kModOverlay{ SectionInfo{
            "modoverlay",
            [] { return std::make_unique<ModOverlaySection>(); },
            {
                State{ "rest", {} },
                State{ "editing",
                       [](HeadlessHost& h, Panel&) {
                           h.press(colX(1) + 30.0f, kRow1 + 14.0f);
                           h.moveTo(colX(1) + 60.0f, kRow1 + 14.0f, 3);   // 30 px of travel: depth +0.25
                       } },
            } } };
    }
}

// FUNKGUI_TEST name=fg.gallery.ruleslider timeout=600 gpu=0 exe=FunkGuiGalleryProbe args="--section ruleslider"
//
// The "ruleslider" gallery section (FCompressor docs/design/02-funkgui-and-ui.md §3.11, §5.4, §8.1–§8.3; G5):
// RuleSlider in all five states on 02 §6.1's slot grid (7 columns of 112 px on a 128 px pitch; two primary rows and a
// secondary row) with the markers and the fit-rule cases of §8.3 as FCompressor's first Modes use them:
//   row A  THRESHOLD continuous · RATIO stepped 2·4·10 · KNEE derived = RATIO · ATTACK locked ~10 MS (dotted) ·
//          RANGE n/a · ATTACK hybrid (.02–.8 ms + OFF end cell) · the same hybrid on its OFF cell
//   row B  INPUT renamed (THRESHOLD) · MIX extension + · ATTACK clamped · FET RATIO 4·8·12·20·ALL (labels) ·
//          VOICE OFF·TUBE·DIODE·BRIGHT (ticks only) · Mu 67 DRIVE, 21 detents (ticks only) · MAKEUP bipolar with soft
//          notches
//   row C  secondary: RANGE · DETECT PEAK·RMS·PK+RMS · LOOKAHEAD locked (budget off) · HOLD n/a · RELEASE
//          .1·.3·.6·1.2·AUTO · S2 RELEASE derived = TIME · SC HPF hybrid (OFF + 20–500 Hz)
// and a footer with the spec line of the slot under the pointer, pressed, focused or last touched (the reason, for a
// refused gesture). States: rest, hover (track fill and accent), hover-label (a detent label ink70), drag (a
// continuous drag), ghost (a stepped drag held open: the ghost caret), wheel (one notch on a ticks-only slot), keys
// (Tab to RATIO, → one detent, focus ring), refused (a click on the locked slot), hybrid-cell (Home on the hybrid:
// the OFF cell). Every value is exact arithmetic on host-normalised values, so the fingerprint is arch-neutral. The
// line above registers the test; the tools glob compiles this file into FunkGuiGalleryProbe.

#include "GalleryPanel.h"

#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Ease.h>
#include <funkgui/core/Format.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/params/ParamPort.h>
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

        // One slot as a product would describe it. Continuous tracks map host01 [hostLo, hostHi] onto the plain range
        // [lo, hi]; end cells and detents carry their own host01; locked, derived and n/a views are fixed text.
        struct Def
        {
            SlotGeom    geom;
            ValueState  state = ValueState::continuous;
            const char* label = "";
            const char* aka = nullptr;
            const char* tag = nullptr;
            const char* reason = nullptr;
            float       lo = 0.0f, hi = 1.0f;            // plain range (continuous)
            float       hostLo = 0.0f, hostHi = 1.0f;    // its host01 span (hybrid: the cells lie outside)
            int         dp = 1;
            bool        plus = false;                    // "+4.0"
            const char* unit = "";
            const char* sub = nullptr;
            bool        liveSub = false;
            std::vector<Detent> detents, endLo;
            std::vector<float>  notches;
            float       init = 0.5f, def = 0.5f;         // host01
            bool        bipolar = false, clamped = false;
            const char* fixedValue = "";                 // locked / derived / n/a
            float       fixedTrack = 0.0f;
            const char* valueOverride = nullptr;         // clamped: the effective value's text
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
                v.aka = d_.aka;
                v.tag = d_.tag;
                v.reason = d_.reason;
                v.bipolar = d_.bipolar;
                v.clamped = d_.clamped;
                v.live = d_.liveSub;
                v.trackDefault = trackOf(d_.def);
                v.atDefault = ease::sameBits(h, d_.def);
                v.nNotches = static_cast<int>(d_.notches.size());
                v.notches = d_.notches.empty() ? nullptr : d_.notches.data();
                if (d_.sub != nullptr)
                    std::snprintf(v.text.sub, sizeof v.text.sub, "%s", d_.sub);
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
                    const Detent& det = d_.detents[static_cast<size_t>(best)];
                    std::snprintf(v.text.value, sizeof v.text.value, "%s", det.label);
                    const char c0 = det.label[0];
                    const bool numeric = (c0 >= '0' && c0 <= '9') || c0 == '.' || c0 == '\xE2';   // digits or U+2212
                    std::snprintf(v.text.unit, sizeof v.text.unit, "%s", numeric ? d_.unit : "");
                    std::snprintf(v.text.spoken, sizeof v.text.spoken, "%s", det.spoken);
                    return;
                }
                if (d_.state != ValueState::continuous)
                {
                    v.track = d_.fixedTrack;
                    std::snprintf(v.text.value, sizeof v.text.value, "%s", d_.fixedValue);
                    std::snprintf(v.text.unit, sizeof v.text.unit, "%s", d_.unit);
                    return;
                }
                v.nEndLo = static_cast<int>(d_.endLo.size());
                v.endLo = d_.endLo.empty() ? nullptr : d_.endLo.data();
                for (size_t i = 0; i < d_.endLo.size(); ++i)
                    if (ease::sameBits(d_.endLo[i].host01, h))
                        v.activeEnd = -static_cast<int>(i) - 1;
                v.track = trackOf(h);
                if (v.activeEnd != 0)
                {
                    const Detent& cell = d_.endLo[static_cast<size_t>(-v.activeEnd - 1)];
                    std::snprintf(v.text.value, sizeof v.text.value, "%s", cell.label);
                    std::snprintf(v.text.spoken, sizeof v.text.spoken, "%s", cell.spoken);
                    return;
                }
                if (d_.valueOverride != nullptr)
                    std::snprintf(v.text.value, sizeof v.text.value, "%s", d_.valueOverride);
                else
                {
                    const float plain = d_.lo + v.track * (d_.hi - d_.lo);
                    char num[20];
                    fmt::db(plain, d_.dp, num, sizeof num);
                    std::snprintf(v.text.value, sizeof v.text.value, "%s%s", d_.plus && plain > 0.0f ? "+" : "", num);
                }
                std::snprintf(v.text.unit, sizeof v.text.unit, "%s", d_.unit);
            }

            ParamPort* port() override { return &port_; }
            float host01FromTrack(float t) const override { return d_.hostLo + t * (d_.hostHi - d_.hostLo); }
            float defaultHost01() const override { return d_.def; }

        private:
            float trackOf(float h) const
            {
                const float t = (h - d_.hostLo) / (d_.hostHi - d_.hostLo);
                return t <= 0.0f ? 0.0f : (t >= 1.0f ? 1.0f : t);
            }

            const Def& d_;
            MemPort&   port_;
        };

        // ---- the slots --------------------------------------------------------------------------------------------

        constexpr float kCol = 128.0f, kLeft = 16.0f, kW = 112.0f;
        constexpr float kRowA = 24.0f, kRowB = 120.0f, kRowC = 216.0f, kFooter = 296.0f;

        SlotGeom at(int col, float top, SlotSize size = SlotSize::primary)
        {
            return { kLeft + kCol * static_cast<float>(col), top, kW, size };
        }

        std::vector<Detent> steps(std::initializer_list<const char*> names, std::initializer_list<const char*> spoken)
        {
            std::vector<Detent> d;
            const float n = static_cast<float>(names.size());
            auto s = spoken.begin();
            float i = 0.0f;
            for (const char* name : names)
            {
                d.push_back({ n > 1.0f ? i / (n - 1.0f) : 0.0f, name, s != spoken.end() ? *s++ : name });
                i += 1.0f;
            }
            return d;
        }

        std::vector<Def> slots()
        {
            std::vector<Def> v;
            Def d;
            // row A: the five states and the hybrid
            d = {}; d.geom = at(0, kRowA); d.label = "THRESHOLD"; d.lo = -60.0f; d.hi = 0.0f; d.unit = "DB";
            d.sub = "DET \xE2\x88\x92" "14.2"; d.liveSub = true; d.init = 0.7f; d.def = 0.75f;
            v.push_back(d);
            d = {}; d.geom = at(1, kRowA); d.state = ValueState::stepped; d.label = "RATIO"; d.unit = ":1";
            d.detents = steps({ "2", "4", "10" }, { "2 to 1", "4 to 1", "10 to 1" }); d.init = 0.5f; d.def = 0.5f;
            v.push_back(d);
            d = {}; d.geom = at(2, kRowA); d.state = ValueState::derived; d.label = "KNEE"; d.tag = "= RATIO";
            d.reason = "THE KNEE FOLLOWS THE RATIO SWITCH"; d.fixedValue = "6.0"; d.unit = "DB";
            d.sub = "FOLLOWS RATIO"; d.fixedTrack = 0.3f;
            v.push_back(d);
            d = {}; d.geom = at(3, kRowA); d.state = ValueState::locked; d.label = "ATTACK"; d.tag = "FIXED";
            d.reason = "THE OPTICAL CELL SETS ITS OWN ATTACK"; d.fixedValue = "~10"; d.unit = "MS";
            d.sub = "T4 CELL \xC2\xB7 PROGRAM"; d.fixedTrack = 0.35f;
            v.push_back(d);
            d = {}; d.geom = at(4, kRowA); d.state = ValueState::na; d.label = "RANGE";
            d.reason = "NO RANGE CONTROL ON THIS UNIT";
            v.push_back(d);
            d = {}; d.geom = at(5, kRowA); d.label = "ATTACK"; d.lo = 20.0f; d.hi = 800.0f; d.dp = 0;
            d.unit = "\xC2\xB5S"; d.hostLo = 0.1f; d.hostHi = 1.0f; d.sub = "DIAL 4.2";
            d.endLo = { { 0.0f, "OFF", "Off" } }; d.init = 0.3f; d.def = 0.5f;
            v.push_back(d);
            d.geom = at(6, kRowA); d.init = 0.0f;
            v.push_back(d);
            // row B: markers and the fit rule
            d = {}; d.geom = at(0, kRowB); d.label = "INPUT"; d.aka = "THRESHOLD"; d.lo = 0.0f; d.hi = 48.0f;
            d.dp = 0; d.unit = "DB"; d.sub = "THRESHOLD \xE2\x88\x92" "28 DB";
            d.reason = "DRIVES A FIXED \xE2\x88\x92" "12 DBFS THRESHOLD"; d.init = 0.625f; d.def = 0.5f;
            v.push_back(d);
            d = {}; d.geom = at(1, kRowB); d.label = "MIX"; d.tag = "+"; d.lo = 0.0f; d.hi = 100.0f; d.dp = 0;
            d.unit = "%"; d.init = 1.0f; d.def = 1.0f;
            v.push_back(d);
            d = {}; d.geom = at(2, kRowB); d.label = "ATTACK"; d.lo = 20.0f; d.hi = 800.0f; d.dp = 0;
            d.unit = "\xC2\xB5S"; d.clamped = true; d.valueOverride = "20"; d.sub = "CLAMPED FROM 5 \xC2\xB5S";
            d.init = 0.0f; d.def = 0.5f;
            v.push_back(d);
            d = {}; d.geom = at(3, kRowB); d.state = ValueState::stepped; d.label = "RATIO"; d.unit = ":1";
            d.detents = steps({ "4", "8", "12", "20", "ALL" }, { "4 to 1", "8 to 1", "12 to 1", "20 to 1", "all buttons" });
            d.init = 1.0f; d.def = 0.0f;
            v.push_back(d);
            d = {}; d.geom = at(4, kRowB); d.state = ValueState::stepped; d.label = "VOICE";
            d.detents = steps({ "OFF", "TUBE", "DIODE", "BRIGHT" }, { "off", "tube", "diode", "bright" });
            d.init = 1.0f / 3.0f; d.def = 0.0f;
            v.push_back(d);
            {
                static const char* names[21] = { "\xE2\x88\x92" "20", "\xE2\x88\x92" "19", "\xE2\x88\x92" "18",
                    "\xE2\x88\x92" "17", "\xE2\x88\x92" "16", "\xE2\x88\x92" "15", "\xE2\x88\x92" "14",
                    "\xE2\x88\x92" "13", "\xE2\x88\x92" "12", "\xE2\x88\x92" "11", "\xE2\x88\x92" "10",
                    "\xE2\x88\x92" "9", "\xE2\x88\x92" "8", "\xE2\x88\x92" "7", "\xE2\x88\x92" "6", "\xE2\x88\x92" "5",
                    "\xE2\x88\x92" "4", "\xE2\x88\x92" "3", "\xE2\x88\x92" "2", "\xE2\x88\x92" "1", "0" };
                d = {}; d.geom = at(5, kRowB); d.state = ValueState::stepped; d.label = "DRIVE"; d.unit = "DB";
                for (int i = 0; i < 21; ++i)
                    d.detents.push_back({ static_cast<float>(i) / 20.0f, names[i], names[i] });
                d.init = 14.0f / 20.0f; d.def = 1.0f;
                v.push_back(d);
            }
            d = {}; d.geom = at(6, kRowB); d.label = "MAKEUP"; d.lo = -24.0f; d.hi = 24.0f; d.plus = true;
            d.unit = "DB"; d.bipolar = true; d.notches = { 0.375f, 0.625f }; d.init = 0.583333313f; d.def = 0.5f;
            v.push_back(d);
            // row C: secondary slots
            const SlotSize s2 = SlotSize::secondary;
            d = {}; d.geom = at(0, kRowC, s2); d.label = "RANGE"; d.lo = 0.0f; d.hi = 60.0f; d.dp = 0; d.unit = "DB";
            d.init = 2.0f / 3.0f; d.def = 1.0f;
            v.push_back(d);
            d = {}; d.geom = at(1, kRowC, s2); d.state = ValueState::stepped; d.label = "DETECT";
            d.detents = steps({ "PEAK", "RMS", "PK+RMS" }, { "peak", "RMS", "peak plus RMS" }); d.init = 0.5f;
            v.push_back(d);
            d = {}; d.geom = at(2, kRowC, s2); d.state = ValueState::locked; d.label = "LOOKAHEAD"; d.tag = "FIXED";
            d.reason = "LOOKAHEAD BUDGET IS OFF \xE2\x80\x94 SET 5 MS OR 20 MS (ADDS LATENCY)"; d.fixedValue = "0";
            d.unit = "MS"; d.sub = "BUDGET OFF"; d.fixedTrack = 0.0f;
            v.push_back(d);
            d = {}; d.geom = at(3, kRowC, s2); d.state = ValueState::na; d.label = "HOLD";
            d.reason = "NO HOLD STAGE IN THIS MODE";
            v.push_back(d);
            d = {}; d.geom = at(4, kRowC, s2); d.state = ValueState::stepped; d.label = "RELEASE"; d.unit = "S";
            d.detents = steps({ ".1", ".3", ".6", "1.2", "AUTO" }, { "0.1 seconds", "0.3 seconds", "0.6 seconds",
                                                                  "1.2 seconds", "auto" });
            d.init = 0.25f; d.def = 0.25f;
            v.push_back(d);
            d = {}; d.geom = at(5, kRowC, s2); d.state = ValueState::derived; d.label = "S2 RELEASE"; d.tag = "= TIME";
            d.reason = "STAGE 2 RELEASE FOLLOWS THE TIME SWITCH"; d.fixedValue = "2.0"; d.unit = "S";
            d.sub = "EFF 2.4 S"; d.fixedTrack = 0.55f;
            v.push_back(d);
            d = {}; d.geom = at(6, kRowC, s2); d.label = "SC HPF"; d.lo = 20.0f; d.hi = 500.0f; d.dp = 0; d.unit = "HZ";
            d.hostLo = 0.1f; d.hostHi = 1.0f; d.endLo = { { 0.0f, "OFF", "Off" } }; d.init = 0.3f; d.def = 0.0f;
            v.push_back(d);
            return v;
        }

        // ---- the section ------------------------------------------------------------------------------------------

        struct Entry
        {
            Entry(const Def& d, uint32_t id) : def(d), port(d.label, d.init), model(def, port), slider(model, d.geom, id)
            {
            }
            Def        def;
            MemPort    port;
            Model      model;
            RuleSlider slider;
        };

        class RuleSliderSection final : public Section
        {
        public:
            RuleSliderSection() : Section(912, 320)
            {
                uint32_t id = 100;
                for (const Def& d : slots())
                    entries_.push_back(std::make_unique<Entry>(d, id++));
            }

            void tick(float dt) override
            {
                for (size_t i = 0; i < entries_.size(); ++i)
                {
                    RuleSlider& s = entries_[i]->slider;
                    const bool hovered = hasPointer_ && s.contains(pointer_);
                    s.tick(dt, hovered, ring_ && focus_ == static_cast<int>(i), false);
                }
                dirty_ = false;
            }

            bool wantsFullRate() const override
            {
                if (dirty_)
                    return true;
                for (const auto& e : entries_)
                    if (!e->slider.settled())
                        return true;
                return false;
            }

            void pointerMove(const PointerEvent& e) override
            {
                pointer_ = { e.x, e.y };
                hasPointer_ = true;
                for (auto& en : entries_)
                {
                    if (en->slider.contains(pointer_))
                        en->slider.pointerMove(e);
                    else
                        en->slider.pointerExit();
                }
                dirty_ = true;
            }

            void pointerExit() override
            {
                hasPointer_ = false;
                for (auto& en : entries_)
                    en->slider.pointerExit();
                dirty_ = true;
            }

            void pointerDown(const PointerEvent& e) override
            {
                ring_ = false;                           // the pointer is the affordance now (HR)
                pressed_ = at({ e.x, e.y });
                if (pressed_ >= 0)
                {
                    touched_ = pressed_;
                    entries_[static_cast<size_t>(pressed_)]->slider.pointerDown(e, *gestures(), *host());
                }
                dirty_ = true;
            }

            void pointerDrag(const PointerEvent& e) override
            {
                if (pressed_ >= 0)
                    entries_[static_cast<size_t>(pressed_)]->slider.pointerDrag(e, *gestures());
                dirty_ = true;
            }

            void pointerUp(const PointerEvent& e) override
            {
                if (pressed_ >= 0)
                    entries_[static_cast<size_t>(pressed_)]->slider.pointerUp(e, *gestures());
                pressed_ = -1;
                dirty_ = true;
            }

            void doubleClick(const PointerEvent& e) override
            {
                const int i = at({ e.x, e.y });
                if (i >= 0)
                    entries_[static_cast<size_t>(i)]->slider.doubleClick(*gestures());
                dirty_ = true;
            }

            bool wheel(const WheelEvent& e) override
            {
                const int i = at({ e.x, e.y });
                dirty_ = true;
                if (i < 0)
                    return false;
                touched_ = i;
                return entries_[static_cast<size_t>(i)]->slider.wheel(e, *gestures(), host()->nowSeconds());
            }

            bool key(const KeyEvent& e) override
            {
                dirty_ = true;
                const int n = static_cast<int>(entries_.size());
                if (e.key == Key::tab)
                {
                    const int dir = e.mods.shift ? -1 : 1;
                    focus_ = focus_ < 0 ? (dir > 0 ? 0 : n - 1) : (focus_ + dir + n) % n;
                    ring_ = true;
                    touched_ = focus_;
                    return true;
                }
                if (e.key == Key::escape && ring_)
                {
                    ring_ = false;
                    return true;
                }
                if (focus_ < 0)
                    return false;
                return entries_[static_cast<size_t>(focus_)]->slider.key(e, *gestures());
            }

            Cursor cursor() const override
            {
                const int i = hasPointer_ ? at(pointer_) : -1;
                return i >= 0 ? entries_[static_cast<size_t>(i)]->slider.cursorAt(pointer_) : Cursor::normal;
            }

            void accessibility(std::vector<A11yItem>& out) const override
            {
                for (const auto& e : entries_)
                {
                    A11yItem item;
                    e->slider.accessibility(item);
                    out.push_back(item);
                }
            }

            void a11yAction(uint32_t id, A11yAction a, double value) override
            {
                for (auto& e : entries_)
                    if (e->slider.a11yId() == id)
                        e->slider.a11yAction(a, value, *gestures());
                dirty_ = true;
            }

            void draw(Canvas& c, const Theme& th) override
            {
                for (size_t i = 0; i < entries_.size(); ++i)
                    entries_[i]->slider.draw(c, th, ring_ && focus_ == static_cast<int>(i));
                int src = pressed_;
                if (src < 0 && hasPointer_)
                    src = at(pointer_);
                if (src < 0)
                    src = ring_ ? focus_ : touched_;
                if (src >= 0)
                {
                    char line[256];
                    entries_[static_cast<size_t>(src)]->slider.specLine(line, sizeof line);
                    const Canvas::Scope s(c, tags::hint, false);
                    c.text(line, kLeft, kFooter, type::kLabel, th.ink32);
                }
            }

        private:
            int at(Point p) const
            {
                for (size_t i = 0; i < entries_.size(); ++i)
                    if (entries_[i]->slider.contains(p))
                        return static_cast<int>(i);
                return -1;
            }

            std::vector<std::unique_ptr<Entry>> entries_;
            Point pointer_{};
            bool  hasPointer_ = false, ring_ = false, dirty_ = false;
            int   pressed_ = -1, focus_ = -1, touched_ = -1;
        };

        // Script points (02 §6.1 slot anatomy: label top, value +18, detent line +46, track +64).
        constexpr float colX(int c) { return kLeft + kCol * static_cast<float>(c); }

        const Registration kRuleSlider{ SectionInfo{
            "ruleslider",
            [] { return std::make_unique<RuleSliderSection>(); },
            {
                State{ "rest", {} },
                State{ "hover", [](HeadlessHost& h, Panel&) { h.move(colX(1) + 30.0f, kRowA + 30.0f); } },
                State{ "hover-label",
                       [](HeadlessHost& h, Panel&) { h.move(colX(1) + 112.0f * 5.0f / 6.0f, kRowA + 50.0f); } },
                State{ "drag",
                       [](HeadlessHost& h, Panel&) {
                           h.drag(colX(0) + 40.0f, kRowA + 30.0f, colX(0) + 76.0f, kRowA + 30.0f);
                       } },
                State{ "ghost",
                       [](HeadlessHost&, Panel& p) {
                           PointerEvent e;
                           e.x = colX(3) + 90.0f;
                           e.y = kRowB + 30.0f;
                           p.pointerMove(e);
                           p.pointerDown(e);
                           e.x -= 24.0f;                 // FET ratio: pitch 60, commit 36 -> held, ghost shown
                           p.pointerDrag(e);
                       } },
                State{ "wheel", [](HeadlessHost& h, Panel&) { h.wheel(colX(4) + 40.0f, kRowB + 30.0f, 0.25f); } },
                State{ "keys", [](HeadlessHost& h, Panel&) { h.keys("tab,tab,right"); } },
                State{ "refused", [](HeadlessHost& h, Panel&) { h.click(colX(3) + 40.0f, kRowA + 30.0f); } },
                State{ "hybrid-cell", [](HeadlessHost& h, Panel&) { h.keys("tab,tab,tab,tab,tab,tab,home"); } },
            } } };
    }
}

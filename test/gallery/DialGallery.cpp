// FUNKGUI_TEST name=fg.gallery.dial timeout=600 gpu=0 exe=FunkGuiGalleryProbe args="--section dial"
//
// The "dial" gallery section (FSynth G2; FSynth docs/design/02-funkgui-and-ui.md §4.2, §14.1): Dial in all five states
// and its variants, as a product's inspector grid lays them out (60 x 80 cells, 4 px gutters inside each). Each dial is
// labelled with what it shows:
//   row 1  VALUE continuous · STEPS 3 stepped LO·MID·HI · LOCKED (dotted, FIXED) · DERIVED (= VALUE, in its LINKED
//          box) · N/A · HYBRID on its OFF end cell · BIPOLAR (from 12:00)
//   row 2  STEPS 21, stepped (no ticks: a cell is under 4 px of arc) · four compact dials (VALUE, STEPS 5, LOCKED,
//          BIPOLAR) · IN RANGE, a hybrid inside its range with a low and a high end cell · DEFAULT, continuous at its
//          default (the value in ink32)
// and a footer with the spec line of the dial under the pointer, pressed, focused or last touched (the reason, for a
// refused gesture), cut to whole fields that fit. States: rest; hover (VALUE under the pointer: the accentDim fill,
// the value arc and the value in accent); drag (VALUE dragged 36 px up: +0.15); ghost (a stepped drag on STEPS 3
// held open 24 px short of its commit: the ghost pointer); keys (Tab to STEPS 3, → one detent, the focus ring);
// refused (a click on LOCKED: no write, the reason in the footer). Every value is exact arithmetic on host-normalised
// values, so the fingerprint is arch-neutral. Its text names the states shown, never a product's Modes or
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

        // One dial as a product would describe it. Continuous tracks map host01 [hostLo, hostHi] onto the plain range
        // [lo, hi]; end cells and detents carry their own host01; locked, derived and n/a views are fixed text.
        struct Def
        {
            DialGeom    geom;
            ValueState  state = ValueState::continuous;
            const char* label = "";
            const char* tag = nullptr;
            const char* reason = nullptr;
            float       lo = 0.0f, hi = 100.0f;          // plain range (continuous)
            float       hostLo = 0.0f, hostHi = 1.0f;    // its host01 span (hybrid: the cells lie outside)
            int         dp = 0;
            bool        plus = false;                    // "+20"
            const char* unit = "";
            std::vector<Detent> detents, endLo, endHi;
            float       init = 0.5f, def = 0.5f;         // host01
            bool        bipolar = false;
            const char* fixedValue = "";                 // locked / derived / n/a
            float       fixedTrack = 0.0f;
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
                v.trackDefault = trackOf(d_.def);
                v.atDefault = ease::sameBits(h, d_.def);
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
                    std::snprintf(v.text.unit, sizeof v.text.unit, "%s", d_.unit);
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
                v.nEndHi = static_cast<int>(d_.endHi.size());
                v.endHi = d_.endHi.empty() ? nullptr : d_.endHi.data();
                const Detent* cell = nullptr;
                for (size_t i = 0; i < d_.endLo.size(); ++i)
                    if (ease::sameBits(d_.endLo[i].host01, h))
                    {
                        v.activeEnd = -static_cast<int>(i) - 1;
                        cell = &d_.endLo[i];
                    }
                for (size_t i = 0; i < d_.endHi.size(); ++i)
                    if (ease::sameBits(d_.endHi[i].host01, h))
                    {
                        v.activeEnd = static_cast<int>(i) + 1;
                        cell = &d_.endHi[i];
                    }
                v.track = trackOf(h);
                if (cell != nullptr)
                {
                    std::snprintf(v.text.value, sizeof v.text.value, "%s", cell->label);
                    std::snprintf(v.text.spoken, sizeof v.text.spoken, "%s", cell->spoken);
                    return;
                }
                const float plain = d_.lo + v.track * (d_.hi - d_.lo);
                char num[20];
                fmt::db(plain, d_.dp, num, sizeof num);
                std::snprintf(v.text.value, sizeof v.text.value, "%s%s", d_.plus && plain > 0.0f ? "+" : "", num);
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

        // ---- the dials --------------------------------------------------------------------------------------------

        constexpr float kLeft = 10.0f, kPitch = 60.0f, kRow1 = 4.0f, kRow2 = 88.0f, kFooter = 169.0f;
        constexpr float kFooterW = 420.0f;

        DialGeom cellAt(float x, float top, DialSize size = DialSize::primary) { return { x, top, size }; }
        constexpr float colX(int c) { return kLeft + kPitch * static_cast<float>(c); }

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

        std::vector<Def> dials()
        {
            std::vector<Def> v;
            Def d;
            // row 1: the five states, the hybrid on its end cell, the bipolar dial
            d = {}; d.geom = cellAt(colX(0), kRow1); d.label = "VALUE"; d.unit = "%"; d.init = 0.62f; d.def = 0.5f;
            v.push_back(d);
            d = {}; d.geom = cellAt(colX(1), kRow1); d.state = ValueState::stepped; d.label = "STEPS 3";
            d.detents = steps({ "LO", "MID", "HI" }, { "low", "middle", "high" }); d.init = 0.5f; d.def = 0.5f;
            v.push_back(d);
            d = {}; d.geom = cellAt(colX(2), kRow1); d.state = ValueState::locked; d.label = "LOCKED"; d.tag = "FIXED";
            d.reason = "HELD BY THE FIXTURE"; d.fixedValue = "20"; d.unit = "MS"; d.fixedTrack = 0.4f;
            v.push_back(d);
            d = {}; d.geom = cellAt(colX(3), kRow1); d.state = ValueState::derived; d.label = "DERIVED";
            d.tag = "= VALUE"; d.reason = "FOLLOWS VALUE"; d.fixedValue = "62"; d.unit = "%";
            d.fixedTrack = 0.62f;
            v.push_back(d);
            d = {}; d.geom = cellAt(colX(4), kRow1); d.state = ValueState::na; d.label = "N/A";
            d.reason = "NOT USED HERE";
            v.push_back(d);
            d = {}; d.geom = cellAt(colX(5), kRow1); d.label = "HYBRID"; d.lo = 1.0f; d.hi = 50.0f; d.dp = 1;
            d.unit = "HZ"; d.hostLo = 0.1f; d.hostHi = 1.0f; d.endLo = { { 0.0f, "OFF", "Off" } }; d.init = 0.0f;
            d.def = 0.5f;
            v.push_back(d);
            d = {}; d.geom = cellAt(colX(6), kRow1); d.label = "BIPOLAR"; d.lo = -50.0f; d.hi = 50.0f; d.plus = true;
            d.unit = "%"; d.bipolar = true; d.init = 0.7f; d.def = 0.5f;
            v.push_back(d);
            // row 2: 21 detents, compact dials, a hybrid inside its range, a dial at its default
            {
                static const char* names[21] = { "\xE2\x88\x92" "10", "\xE2\x88\x92" "9", "\xE2\x88\x92" "8",
                    "\xE2\x88\x92" "7", "\xE2\x88\x92" "6", "\xE2\x88\x92" "5", "\xE2\x88\x92" "4", "\xE2\x88\x92" "3",
                    "\xE2\x88\x92" "2", "\xE2\x88\x92" "1", "0", "+1", "+2", "+3", "+4", "+5", "+6", "+7", "+8", "+9",
                    "+10" };
                d = {}; d.geom = cellAt(colX(0), kRow2); d.state = ValueState::stepped; d.label = "STEPS 21";
                for (int i = 0; i < 21; ++i)
                    d.detents.push_back({ static_cast<float>(i) / 20.0f, names[i], names[i] });
                d.init = 0.3f; d.def = 0.5f;
                v.push_back(d);
            }
            const DialSize sc = DialSize::compact;
            d = {}; d.geom = cellAt(76.0f, kRow2 + 4.0f, sc); d.label = "VALUE"; d.unit = "%"; d.init = 0.3f;
            d.def = 0.5f;
            v.push_back(d);
            d = {}; d.geom = cellAt(136.0f, kRow2 + 4.0f, sc); d.state = ValueState::stepped; d.label = "STEPS 5";
            d.detents = steps({ "1", "2", "3", "4", "5" }, { "one", "two", "three", "four", "five" }); d.init = 0.75f;
            d.def = 0.0f;
            v.push_back(d);
            d = {}; d.geom = cellAt(196.0f, kRow2 + 4.0f, sc); d.state = ValueState::locked; d.label = "LOCKED";
            d.tag = "FIXED"; d.reason = "HELD BY THE FIXTURE"; d.fixedValue = "100"; d.unit = "%"; d.fixedTrack = 1.0f;
            v.push_back(d);
            d = {}; d.geom = cellAt(256.0f, kRow2 + 4.0f, sc); d.label = "BIPOLAR"; d.lo = -50.0f; d.hi = 50.0f;
            d.plus = true; d.bipolar = true; d.init = 0.25f; d.def = 0.5f;
            v.push_back(d);
            d = {}; d.geom = cellAt(316.0f, kRow2); d.label = "IN RANGE"; d.lo = 10.0f; d.hi = 90.0f; d.unit = "%";
            d.hostLo = 0.1f; d.hostHi = 0.9f; d.endLo = { { 0.0f, "OFF", "Off" } };
            d.endHi = { { 1.0f, "MAX", "Maximum" } }; d.init = 0.7f; d.def = 0.5f;
            v.push_back(d);
            d = {}; d.geom = cellAt(376.0f, kRow2); d.label = "DEFAULT"; d.unit = "%"; d.init = 0.75f; d.def = 0.75f;
            v.push_back(d);
            return v;
        }

        // ---- the section ------------------------------------------------------------------------------------------

        struct Entry
        {
            Entry(const Def& d, uint32_t id) : def(d), port(d.label, d.init), model(def, port), dial(model, d.geom, id) {}
            Def     def;
            MemPort port;
            Model   model;
            Dial    dial;
        };

        // The longest prefix of whole "   "-separated fields of `line` that fits in `maxW` px of kMicro.
        std::string fitFields(Canvas& c, const char* line, float maxW)
        {
            std::string s = line;
            while (!s.empty() && c.textWidth(s.c_str(), type::kMicro) > maxW)
            {
                const size_t cut = s.rfind("   ");
                if (cut == std::string::npos)
                    return {};
                s.resize(cut);
            }
            return s;
        }

        class DialSection final : public Section
        {
        public:
            DialSection() : Section(440, 180)
            {
                uint32_t id = 100;
                for (const Def& d : dials())
                    entries_.push_back(std::make_unique<Entry>(d, id++));
            }

            void tick(float dt) override
            {
                for (size_t i = 0; i < entries_.size(); ++i)
                {
                    Dial& d = entries_[i]->dial;
                    const bool hovered = hasPointer_ && d.contains(pointer_);
                    d.tick(dt, hovered, ring_ && focus_ == static_cast<int>(i), false);
                }
                dirty_ = false;
            }

            bool wantsFullRate() const override
            {
                if (dirty_)
                    return true;
                for (const auto& e : entries_)
                    if (!e->dial.settled())
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

            void pointerDown(const PointerEvent& e) override
            {
                ring_ = false;                           // the pointer is the affordance now (HR)
                pressed_ = at({ e.x, e.y });
                if (pressed_ >= 0)
                {
                    touched_ = pressed_;
                    entries_[static_cast<size_t>(pressed_)]->dial.pointerDown(e, *gestures(), *host());
                }
                dirty_ = true;
            }

            void pointerDrag(const PointerEvent& e) override
            {
                pointer_ = { e.x, e.y };
                if (pressed_ >= 0)
                    entries_[static_cast<size_t>(pressed_)]->dial.pointerDrag(e, *gestures());
                dirty_ = true;
            }

            void pointerUp(const PointerEvent& e) override
            {
                if (pressed_ >= 0)
                    entries_[static_cast<size_t>(pressed_)]->dial.pointerUp(e, *gestures());
                pressed_ = -1;
                dirty_ = true;
            }

            void doubleClick(const PointerEvent& e) override
            {
                const int i = at({ e.x, e.y });
                if (i >= 0)
                    entries_[static_cast<size_t>(i)]->dial.doubleClick(*gestures());
                dirty_ = true;
            }

            bool wheel(const WheelEvent& e) override
            {
                const int i = at({ e.x, e.y });
                dirty_ = true;
                if (i < 0)
                    return false;
                touched_ = i;
                return entries_[static_cast<size_t>(i)]->dial.wheel(e, *gestures(), host()->nowSeconds());
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
                return entries_[static_cast<size_t>(focus_)]->dial.key(e, *gestures());
            }

            Cursor cursor() const override
            {
                const int i = hasPointer_ ? at(pointer_) : -1;
                return i >= 0 ? entries_[static_cast<size_t>(i)]->dial.cursorAt(pointer_) : Cursor::normal;
            }

            void accessibility(std::vector<A11yItem>& out) const override
            {
                for (const auto& e : entries_)
                {
                    A11yItem item;
                    e->dial.accessibility(item);
                    out.push_back(item);
                }
            }

            void a11yAction(uint32_t id, A11yAction a, double value) override
            {
                for (auto& e : entries_)
                    if (e->dial.a11yId() == id)
                        e->dial.a11yAction(a, value, *gestures());
                dirty_ = true;
            }

            void draw(Canvas& c, const Theme& th) override
            {
                for (size_t i = 0; i < entries_.size(); ++i)
                    entries_[i]->dial.draw(c, th, ring_ && focus_ == static_cast<int>(i));
                int src = pressed_;
                if (src < 0 && hasPointer_)
                    src = at(pointer_);
                if (src < 0)
                    src = ring_ ? focus_ : touched_;
                if (src >= 0)
                {
                    char line[256];
                    entries_[static_cast<size_t>(src)]->dial.specLine(line, sizeof line);
                    const std::string shown = fitFields(c, line, kFooterW);
                    const Canvas::Scope s(c, tags::hint, false);
                    c.text(shown.c_str(), kLeft, kFooter, type::kMicro, th.ink32);
                }
            }

        private:
            int at(Point p) const
            {
                for (size_t i = 0; i < entries_.size(); ++i)
                    if (entries_[i]->dial.contains(p))
                        return static_cast<int>(i);
                return -1;
            }

            std::vector<std::unique_ptr<Entry>> entries_;
            Point pointer_{};
            bool  hasPointer_ = false, ring_ = false, dirty_ = false;
            int   pressed_ = -1, focus_ = -1, touched_ = -1;
        };

        // Script points: a primary dial's centre is (x + 30, top + 34).
        const Registration kDial{ SectionInfo{
            "dial",
            [] { return std::make_unique<DialSection>(); },
            {
                State{ "rest", {} },
                State{ "hover", [](HeadlessHost& h, Panel&) { h.move(colX(0) + 30.0f, kRow1 + 34.0f); } },
                State{ "drag",
                       [](HeadlessHost& h, Panel&) {
                           h.drag(colX(0) + 30.0f, kRow1 + 34.0f, colX(0) + 30.0f, kRow1 - 2.0f);
                       } },
                State{ "ghost",
                       [](HeadlessHost& h, Panel&) {
                           h.press(colX(1) + 30.0f, kRow1 + 34.0f);
                           h.moveTo(colX(1) + 30.0f, kRow1 + 10.0f);   // 24 px up: pitch 64, commit 38 -> held
                       } },
                State{ "keys", [](HeadlessHost& h, Panel&) { h.keys("tab,tab,right"); } },
                State{ "refused", [](HeadlessHost& h, Panel&) { h.click(colX(2) + 30.0f, kRow1 + 34.0f); } },
            } } };
    }
}

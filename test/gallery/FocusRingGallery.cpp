// FUNKGUI_TEST name=fg.gallery.focusring timeout=600 gpu=0 exe=FunkGuiGalleryProbe args="--section focusring"
//
// The "focusring" gallery section (FCompressor docs/design/02-funkgui-and-ui.md §5.8, §8.1, §8.9; G5): drawFocusRing
// as the controls draw it. Four accent hairlines on hit.reduced(1), floored to device pixels (HR BgfxEditor.cpp:
// 1542-1551), tagged FOCUS_RING: standalone rings on rectangles at fractional positions (and one too thin to have an
// inside, which draws nothing), and a Tab order walking a primary slot, its attached word, a stepped secondary slot
// and an n/a slot (which is in the order to reach its reason, 02 §8.1). States: rest, tab1 (the slot), tab2 (its
// word), tab3 (the stepped slot), tab4 (the n/a slot), shift-tab (backwards from nothing: the last stop), escape
// (the ring hidden, focus kept). The line above registers the test; the tools glob compiles this file into
// FunkGuiGalleryProbe.

#include "GalleryPanel.h"

#include <funkgui/canvas/Tags.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/params/ParamPort.h>
#include <funkgui/widgets/AttachedWord.h>
#include <funkgui/widgets/FocusRing.h>
#include <funkgui/widgets/RuleSlider.h>
#include <funkgui/widgets/ValueModel.h>

#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace funkgui::gallery
{
    namespace
    {
        class RingPort final : public ParamPort
        {
        public:
            RingPort(const char* id, float v) : id_(id), v_(v) {}
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

        constexpr Detent kDetect[3] = { { 0.0f, "PEAK", "peak" }, { 0.5f, "RMS", "RMS" },
                                        { 1.0f, "PK+RMS", "peak plus RMS" } };

        // MAKEUP (continuous), DETECT (stepped) or HOLD (n/a), from one parameter each.
        class RingModel final : public ValueModel
        {
        public:
            RingModel(ValueState s, const char* label, RingPort& p) : state_(s), label_(label), port_(p) {}

            uint64_t key() const override { return std::bit_cast<uint32_t>(port_.value01()); }

            void view(ValueView& v) const override
            {
                const float h = port_.value01();
                v.state = state_;
                v.label = label_;
                if (state_ == ValueState::na)
                {
                    v.reason = "NO HOLD STAGE IN THIS MODE";
                    return;
                }
                if (state_ == ValueState::stepped)
                {
                    v.nDetents = 3;
                    v.detents = kDetect;
                    v.detent = h < 0.25f ? 0 : (h < 0.75f ? 1 : 2);
                    v.track = (static_cast<float>(v.detent) + 0.5f) / 3.0f;
                    std::snprintf(v.text.value, sizeof v.text.value, "%s", kDetect[v.detent].label);
                    return;
                }
                v.track = h;
                v.trackDefault = 0.5f;
                v.bipolar = true;
                const float db = -24.0f + 48.0f * h;
                std::snprintf(v.text.value, sizeof v.text.value, "%s%.1f", db > 0.0f ? "+" : "", static_cast<double>(db));
                std::snprintf(v.text.unit, sizeof v.text.unit, "DB");
            }

            ParamPort* port() override { return &port_; }
            float host01FromTrack(float t) const override { return t; }
            float defaultHost01() const override { return 0.5f; }

        private:
            ValueState  state_;
            const char* label_;
            RingPort&   port_;
        };

        class RingToggle final : public ToggleModel
        {
        public:
            explicit RingToggle(RingPort& p) : port_(p) {}
            bool on() const override { return port_.value01() > 0.5f; }
            void set(bool v, GestureController& g) override { g.tap(port_, v ? 1.0f : 0.0f); }
            ParamPort* port() override { return &port_; }

        private:
            RingPort& port_;
        };

        constexpr SlotGeom kMakeup{ 16.0f, 24.0f, 112.0f, SlotSize::primary };
        constexpr SlotGeom kDetectGeom{ 144.0f, 24.0f, 112.0f, SlotSize::secondary };
        constexpr SlotGeom kHold{ 272.0f, 24.0f, 112.0f, SlotSize::secondary };

        // Standalone rings: fractional positions, and one 1.5 px wide rectangle whose inset has no inside.
        constexpr std::array<Rect, 4> kLoose{ { { 16.25f, 120.5f, 60.3f, 30.7f }, { 96.7f, 124.2f, 41.0f, 12.0f },
                                                { 160.0f, 118.0f, 1.5f, 30.0f }, { 184.5f, 118.5f, 24.0f, 24.0f } } };

        class FocusRingSection final : public Section
        {
        public:
            FocusRingSection()
                : Section(400, 176), makeupPort_("makeup", 0.625f), autoPort_("automu", 0.0f),
                  detectPort_("det", 0.5f), holdPort_("hold", 0.0f),
                  makeup_(ValueState::continuous, "MAKEUP", makeupPort_), detect_(ValueState::stepped, "DETECT", detectPort_),
                  hold_(ValueState::na, "HOLD", holdPort_), toggle_(autoPort_),
                  sliders_{ { RuleSlider(makeup_, kMakeup, 300), RuleSlider(detect_, kDetectGeom, 302),
                              RuleSlider(hold_, kHold, 303) } },
                  word_(toggle_, kMakeup, "AUTO", 301)
            {
                sliders_[0].setWord(&word_);
            }

            void tick(float dt) override
            {
                for (size_t i = 0; i < sliders_.size(); ++i)
                    sliders_[i].tick(dt, false, ring_ && focus_ == sliderStop(i), false);
                word_.tick(dt, false);
                dirty_ = false;
            }

            bool wantsFullRate() const override
            {
                bool busy = dirty_ || !word_.settled();
                for (const RuleSlider& s : sliders_)
                    busy = busy || !s.settled();
                return busy;
            }

            bool key(const KeyEvent& e) override
            {
                dirty_ = true;                           // the focused slot's chrome eases in on the next ticks
                if (e.key == Key::tab)
                {
                    const int dir = e.mods.shift ? -1 : 1;
                    focus_ = focus_ < 0 ? (dir > 0 ? 0 : kStops - 1) : (focus_ + dir + kStops) % kStops;
                    ring_ = true;
                    return true;
                }
                if (e.key == Key::escape && ring_)
                {
                    ring_ = false;
                    return true;
                }
                return false;
            }

            void accessibility(std::vector<A11yItem>& out) const override
            {
                A11yItem item;
                sliders_[0].accessibility(item);
                out.push_back(item);
                word_.accessibility(out);
                for (size_t i = 1; i < sliders_.size(); ++i)
                {
                    sliders_[i].accessibility(item);
                    out.push_back(item);
                }
            }

            void draw(Canvas& c, const Theme& th) override
            {
                for (size_t i = 0; i < sliders_.size(); ++i)
                    sliders_[i].draw(c, th, ring_ && focus_ == sliderStop(i));
                word_.draw(c, th, ring_ && focus_ == 1);
                for (const Rect& r : kLoose)
                    drawFocusRing(c, r, th.accent);
                const Canvas::Scope s(c, tags::slotLabel, false);
                c.text("LOOSE RINGS", 216.0f, 128.0f, type::kCaption, th.ink32);
            }

        private:
            static constexpr int kStops = 4;             // MAKEUP, its AUTO word, DETECT, HOLD

            static int sliderStop(size_t i) { return i == 0 ? 0 : static_cast<int>(i) + 1; }

            RingPort   makeupPort_, autoPort_, detectPort_, holdPort_;
            RingModel  makeup_, detect_, hold_;
            RingToggle toggle_;
            std::array<RuleSlider, 3> sliders_;
            AttachedWord word_;
            int  focus_ = -1;
            bool ring_ = false, dirty_ = false;
        };

        const Registration kFocusRing{ SectionInfo{
            "focusring",
            [] { return std::make_unique<FocusRingSection>(); },
            {
                State{ "rest", {} },
                State{ "tab1", [](HeadlessHost& h, Panel&) { h.keys("tab"); } },
                State{ "tab2", [](HeadlessHost& h, Panel&) { h.keys("tab,tab"); } },
                State{ "tab3", [](HeadlessHost& h, Panel&) { h.keys("tab,tab,tab"); } },
                State{ "tab4", [](HeadlessHost& h, Panel&) { h.keys("tab,tab,tab,tab"); } },
                State{ "shift-tab", [](HeadlessHost& h, Panel&) { h.keys("shift+tab"); } },
                State{ "escape", [](HeadlessHost& h, Panel&) { h.keys("tab,escape"); } },
            } } };
    }
}

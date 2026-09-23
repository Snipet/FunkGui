// FUNKGUI_TEST name=fg.gallery.latch timeout=600 gpu=0 exe=FunkGuiGalleryProbe args="--section latch"
//
// The "latch" gallery section (FCompressor docs/design/02-funkgui-and-ui.md §5.5, §6.3, §7.1, §8.9, §8.10; G6):
// LatchToggle as FCompressor's display row uses it — DELTA (off) and BYPASS (on), each a ParamToggle over a parameter,
// CHARACTERISTICS (a UI-state ToggleModel with no parameter, so no host menu), and LISTEN refused with its reason —
// and a footer: the reason of the latch under the pointer when it is refused, then a log of the writes and host menus,
// which is also an a11y item, so the golden a11y lines pin the latch semantics. States: rest, hover (ink100 text),
// armed (pressed and held: ink32 fill, accent text; nothing written yet), toggled (a click commits on up: one write),
// off (BYPASS clicked off), dragoff (down on DELTA, drag off, up: nothing), refused (a click on LISTEN: nothing, the
// reason in the footer), keys (Tab to DELTA, Return toggles it; focus ring), space-ui (Tab to CHARACTERISTICS, Space
// toggles the UI state: no parameter write), popup (ctrl-click on BYPASS: the host menu, no write). The line above
// registers the test; the tools glob compiles this file into FunkGuiGalleryProbe.

#include "GalleryPanel.h"

#include <funkgui/canvas/Tags.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/params/ParamPort.h>
#include <funkgui/widgets/LatchToggle.h>

#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace funkgui::gallery
{
    namespace
    {
        class CountingPort final : public ParamPort
        {
        public:
            CountingPort(const char* id, float v) : id_(id), v_(v) {}
            float value01() const override { return v_; }
            float default01() const override { return 0.0f; }
            int   numSteps() const override { return 2; }
            void  beginGesture() override {}
            void  setValue01(float v) override
            {
                v_ = v;
                ++writes;
            }
            void  endGesture() override {}
            const char* id() const override { return id_; }
            void* native() const override { return nullptr; }

            int writes = 0;

        private:
            const char* id_;
            float       v_;
        };

        class CountingHost final : public HostServices
        {
        public:
            void   bind(HostServices& h) { inner_ = &h; }
            void   setUnboundedDrag(bool on) override { inner_->setUnboundedDrag(on); }
            void   showParamMenu(ParamPort& p, float x, float y) override
            {
                ++menus;
                inner_->showParamMenu(p, x, y);
            }
            void   nudgeFullRate() override { inner_->nudgeFullRate(); }
            double nowSeconds() const override { return inner_->nowSeconds(); }
            void   beginBatch() override { inner_->beginBatch(); }
            void   endBatch() override { inner_->endBatch(); }

            int menus = 0;

        private:
            HostServices* inner_ = nullptr;
        };

        // CHARACTERISTICS: per-instance UI state (02 §7.1), not a parameter.
        class ScreenToggle final : public ToggleModel
        {
        public:
            bool on() const override { return on_; }
            void set(bool v, GestureController&) override { on_ = v; }

        private:
            bool on_ = false;
        };

        // LISTEN, refused: a ParamToggle whose parameter cannot be used here.
        class RefusedToggle final : public ParamToggle
        {
        public:
            using ParamToggle::ParamToggle;
            bool enabled() const override { return false; }
            const char* reason() const override { return "NO SIDECHAIN BUS CONNECTED"; }
        };

        constexpr float kLeft = 16.0f, kReasonY = 64.0f, kLogY = 84.0f;
        constexpr Point kOutside{ -1000.0f, -1000.0f };
        constexpr uint32_t kIdLog = 900;
        constexpr Rect kDelta{ 16, 16, 88, 36 }, kBypass{ 112, 16, 96, 36 }, kChars{ 216, 16, 132, 36 },
                       kListen{ 356, 16, 108, 36 };

        class LatchSection final : public Section
        {
        public:
            LatchSection() : Section(480, 104), delta_(deltaPort_), bypass_(bypassPort_), listen_(listenPort_)
            {
                latches_.push_back(std::make_unique<LatchToggle>(delta_, kDelta, "DELTA", 100));
                latches_.push_back(std::make_unique<LatchToggle>(bypass_, kBypass, "BYPASS", 101));
                latches_.push_back(std::make_unique<LatchToggle>(chars_, kChars, "CHARACTERISTICS", 102));
                latches_.push_back(std::make_unique<LatchToggle>(listen_, kListen, "LISTEN", 103));
            }

            void attach(HostServices& h) override
            {
                Section::attach(h);
                counting_.bind(h);
                g_.emplace(counting_);
            }

            void idle(double nowSec) override
            {
                if (g_)
                    g_->poll(nowSec);
            }

            void closeGestures() override
            {
                if (g_)
                    g_->closeAll();
            }

            void tick(float dt) override
            {
                for (auto& l : latches_)
                    l->tick(dt, hasPointer_ ? pointer_ : kOutside);
                dirty_ = false;
            }

            bool wantsFullRate() const override
            {
                if (dirty_)
                    return true;
                for (const auto& l : latches_)
                    if (!l->settled())
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
                ring_ = false;
                pointer_ = { e.x, e.y };
                hasPointer_ = true;
                pressed_ = -1;
                for (size_t i = 0; i < latches_.size(); ++i)
                    if (latches_[i]->contains(pointer_))
                    {
                        pressed_ = static_cast<int>(i);
                        latches_[i]->pointerDown(e, *g_);
                    }
                dirty_ = true;
            }

            void pointerDrag(const PointerEvent& e) override
            {
                pointer_ = { e.x, e.y };
                if (pressed_ >= 0)
                    latches_[static_cast<size_t>(pressed_)]->pointerDrag(e);
                dirty_ = true;
            }

            void pointerUp(const PointerEvent& e) override
            {
                if (pressed_ >= 0)
                    latches_[static_cast<size_t>(pressed_)]->pointerUp(e, *g_);
                pressed_ = -1;
                dirty_ = true;
            }

            bool key(const KeyEvent& e) override
            {
                dirty_ = true;
                const int n = static_cast<int>(latches_.size());
                if (e.key == Key::tab)
                {
                    const int dir = e.mods.shift ? -1 : 1;
                    focus_ = focus_ < 0 ? (dir > 0 ? 0 : n - 1) : (focus_ + dir + n) % n;
                    ring_ = true;
                    return true;
                }
                if (focus_ < 0)
                    return false;
                return latches_[static_cast<size_t>(focus_)]->key(e, *g_);
            }

            Cursor cursor() const override
            {
                for (const auto& l : latches_)
                    if (hasPointer_ && l->contains(pointer_))
                        return l->cursorAt(pointer_);
                return Cursor::normal;
            }

            void accessibility(std::vector<A11yItem>& out) const override
            {
                for (const auto& l : latches_)
                    l->accessibility(out);
                A11yItem log;
                log.id = kIdLog;
                log.role = A11yRole::staticText;
                log.bounds = { kLeft, kLogY, 448, 14 };
                log.title = "LOG";
                log.value = logText();
                log.readOnly = true;
                out.push_back(log);
            }

            void a11yAction(uint32_t id, A11yAction a, double) override
            {
                for (auto& l : latches_)
                    if (l->a11yAction(id, a, *g_))
                        break;
                dirty_ = true;
            }

            void draw(Canvas& c, const Theme& th) override
            {
                for (size_t i = 0; i < latches_.size(); ++i)
                    latches_[i]->draw(c, th, ring_ && focus_ == static_cast<int>(i));
                const Canvas::Scope s(c, tags::hint, false);
                for (const auto& l : latches_)
                    if (hasPointer_ && l->contains(pointer_) && l->reason() != nullptr)
                        c.text(l->reason(), kLeft, kReasonY, type::kLabel, th.ink32);
                c.text(logText().c_str(), kLeft, kLogY, type::kMicro, th.ink32);
            }

        private:
            std::string logText() const
            {
                char buf[96];
                std::snprintf(buf, sizeof buf, "WRITES %d   MENUS %d   SCREEN %s",
                              deltaPort_.writes + bypassPort_.writes + listenPort_.writes, counting_.menus,
                              chars_.on() ? "CHARACTERISTICS" : "PANEL");
                return buf;
            }

            CountingPort  deltaPort_{ "delta", 0.0f }, bypassPort_{ "bypass", 1.0f }, listenPort_{ "listen", 0.0f };
            ParamToggle   delta_, bypass_;
            ScreenToggle  chars_;
            RefusedToggle listen_;
            std::vector<std::unique_ptr<LatchToggle>> latches_;
            CountingHost  counting_;
            std::optional<GestureController> g_;
            Point pointer_{};
            bool  hasPointer_ = false, ring_ = false, dirty_ = false;
            int   focus_ = -1, pressed_ = -1;
        };

        constexpr float kDeltaX = kDelta.x + 44.0f, kDeltaY = kDelta.y + 18.0f;
        constexpr float kBypassX = kBypass.x + 48.0f, kBypassY = kBypass.y + 18.0f;

        const Registration kLatch{ SectionInfo{
            "latch",
            [] { return std::make_unique<LatchSection>(); },
            {
                State{ "rest", {} },
                State{ "hover", [](HeadlessHost& h, Panel&) { h.move(kDeltaX, kDeltaY); } },
                State{ "armed",
                       [](HeadlessHost&, Panel& p) {
                           PointerEvent e;
                           e.x = kDeltaX;
                           e.y = kDeltaY;
                           p.pointerMove(e);
                           p.pointerDown(e);
                       } },
                State{ "toggled", [](HeadlessHost& h, Panel&) { h.click(kDeltaX, kDeltaY); } },
                State{ "off", [](HeadlessHost& h, Panel&) { h.click(kBypassX, kBypassY); } },
                State{ "dragoff",
                       [](HeadlessHost& h, Panel&) { h.drag(kDeltaX, kDeltaY, kDeltaX + 20.0f, kDeltaY + 50.0f, 4); } },
                State{ "refused", [](HeadlessHost& h, Panel&) { h.click(kListen.x + 54.0f, kListen.y + 18.0f); } },
                State{ "keys", [](HeadlessHost& h, Panel&) { h.keys("tab,return"); } },
                State{ "space-ui", [](HeadlessHost& h, Panel&) { h.keys("tab,tab,tab,space"); } },
                State{ "popup",
                       [](HeadlessHost& h, Panel&) {
                           Mods m;
                           m.ctrl = true;
                           h.click(kBypassX, kBypassY, m);
                       } },
            } } };
    }
}

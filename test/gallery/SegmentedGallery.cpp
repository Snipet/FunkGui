// FUNKGUI_TEST name=fg.gallery.segmented timeout=600 gpu=0 exe=FunkGuiGalleryProbe args="--section segmented"
//
// The "segmented" gallery section (FCompressor docs/design/02-funkgui-and-ui.md §5.5, §6.3, §8.9, §8.10; G6):
// SegmentedSelector in both styles, over the models a product uses, laid out like FCompressor's display row:
//   QUALITY   ECO [STD] HQ          text cells over a 3-choice parameter (ParamCells), with a11y help
//   LOOKAHEAD [OFF] 5 MS 20 MS      text cells whose last cell is disabled with a reason (a ParamCells subclass)
//   RATIO     [4] 8 12 20 ALL       boxed cells over a 5-choice parameter
//   SIDECHAIN | COLOUR              tab cells with no parameter (a plain CellModel; no host menu)
// and a footer: the help (or refusal reason) of the cell under the pointer, then a log of the writes and host menus
// the section's ports and host saw, which is also an a11y item, so the golden a11y lines pin the tap semantics.
// States: rest, hover (ink100), select (a click selects on down: one write), same (a click on the active cell: no
// write), refused (a click on the disabled cell: no write, the reason in the footer), popup (a ctrl-click: the host
// menu, no write), boxed (a boxed cell selected), tab (a tab cell; no port, no write), keys (Tab to QUALITY, → selects
// HQ; focus ring), keys-end (Tab twice to LOOKAHEAD, End skips the disabled cell). The line above registers the test;
// the tools glob compiles this file into FunkGuiGalleryProbe.

#include "GalleryPanel.h"

#include <funkgui/canvas/Tags.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/params/ParamPort.h>
#include <funkgui/widgets/SegmentedSelector.h>

#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace funkgui::gallery
{
    namespace
    {
        // A host parameter that counts its writes.
        class CountingPort final : public ParamPort
        {
        public:
            CountingPort(const char* id, float v) : id_(id), v_(v) {}
            float value01() const override { return v_; }
            float default01() const override { return 0.0f; }
            int   numSteps() const override { return 5; }
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

        // Forwards every call to the real host and counts the host-menu requests.
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

        // LOOKAHEAD: 20 MS is refused in this gallery, with its reason as the cell's help.
        class LookaheadCells final : public ParamCells
        {
        public:
            using ParamCells::ParamCells;
            bool enabled(int i) const override { return i != 2; }
            const char* help(int i) const override
            {
                return i == 2 ? "20 MS NEEDS THE HQ SETTING IN THIS GALLERY" : ParamCells::help(i);
            }
        };

        // Tabs: UI state, not a parameter.
        class TabCells final : public CellModel
        {
        public:
            int  count() const override { return 2; }
            int  active() const override { return tab_; }
            const char* label(int i) const override { return i == 0 ? "SIDECHAIN" : "COLOUR"; }
            const char* spoken(int i) const override { return i == 0 ? "Side-chain" : "Colour"; }
            const char* help(int i) const override
            {
                return i == 0 ? "SHOWS THE SIDE-CHAIN FILTER" : "SHOWS THE COLOUR STAGE";
            }
            void select(int i, GestureController&) override { tab_ = i; }

        private:
            int tab_ = 0;
        };

        constexpr float kLeft = 16.0f, kCells = 82.0f, kHelpY = 138.0f, kLogY = 156.0f;
        constexpr Point kOutside{ -1000.0f, -1000.0f };
        constexpr uint32_t kIdLog = 900;

        class SegmentedSection final : public Section
        {
        public:
            SegmentedSection()
                : Section(480, 180),
                  quality_(qualityPort_, { { "ECO", "Eco", "ECO: NO OVERSAMPLING, NO ADDED LATENCY" },
                                           { "STD", "Std", "STD: 2 TIMES IIR OVERSAMPLING, 4 SAMPLES LATENCY" },
                                           { "HQ", "HQ", "HQ: 4 TIMES LINEAR-PHASE OVERSAMPLING" } }),
                  lookahead_(lookaheadPort_, { { "OFF", "Off", "NO LOOKAHEAD" },
                                               { "5 MS", "5 milliseconds", "5 MS LOOKAHEAD, 5 MS LATENCY" },
                                               { "20 MS", "20 milliseconds", nullptr } }),
                  ratio_(ratioPort_, { { "4", "4 to 1" }, { "8", "8 to 1" }, { "12", "12 to 1" }, { "20", "20 to 1" },
                                       { "ALL", "all buttons" } })
            {
                sels_.push_back(std::make_unique<SegmentedSelector>(
                    quality_, std::vector<Rect>{ { kCells, 16, 32, 16 }, { 118, 16, 32, 16 }, { 154, 16, 26, 16 } },
                    CellStyle::text, "QUALITY", Point{ kLeft, 20 }, 100));
                sels_.push_back(std::make_unique<SegmentedSelector>(
                    lookahead_, std::vector<Rect>{ { kCells, 42, 32, 16 }, { 118, 42, 38, 16 }, { 160, 42, 44, 16 } },
                    CellStyle::text, "LOOKAHEAD", Point{ kLeft, 46 }, 200));
                sels_.push_back(std::make_unique<SegmentedSelector>(
                    ratio_,
                    std::vector<Rect>{ { kCells, 70, 40, 28 }, { 126, 70, 40, 28 }, { 170, 70, 40, 28 },
                                       { 214, 70, 40, 28 }, { 258, 70, 48, 28 } },
                    CellStyle::boxed, "RATIO", Point{ kLeft, 80 }, 300));
                sels_.push_back(std::make_unique<SegmentedSelector>(
                    tabs_, std::vector<Rect>{ { kCells, 110, 62, 16 }, { 150, 110, 44, 16 } }, CellStyle::text,
                    nullptr, Point{}, 400));
                sels_.back()->setSpokenTitle("Side-chain or colour");
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
                for (auto& s : sels_)
                    s->tick(dt, hasPointer_ ? pointer_ : kOutside);
                dirty_ = false;
            }

            bool wantsFullRate() const override
            {
                if (dirty_)
                    return true;
                for (const auto& s : sels_)
                    if (!s->settled())
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
                pointer_ = { e.x, e.y };
                hasPointer_ = true;
                for (auto& s : sels_)
                    if (s->contains(pointer_))
                        s->pointerDown(e, *g_);
                dirty_ = true;
            }

            bool key(const KeyEvent& e) override
            {
                dirty_ = true;
                const int n = static_cast<int>(sels_.size());
                if (e.key == Key::tab)
                {
                    const int dir = e.mods.shift ? -1 : 1;
                    focus_ = focus_ < 0 ? (dir > 0 ? 0 : n - 1) : (focus_ + dir + n) % n;
                    ring_ = true;
                    return true;
                }
                if (focus_ < 0)
                    return false;
                return sels_[static_cast<size_t>(focus_)]->key(e, *g_);
            }

            Cursor cursor() const override
            {
                for (const auto& s : sels_)
                    if (hasPointer_ && s->contains(pointer_))
                        return s->cursorAt(pointer_);
                return Cursor::normal;
            }

            void accessibility(std::vector<A11yItem>& out) const override
            {
                for (const auto& s : sels_)
                    s->accessibility(out);
                A11yItem log;
                log.id = kIdLog;
                log.role = A11yRole::staticText;
                log.bounds = { kLeft, kLogY, 448, 14 };
                log.title = "LOG";
                log.value = logText();
                log.readOnly = true;
                out.push_back(log);
            }

            void a11yAction(uint32_t id, A11yAction a, double value) override
            {
                for (auto& s : sels_)
                    if (s->a11yAction(id, a, value, *g_))
                        break;
                dirty_ = true;
            }

            void draw(Canvas& c, const Theme& th) override
            {
                for (size_t i = 0; i < sels_.size(); ++i)
                    sels_[i]->draw(c, th, ring_ && focus_ == static_cast<int>(i));
                const Canvas::Scope s(c, tags::hint, false);
                if (const char* h = helpUnderPointer())
                    c.text(h, kLeft, kHelpY, type::kLabel, th.ink32);
                c.text(logText().c_str(), kLeft, kLogY, type::kMicro, th.ink32);
            }

        private:
            const char* helpUnderPointer() const
            {
                if (!hasPointer_)
                    return nullptr;
                const CellModel* models[] = { &quality_, &lookahead_, &ratio_, &tabs_ };
                for (size_t i = 0; i < sels_.size(); ++i)
                    if (const int cell = sels_[i]->cellAt(pointer_); cell >= 0)
                        return models[i]->help(cell);
                return nullptr;
            }

            std::string logText() const
            {
                char buf[96];
                std::snprintf(buf, sizeof buf, "WRITES %d   MENUS %d   TAB %s",
                              qualityPort_.writes + lookaheadPort_.writes + ratioPort_.writes, counting_.menus,
                              tabs_.label(tabs_.active()));
                return buf;
            }

            CountingPort   qualityPort_{ "quality", 0.5f }, lookaheadPort_{ "labudget", 0.0f }, ratioPort_{ "ratio", 0.0f };
            ParamCells     quality_;
            LookaheadCells lookahead_;
            ParamCells     ratio_;
            TabCells       tabs_;
            std::vector<std::unique_ptr<SegmentedSelector>> sels_;
            CountingHost   counting_;
            std::optional<GestureController> g_;
            Point pointer_{};
            bool  hasPointer_ = false, ring_ = false, dirty_ = false;
            int   focus_ = -1;
        };

        const Registration kSegmented{ SectionInfo{
            "segmented",
            [] { return std::make_unique<SegmentedSection>(); },
            {
                State{ "rest", {} },
                State{ "hover", [](HeadlessHost& h, Panel&) { h.move(98.0f, 24.0f); } },
                State{ "select", [](HeadlessHost& h, Panel&) { h.click(98.0f, 24.0f); } },
                State{ "same", [](HeadlessHost& h, Panel&) { h.click(134.0f, 24.0f); } },
                State{ "refused", [](HeadlessHost& h, Panel&) { h.click(182.0f, 50.0f); } },
                State{ "popup",
                       [](HeadlessHost& h, Panel&) {
                           Mods m;
                           m.ctrl = true;
                           h.click(98.0f, 50.0f, m);
                       } },
                State{ "boxed", [](HeadlessHost& h, Panel&) { h.click(190.0f, 84.0f); } },
                State{ "tab", [](HeadlessHost& h, Panel&) { h.click(172.0f, 118.0f); } },
                State{ "keys", [](HeadlessHost& h, Panel&) { h.keys("tab,right"); } },
                State{ "keys-end", [](HeadlessHost& h, Panel&) { h.keys("tab,tab,end"); } },
            } } };
    }
}

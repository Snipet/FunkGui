// FUNKGUI_TEST name=fg.gallery.dwell timeout=600 gpu=0 exe=FunkGuiGalleryProbe args="--section dwell"
//
// The "dwell" gallery section (FCompressor docs/design/02-funkgui-and-ui.md §5.7, §7.1; HR BgfxEditor.cpp:1010-1047;
// G6): a DwellSelector choosing which of three views fills a plot, and a ScreenFader switching two screens.
//   - Tabs RANK / FILTER / MOD (a SegmentedSelector whose model pins the view: tab-pinned).
//   - Three mock controls, one per view: pressing one holds its view for the press, releasing it keeps the view for
//     the 0.9 s dwell, and a wheel notch on one touches its view (the dwell), as HR's band did; hover does nothing.
//   - The plot draws outgoing() at 1 - amount() and incoming() at amount(), each view its own shapes (bars, a filter
//     curve, a modulation wave); a layer at alpha 0 is not drawn, so a settled frame holds one view.
//   - A SCREEN latch pins a ScreenFader (tau kScreenFadeTau) between PANEL and CHARACTERISTICS, whose names crossfade
//     in the strip at the bottom.
// Every state is settled, so a dwell has always run out by the frame: release and touch come back to the pinned view
// (the same frame as rest), which is the dwell's contract. States: rest (RANK), pin (the MOD tab), hold (FILTER held:
// down with no up), release (a click on FILTER: back to RANK), touch (a wheel notch on MOD: back to RANK), keys (Tab to
// the tabs, End pins MOD; focus ring), screen (the SCREEN latch: CHARACTERISTICS). The line above registers the test;
// the tools glob compiles this file into FunkGuiGalleryProbe.

#include "GalleryPanel.h"

#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Col.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/widgets/DwellSelector.h>
#include <funkgui/widgets/LatchToggle.h>
#include <funkgui/widgets/SegmentedSelector.h>

#include <memory>
#include <vector>

namespace funkgui::gallery
{
    namespace
    {
        constexpr const char* kViewNames[] = { "RANK", "FILTER", "MOD" };
        constexpr const char* kScreenNames[] = { "PANEL", "CHARACTERISTICS" };
        constexpr Rect  kControls[] = { { 16, 40, 136, 28 }, { 160, 40, 136, 28 }, { 304, 40, 136, 28 } };
        constexpr const char* kControlNames[] = { "SIZE", "CUTOFF", "RATE" };
        constexpr Rect  kPlot{ 16, 80, 448, 88 };
        constexpr float kScreenY = 180.0f;
        constexpr Point kOutside{ -1000.0f, -1000.0f };
        constexpr uint32_t kIdView = 900, kIdScreen = 901, kIdControls = 910;

        using Views = DwellSelector<int>;

        // The tabs pin the view (02 §7.3: "tab-pinned").
        class TabCells final : public CellModel
        {
        public:
            explicit TabCells(Views& v) : views_(v) {}
            int  count() const override { return 3; }
            int  active() const override { return views_.pinned(); }
            const char* label(int i) const override { return i >= 0 && i < 3 ? kViewNames[i] : ""; }
            const char* spoken(int i) const override { return label(i); }
            void select(int i, GestureController&) override { views_.pin(i); }

        private:
            Views& views_;
        };

        class ScreenToggle final : public ToggleModel
        {
        public:
            explicit ScreenToggle(ScreenFader& f) : fader_(f) {}
            bool on() const override { return fader_.pinned() == 1; }
            void set(bool v, GestureController&) override { fader_.pin(v ? 1 : 0); }

        private:
            ScreenFader& fader_;
        };

        class DwellSection final : public Section
        {
        public:
            DwellSection()
                : Section(480, 208), fader_(0, kScreenFadeTau), tabModel_(views_), screenModel_(fader_),
                  tabs_(tabModel_, { { 16, 12, 40, 16 }, { 60, 12, 52, 16 }, { 116, 12, 32, 16 } }, CellStyle::text,
                        nullptr, {}, 100),
                  screen_(screenModel_, { 352, 8, 112, 24 }, "SCREEN", 200)
            {
                tabs_.setSpokenTitle("View");
            }

            void tick(float dt) override
            {
                const Point p = hasPointer_ ? pointer_ : kOutside;
                tabs_.tick(dt, p);
                screen_.tick(dt, p);
                views_.tick(dt);
                fader_.tick(dt);
                dirty_ = false;
            }

            bool wantsFullRate() const override
            {
                return dirty_ || !tabs_.settled() || !screen_.settled() || !views_.settled() || !fader_.settled();
            }

            void pointerMove(const PointerEvent& e) override
            {
                pointer_ = { e.x, e.y };
                hasPointer_ = true;
                dirty_ = true;                           // hover never switches views
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
                if (tabs_.contains(pointer_))
                    tabs_.pointerDown(e, *gestures());
                else if (screen_.contains(pointer_))
                {
                    screen_.pointerDown(e, *gestures());
                    pressedScreen_ = true;
                }
                else if (const int c = controlAt(pointer_); c >= 0)
                    views_.hold(c);                      // a drag holds the dragged control's view
                dirty_ = true;
            }

            void pointerDrag(const PointerEvent& e) override
            {
                pointer_ = { e.x, e.y };
                if (pressedScreen_)
                    screen_.pointerDrag(e);
                dirty_ = true;
            }

            void pointerUp(const PointerEvent& e) override
            {
                if (pressedScreen_)
                    screen_.pointerUp(e, *gestures());
                pressedScreen_ = false;
                views_.hold(std::nullopt);               // released: the view stays for the dwell
                dirty_ = true;
            }

            bool wheel(const WheelEvent& e) override
            {
                const int c = controlAt({ e.x, e.y });
                if (c < 0)
                    return false;
                views_.touch(c);                         // a wheel write: hold the view for the dwell
                dirty_ = true;
                return true;
            }

            bool key(const KeyEvent& e) override
            {
                dirty_ = true;
                if (e.key == Key::tab)
                {
                    focus_ = focus_ < 0 ? 0 : (focus_ + 1) % 2;
                    ring_ = true;
                    return true;
                }
                if (focus_ == 0)
                    return tabs_.key(e, *gestures());
                if (focus_ == 1)
                    return screen_.key(e, *gestures());
                return false;
            }

            Cursor cursor() const override
            {
                if (hasPointer_ && tabs_.contains(pointer_))
                    return tabs_.cursorAt(pointer_);
                if (hasPointer_ && screen_.contains(pointer_))
                    return screen_.cursorAt(pointer_);
                return Cursor::normal;
            }

            void accessibility(std::vector<A11yItem>& out) const override
            {
                tabs_.accessibility(out);
                screen_.accessibility(out);
                for (uint32_t i = 0; i < 3; ++i)
                {
                    A11yItem c;
                    c.id = kIdControls + i;
                    c.role = A11yRole::button;
                    c.bounds = kControls[i];
                    c.title = kControlNames[i];
                    c.help = "HOLDS THE VIEW WHILE PRESSED";
                    out.push_back(c);
                }
                A11yItem view;
                view.id = kIdView;
                view.role = A11yRole::image;
                view.bounds = kPlot;
                view.title = "VIEW";
                view.value = kViewNames[views_.incoming()];
                out.push_back(view);
                A11yItem scr;
                scr.id = kIdScreen;
                scr.role = A11yRole::staticText;
                scr.bounds = { 16, kScreenY, 448, 16 };
                scr.title = "SCREEN";
                scr.value = kScreenNames[fader_.incoming()];
                scr.readOnly = true;
                out.push_back(scr);
            }

            void a11yAction(uint32_t id, A11yAction a, double value) override
            {
                if (!tabs_.a11yAction(id, a, value, *gestures()))
                    screen_.a11yAction(id, a, *gestures());
                dirty_ = true;
            }

            void draw(Canvas& c, const Theme& th) override
            {
                tabs_.draw(c, th, ring_ && focus_ == 0);
                screen_.draw(c, th, ring_ && focus_ == 1);
                {
                    const Canvas::Scope s(c, tags::slotLabel, false);
                    for (size_t i = 0; i < 3; ++i)
                    {
                        const Rect& r = kControls[i];
                        c.rrect(r.x, r.y, r.w, r.h, 0.0f, th.ink16);
                        c.text(kControlNames[i], r.x + 8.0f, c.capCentreTop(r.centreY(), type::kLabel), type::kLabel,
                               th.ink52);
                        c.text(kViewNames[i], r.right() - 8.0f, c.capCentreTop(r.centreY(), type::kMicro),
                               type::kMicro, th.ink32, Align::right);
                    }
                }
                c.hairlineH(kPlot.x, kPlot.bottom(), kPlot.w, th.ink16);
                const float amt = views_.amount();
                if (amt < 1.0f && views_.outgoing() != views_.incoming())
                    drawView(c, th, views_.outgoing(), 1.0f - amt);
                drawView(c, th, views_.incoming(), amt);
                const float fa = fader_.amount();
                if (fa < 1.0f && fader_.outgoing() != fader_.incoming())
                    c.text(kScreenNames[fader_.outgoing()], 16.0f, kScreenY, type::kLatch, fade(th.ink52, 1.0f - fa));
                c.text(kScreenNames[fader_.incoming()], 16.0f, kScreenY, type::kLatch, fade(th.ink100, fa));
            }

        private:
            static int controlAt(Point p)
            {
                for (int i = 0; i < 3; ++i)
                    if (kControls[i].contains(p))
                        return i;
                return -1;
            }

            // One view at alpha a (colour only; an invisible layer is not drawn).
            static void drawView(Canvas& c, const Theme& th, int view, float a)
            {
                if (!(a > 0.0f))
                    return;
                const Canvas::Scope s(c, tags::none, false);
                const Col ink = fade(th.ink70, a);
                c.text(kViewNames[view], kPlot.x + 4.0f, kPlot.y + 4.0f, type::kMicro, fade(th.ink32, a));
                const float base = kPlot.bottom() - 4.0f;
                if (view == 0)
                {
                    for (int i = 0; i < 16; ++i)                 // RANK: bars
                    {
                        const float h = 12.0f + static_cast<float>((i * 37) % 53);
                        c.rrect(kPlot.x + 24.0f + 26.0f * static_cast<float>(i), base - h, 6.0f, h, 0.0f, ink);
                    }
                    return;
                }
                float xs[48], ys[48];
                for (int i = 0; i < 48; ++i)
                {
                    const float t = static_cast<float>(i) / 47.0f;
                    xs[i] = kPlot.x + 16.0f + t * (kPlot.w - 32.0f);
                    const float lp = t < 0.6f ? 0.0f : (t - 0.6f) * (t - 0.6f) * 300.0f;   // FILTER: a low-pass knee
                    const float tri = 1.0f - 4.0f * (t * 3.0f - static_cast<float>(static_cast<int>(t * 3.0f)) - 0.5f)
                                                 * (t * 3.0f - static_cast<float>(static_cast<int>(t * 3.0f)) - 0.5f);
                    ys[i] = view == 1 ? kPlot.y + 24.0f + (lp < 56.0f ? lp : 56.0f)          // MOD: parabolic waves
                                      : base - 8.0f - 44.0f * tri;
                }
                c.polyline(xs, ys, 48, 1.5f, premix(th.ground, th.ink70, a));
            }

            Views        views_{ 0 };
            ScreenFader  fader_;
            TabCells     tabModel_;
            ScreenToggle screenModel_;
            SegmentedSelector tabs_;
            LatchToggle  screen_;
            Point pointer_{};
            bool  hasPointer_ = false, ring_ = false, dirty_ = false, pressedScreen_ = false;
            int   focus_ = -1;
        };

        constexpr float kFilterX = 228.0f, kModX = 372.0f, kControlY = 54.0f;

        const Registration kDwell{ SectionInfo{
            "dwell",
            [] { return std::make_unique<DwellSection>(); },
            {
                State{ "rest", {} },
                State{ "pin", [](HeadlessHost& h, Panel&) { h.click(132.0f, 20.0f); } },
                State{ "hold",
                       [](HeadlessHost&, Panel& p) {
                           PointerEvent e;
                           e.x = kFilterX;
                           e.y = kControlY;
                           p.pointerMove(e);
                           p.pointerDown(e);
                       } },
                State{ "release", [](HeadlessHost& h, Panel&) { h.click(kFilterX, kControlY); } },
                State{ "touch", [](HeadlessHost& h, Panel&) { h.wheel(kModX, kControlY, 1.0f); } },
                State{ "keys", [](HeadlessHost& h, Panel&) { h.keys("tab,end"); } },
                State{ "screen", [](HeadlessHost& h, Panel&) { h.click(408.0f, 20.0f); } },
            } } };
    }
}

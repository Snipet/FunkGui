// FUNKGUI_TEST name=fg.gallery.hint timeout=600 gpu=0 exe=FunkGuiGalleryProbe args="--section hint"
//
// The "hint" gallery section (FCompressor docs/design/02-funkgui-and-ui.md §5.8, §6.6, §3.7 rule 6; HR
// BgfxEditor.cpp:1564-1650; G6): a HintLine as a footer, over a mock slot whose hover chrome (an accent caret on its
// rule) shows under the pointer or, once a down has been seen with no move ever, always (always-chrome). The footer is
// the first-run hint while it lasts, then the spec line, which is longer than the footer and ends in U+2026 (fitted
// with text::fitEllipsis). Two a11y items say which line is shown and which chrome rule is in force.
//
// The section ticks the hint, but its countdown does not hold the section at full rate, so a state shows the hint at
// the age its script leaves it (a product's Panel does count it: HintLine::wantsFullRate()). States: rest (the hint, 6 s
// left: no tick runs), expired (361 frames: the spec line), moved (a move away from the slot cuts the hint to 0.4 s, and
// 6 frames fade it to 0.3 s: the same geometry as rest, fainter), cut (a move, then 30 frames: the spec line), skip
// (HintLine::skip(): the spec line), chrome (a down and up with no move ever: always-chrome, the caret shows), chrome-
// moved (the same, then a move away: the chrome goes again). The line above registers the test; the tools glob compiles
// this file into FunkGuiGalleryProbe.

#include "GalleryPanel.h"

#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Col.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/widgets/HintLine.h>

#include <memory>
#include <vector>

namespace funkgui::gallery
{
    namespace
    {
        constexpr float kLeft = 16.0f, kFooterY = 42.0f, kFooterW = 448.0f;
        constexpr Rect  kSlot{ 10.0f, 2.0f, 124.0f, 34.0f };          // the mock slot's hit
        constexpr float kTrackY = 30.0f, kTrackW = 112.0f, kCaretX = 58.0f;
        constexpr uint32_t kIdFooter = 900, kIdChrome = 901;
        constexpr const char* kFirstRun = "DRAG A VALUE OR THE CURVE. DOUBLE-CLICK TO RESET.";
        constexpr const char* kSpec = "RATIO   STEPS 2 \xC2\xB7 4 \xC2\xB7 10   DRAG / WHEEL / ARROWS STEP   CLICK A STEP"
                                      "   DBL-CLICK RESET   RIGHT-CLICK MENU";

        class HintSection final : public Section
        {
        public:
            HintSection() : Section(480, 64) {}

            void skip() { hint_.skip(); }

            void tick(float dt) override
            {
                hint_.tick(dt);
                dirty_ = false;
            }

            bool wantsFullRate() const override { return dirty_; }      // not the hint: see the file comment

            void pointerMove(const PointerEvent& e) override
            {
                hint_.pointerMoved();
                hovered_ = kSlot.contains({ e.x, e.y });
                dirty_ = true;
            }

            void pointerExit() override
            {
                hovered_ = false;
                dirty_ = true;
            }

            void pointerDown(const PointerEvent& e) override
            {
                hint_.noteDown();
                hovered_ = kSlot.contains({ e.x, e.y });
                dirty_ = true;
            }

            void accessibility(std::vector<A11yItem>& out) const override
            {
                A11yItem footer;
                footer.id = kIdFooter;
                footer.role = A11yRole::staticText;
                footer.bounds = { kLeft, kFooterY, kFooterW, 14 };
                footer.title = "FOOTER";
                footer.value = hint_.active() ? "first-run hint" : "spec line";
                footer.readOnly = true;
                out.push_back(footer);

                A11yItem chrome;
                chrome.id = kIdChrome;
                chrome.role = A11yRole::staticText;
                chrome.bounds = kSlot;
                chrome.title = "SLOT CHROME";
                chrome.value = hint_.alwaysChrome() ? "always" : (hovered_ ? "hover" : "rest");
                chrome.readOnly = true;
                out.push_back(chrome);
            }

            void draw(Canvas& c, const Theme& th) override
            {
                const bool chrome = hovered_ || hint_.alwaysChrome();
                {
                    const Canvas::Scope s(c, tags::slotLabel, false);
                    c.text("RATIO", kLeft, 6.0f, type::kLabel, chrome ? th.ink70 : th.ink52);
                }
                {
                    const Canvas::Scope s(c, tags::slotTrack, false);
                    c.hairlineH(kLeft, kTrackY, kTrackW, th.ink16);
                    if (chrome)
                        c.rrect(kLeft, kTrackY - 0.5f, kCaretX - kLeft, 1.0f, 0.0f, th.accentDim);
                }
                if (chrome)
                {
                    const Canvas::Scope s(c, tags::slotCaret, false);
                    c.rrect(kCaretX - 1.0f, kTrackY + 3.5f - 11.0f, 2.0f, 11.0f, 0.0f, th.accent);
                }
                hint_.draw(c, th, kLeft, kFooterY, kFooterW, kSpec);
            }

        private:
            HintLine hint_{ kFirstRun };
            bool     hovered_ = false, dirty_ = false;
        };

        PointerEvent at(float x, float y)
        {
            PointerEvent e;
            e.x = x;
            e.y = y;
            return e;
        }

        const Registration kHint{ SectionInfo{
            "hint",
            [] { return std::make_unique<HintSection>(); },
            {
                State{ "rest", {} },
                State{ "expired", [](HeadlessHost& h, Panel&) { h.tick(361); } },
                State{ "moved",
                       [](HeadlessHost& h, Panel&) {
                           h.move(400.0f, 10.0f);
                           h.tick(6);
                       } },
                State{ "cut",
                       [](HeadlessHost& h, Panel&) {
                           h.move(400.0f, 10.0f);
                           h.tick(30);
                       } },
                State{ "skip", [](HeadlessHost&, Panel& p) { static_cast<HintSection&>(p).skip(); } },
                State{ "chrome",
                       [](HeadlessHost&, Panel& p) {
                           p.pointerDown(at(400.0f, 10.0f));    // a down with no move ever (a touch screen)
                           p.pointerUp(at(400.0f, 10.0f));
                       } },
                State{ "chrome-moved",
                       [](HeadlessHost& h, Panel& p) {
                           p.pointerDown(at(400.0f, 10.0f));
                           p.pointerUp(at(400.0f, 10.0f));
                           h.move(400.0f, 20.0f);
                       } },
            } } };
    }
}

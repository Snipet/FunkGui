// FUNKGUI_TEST name=fg.gallery.zoom timeout=600 gpu=0 exe=FunkGuiGalleryProbe args="--section zoom"
//
// The "zoom" gallery section (G7c, v0.8.0; FCompressor ADR-68, docs/sprints/s11.md "G7c"): the UI zoom as a Panel
// sees it. A ZOOM row of text cells over HostServices::zoomSteps(), the active one HostServices::zoomPercent(), each
// click HostServices::setZoomPercent() (a machine-wide preference, not a host parameter: no gesture); a readout of
// what the host says and of the calls made (SET n), also an a11y item; and density samples that show what the zoom
// does to the render: hairlines 1.5 logical px apart (each snaps to its own device px, whatever the effective scale),
// a 1 px outline round a radius-4 rectangle, a 1 px diagonal and 10 px text. The Panel draws all of it in its own
// logical px: only the host's window, drawable and input mapping change with the zoom.
//
// Live (tools/GalleryApp: EditorHost with zoom steps 100 / 125 / 150 / 175, default 100, preference "uiZoom"): a click
// resizes the window at once, every other gallery window follows, and the choice persists across launches.
// FUNKGUI_UI_ZOOM=<percent> pins any section's zoom for a capture (not persisted).
//
// Headless (this test): HeadlessHost stays logical and only simulates the three calls (HeadlessHost::setZoom), so the
// frames and a11y lines below never depend on a zoom; they pin the control. States: none (the host has no zoom: the
// row is empty and the readout says so), rest (steps 100 / 125 / 150 / 175 at 125), hover (150 under the pointer,
// ink100), select (a click on 150: the host takes it), same (a click on the active 125: no call, tap semantics),
// keys (Tab to the row, → selects 150; focus ring), keys-end (Tab, End selects 175). The line above registers the
// test; the tools glob compiles this file into FunkGuiGalleryProbe and FunkGuiGalleryApp.

#include "GalleryPanel.h"

#include <funkgui/canvas/Tags.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/widgets/SegmentedSelector.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace funkgui::gallery
{
    namespace
    {
        constexpr int      kMaxCells = 6;            // a longer step list shows its first six
        constexpr float    kLeft = 16.0f, kRowY = 16.0f, kCellsX = 64.0f, kCellW = 28.0f, kCellGap = 2.0f;
        constexpr float    kReadoutY = 42.0f, kSampleY = 62.0f;
        constexpr Point    kOutside{ -1000.0f, -1000.0f };
        constexpr uint32_t kIdCells = 100, kIdReadout = 900;
        const std::vector<int> kSteps{ 100, 125, 150, 175 };   // FCompressor's steps (ADR-68)

        float cellX(int i) { return kCellsX + static_cast<float>(i) * (kCellW + kCellGap); }

        class ZoomSection final : public Section, private CellModel
        {
        public:
            ZoomSection() : Section(360, 132) {}

            void attach(HostServices& host) override
            {
                Section::attach(host);
                refresh();                           // EditorHost lists a11y before the first tick
            }

            void tick(float dt) override
            {
                refresh();
                if (sel_)
                    sel_->tick(dt, hasPointer_ ? pointer_ : kOutside);
                dirty_ = false;
            }

            // A change of the host's steps (HeadlessHost::setZoom in a state's script) asks for the tick that rebuilds.
            bool wantsFullRate() const override { return dirty_ || stepsChanged() || (sel_ && !sel_->settled()); }

            void pointerMove(const PointerEvent& e) override
            {
                refresh();
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
                refresh();
                ring_ = false;
                pointer_ = { e.x, e.y };
                hasPointer_ = true;
                if (sel_ && sel_->contains(pointer_) && gestures() != nullptr)
                    sel_->pointerDown(e, *gestures());
                dirty_ = true;
            }

            bool key(const KeyEvent& e) override
            {
                refresh();
                dirty_ = true;
                if (e.key == Key::tab)
                {
                    ring_ = !ring_;                  // the row is the section's only Tab stop
                    return true;
                }
                if (!ring_ || !sel_ || gestures() == nullptr)
                    return false;
                return sel_->key(e, *gestures());
            }

            Cursor cursor() const override
            {
                return sel_ && hasPointer_ && sel_->contains(pointer_) ? sel_->cursorAt(pointer_) : Cursor::normal;
            }

            void accessibility(std::vector<A11yItem>& out) const override
            {
                if (sel_)
                    sel_->accessibility(out);
                A11yItem r;
                r.id = kIdReadout;
                r.role = A11yRole::staticText;
                r.bounds = { kLeft, kReadoutY, 328.0f, 14.0f };
                r.title = "ZOOM";
                r.value = readout();
                r.readOnly = true;
                out.push_back(r);
            }

            uint32_t a11yRevision() const override { return rebuilds_; }

            void a11yAction(uint32_t id, A11yAction a, double value) override
            {
                refresh();
                if (sel_ && gestures() != nullptr)
                    sel_->a11yAction(id, a, value, *gestures());
                dirty_ = true;
            }

            void draw(Canvas& c, const Theme& th) override
            {
                if (sel_)
                    sel_->draw(c, th, ring_);
                else
                {
                    const Canvas::Scope s(c, tags::cell, false);
                    c.text("ZOOM", kLeft, kRowY + 4.0f, type::kCaption, th.ink52);
                }
                if (shown_ > 0)
                {
                    const Canvas::Scope s(c, tags::cell, false);   // the unit after the cells
                    c.text("%", cellX(shown_) + 2.0f, c.capCentreTop(kRowY + 8.0f, type::kCaption), type::kCaption,
                           th.ink32);
                }
                {
                    const Canvas::Scope s(c, tags::hint, false);
                    c.text(readout().c_str(), kLeft, kReadoutY, type::kMicro, th.ink32);
                }

                // Density samples: what a zoom changes is where these land in device px, never these calls.
                for (int i = 0; i < 8; ++i)
                {
                    const float d = 1.5f * static_cast<float>(i);
                    c.hairlineH(kLeft, kSampleY + d, 96.0f, th.ink52);
                    c.hairlineV(128.0f + d, kSampleY, 12.0f, th.ink52);
                }
                c.rrect(160.0f, kSampleY, 64.0f, 24.0f, 4.0f, th.ink16, 1.0f, th.ink70);
                c.segment(236.0f, kSampleY + 24.0f, 330.0f, kSampleY, 1.0f, th.accent);
                c.text("HAIRLINES 1.5 PX APART, 10 PX TEXT", kLeft, kSampleY + 36.0f, type::kMicro, th.ink70);
                c.text("THE PANEL DRAWS IN LOGICAL PX", kLeft, kSampleY + 52.0f, type::kCaption, th.ink52);
            }

        private:
            // CellModel over the host's zoom.
            int count() const override { return shown_; }

            int active() const override
            {
                const int z = host() != nullptr ? host()->zoomPercent() : 100;
                for (int i = 0; i < shown_; ++i)
                    if (steps_[static_cast<size_t>(i)] == z)
                        return i;
                return -1;                           // a pinned zoom that is not a step: no cell active
            }

            const char* label(int i) const override { return labels_[static_cast<size_t>(i)].data(); }
            const char* spoken(int i) const override { return spoken_[static_cast<size_t>(i)].data(); }

            void select(int i, GestureController&) override
            {
                if (host() == nullptr || i < 0 || i >= shown_ || i == active())
                    return;                          // tap semantics: nothing when the cell is already active
                host()->setZoomPercent(steps_[static_cast<size_t>(i)]);
                ++calls_;
            }

            bool stepsChanged() const
            {
                if (!built_)
                    return true;
                const std::span<const int> now =
                    host() != nullptr ? host()->zoomSteps() : std::span<const int>{};
                return !std::equal(now.begin(), now.end(), steps_.begin(), steps_.end());
            }

            // The selector follows the host's steps: rebuilt (and the a11y structure revised) when they change.
            void refresh()
            {
                if (!stepsChanged())
                    return;
                std::vector<int> now;
                if (host() != nullptr)
                {
                    const std::span<const int> s = host()->zoomSteps();
                    now.assign(s.begin(), s.end());
                }
                built_ = true;
                steps_ = std::move(now);
                shown_ = static_cast<int>(std::min<size_t>(steps_.size(), kMaxCells));
                std::vector<Rect> cells;
                for (int i = 0; i < shown_; ++i)
                {
                    const int v = steps_[static_cast<size_t>(i)];
                    std::snprintf(labels_[static_cast<size_t>(i)].data(), labels_[0].size(), "%d", v);
                    std::snprintf(spoken_[static_cast<size_t>(i)].data(), spoken_[0].size(), "%d percent", v);
                    cells.push_back({ cellX(i), kRowY, kCellW, 16.0f });
                }
                sel_.reset();
                if (shown_ > 0)
                {
                    sel_.emplace(static_cast<CellModel&>(*this), std::move(cells), CellStyle::text, "ZOOM",
                                 Point{ kLeft, kRowY + 4.0f }, kIdCells);
                    sel_->setSpokenTitle("Interface zoom");
                }
                ++rebuilds_;
            }

            std::string readout() const
            {
                const int z = host() != nullptr ? host()->zoomPercent() : 100;
                if (host() == nullptr || host()->zoomSteps().empty())
                    return "NO ZOOM STEPS: THIS HOST DRAWS AT " + std::to_string(z) + " %";
                std::string s = "DRAWN AT " + std::to_string(z) + " %   STEPS";
                for (const int v : host()->zoomSteps())
                    s += " " + std::to_string(v);
                return s + "   SET " + std::to_string(calls_);
            }

            std::vector<int> steps_;
            int  shown_ = 0;
            bool built_ = false;
            uint32_t rebuilds_ = 0;
            int  calls_ = 0;                         // setZoomPercent calls made (the readout's SET n)
            std::array<std::array<char, 8>, kMaxCells>  labels_{};
            std::array<std::array<char, 24>, kMaxCells> spoken_{};
            std::optional<SegmentedSelector> sel_;
            Point pointer_{};
            bool  hasPointer_ = false, ring_ = false, dirty_ = false;
        };

        // Cell centres: 100 at x 78, 125 at 108, 150 at 138, 175 at 168; the row's centre line is y 24.
        const Registration kZoom{ SectionInfo{
            "zoom",
            [] { return std::make_unique<ZoomSection>(); },
            {
                State{ "none", {} },
                State{ "rest", [](HeadlessHost& h, Panel&) { h.setZoom(kSteps, 125); } },
                State{ "hover", [](HeadlessHost& h, Panel&) {
                          h.setZoom(kSteps, 125);
                          h.move(138.0f, 24.0f);
                      } },
                State{ "select", [](HeadlessHost& h, Panel&) {
                          h.setZoom(kSteps, 125);
                          h.click(138.0f, 24.0f);
                      } },
                State{ "same", [](HeadlessHost& h, Panel&) {
                          h.setZoom(kSteps, 125);
                          h.click(108.0f, 24.0f);
                      } },
                State{ "keys", [](HeadlessHost& h, Panel&) {
                          h.setZoom(kSteps, 125);
                          h.keys("tab,right");
                      } },
                State{ "keys-end", [](HeadlessHost& h, Panel&) {
                          h.setZoom(kSteps, 125);
                          h.keys("tab,end");
                      } },
            } } };
    }
}

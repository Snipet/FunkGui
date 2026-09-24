// FUNKGUI_TEST name=fg.gallery.theme timeout=600 gpu=0 exe=FunkGuiGalleryProbe args="--section theme"
//
// The "theme" gallery section (FCompressor docs/design/02-funkgui-and-ui.md §5.5, §5.9, §6.3, §6.5; Q7; G6): the
// machine-wide preferences as cells. ThemeCells (GRAPHITE / PAPER, HR's theme cells) and two PrefCells groups over
// generic int keys, as FCompressor's band captions use them: the meter SCALE (meterScaleDb 12 / 24 / 48 / 72, default
// 48) and the history SPAN (historySpanTenths 25 / 50 / 100 / 200, default 50), plus a log line that reads the three
// values back from UiPreferences (also an a11y item). The host's own theme is not the preference: selecting PAPER here
// moves the active cell and writes the store, and the frame's palette stays the host's (theme invariance still holds,
// as the cells' geometry does not depend on which is active).
//
// The store: every section is constructed against a scratch store — <ENV_PREFIX>PREFS_DIR when the caller set it (CTest
// points it into the test's sandbox), else a fresh directory under the system temp directory, set before the store
// is first opened — and resets the three keys, so every state starts from GRAPHITE / 48 / 50 whatever an earlier
// state or run wrote, and the gallery never touches the real preferences.
//
// States: rest, hover (PAPER ink100), select (PAPER: the store's theme becomes 1), same (GRAPHITE, already active: no
// write), scale (24), span (20 S), keys (Tab to SCALE, → selects 72; focus ring), keys-theme (Tab to THEME, End
// selects PAPER). The line above registers the test; the tools glob compiles this file into FunkGuiGalleryProbe.

#include "GalleryPanel.h"

#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Config.h>
#include <funkgui/core/Env.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/test/Harness.h>
#include <funkgui/widgets/SegmentedSelector.h>
#include <funkgui/widgets/ThemeCells.h>

#include <juce_core/juce_core.h>

#include <array>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace funkgui::gallery
{
    namespace
    {
        constexpr const char* kScaleKey = "meterScaleDb";
        constexpr const char* kSpanKey = "historySpanTenths";
        constexpr float kLeft = 16.0f, kLogY = 92.0f;
        constexpr Point kOutside{ -1000.0f, -1000.0f };
        constexpr uint32_t kIdLog = 900;

        // Before the store is first opened in this process: make sure it is a scratch one.
        void scratchStore()
        {
            if (funkgui::env("PREFS_DIR") != nullptr)
                return;
            const juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                       .getNonexistentChildFile("FunkGuiGalleryPrefs", "", false);
            dir.createDirectory();
            test::setEnv(FUNKGUI_ENV_PREFIX "PREFS_DIR", dir.getFullPathName().toRawUTF8());
            funkgui::envReload();
        }

        // Runs before the members that open the store.
        struct ResetStore
        {
            ResetStore()
            {
                scratchStore();
                auto& prefs = UiPreferences::get();
                prefs.setTheme(0);
                prefs.setInt(kScaleKey, 48);
                prefs.setInt(kSpanKey, 50);
            }
        };

        class ThemeSection final : public Section
        {
        public:
            ThemeSection()
                : Section(360, 112),
                  scale_(kScaleKey, { 12, 24, 48, 72 },
                         { { "12", "12 decibels" }, { "24", "24 decibels" }, { "48", "48 decibels" },
                           { "72", "72 decibels" } }, 48),
                  span_(kSpanKey, { 25, 50, 100, 200 },
                        { { "2.5", "2.5 seconds" }, { "5", "5 seconds" }, { "10", "10 seconds" }, { "20", "20 seconds" } },
                        50),
                  scaleSel_(scale_, { { 82, 16, 20, 16 }, { 106, 16, 20, 16 }, { 130, 16, 20, 16 }, { 154, 16, 20, 16 } },
                            CellStyle::text, "SCALE", { kLeft, 20 }, 100),
                  spanSel_(span_, { { 82, 42, 26, 16 }, { 112, 42, 16, 16 }, { 132, 42, 20, 16 }, { 156, 42, 20, 16 } },
                           CellStyle::text, "SPAN", { kLeft, 46 }, 200),
                  theme_({ { 196, 68, 72, 16 }, { 272, 68, 54, 16 } }, nullptr, {}, 300)
            {
                scaleSel_.setSpokenTitle("Meter scale");
                spanSel_.setSpokenTitle("History span");
            }

            void tick(float dt) override
            {
                const Point p = hasPointer_ ? pointer_ : kOutside;
                for (SegmentedSelector* s : sels())
                    s->tick(dt, p);
                dirty_ = false;
            }

            bool wantsFullRate() const override
            {
                if (dirty_)
                    return true;
                for (const SegmentedSelector* s : sels())
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
                ring_ = false;
                pointer_ = { e.x, e.y };
                hasPointer_ = true;
                for (SegmentedSelector* s : sels())
                    if (s->contains(pointer_))
                        s->pointerDown(e, *gestures());
                dirty_ = true;
            }

            bool key(const KeyEvent& e) override
            {
                dirty_ = true;
                const auto all = sels();
                const int n = static_cast<int>(all.size());
                if (e.key == Key::tab)
                {
                    const int dir = e.mods.shift ? -1 : 1;
                    focus_ = focus_ < 0 ? (dir > 0 ? 0 : n - 1) : (focus_ + dir + n) % n;
                    ring_ = true;
                    return true;
                }
                if (focus_ < 0)
                    return false;
                return all[static_cast<size_t>(focus_)]->key(e, *gestures());
            }

            Cursor cursor() const override
            {
                for (const SegmentedSelector* s : sels())
                    if (hasPointer_ && s->contains(pointer_))
                        return s->cursorAt(pointer_);
                return Cursor::normal;
            }

            void accessibility(std::vector<A11yItem>& out) const override
            {
                for (const SegmentedSelector* s : sels())
                    s->accessibility(out);
                A11yItem log;
                log.id = kIdLog;
                log.role = A11yRole::staticText;
                log.bounds = { kLeft, kLogY, 328, 14 };
                log.title = "STORE";
                log.value = logText();
                log.readOnly = true;
                out.push_back(log);
            }

            void a11yAction(uint32_t id, A11yAction a, double value) override
            {
                for (SegmentedSelector* s : sels())
                    if (s->a11yAction(id, a, value, *gestures()))
                        break;
                dirty_ = true;
            }

            void draw(Canvas& c, const Theme& th) override
            {
                const auto all = sels();
                for (size_t i = 0; i < all.size(); ++i)
                    all[i]->draw(c, th, ring_ && focus_ == static_cast<int>(i));
                {
                    const Canvas::Scope s(c, tags::cell, false);   // the units after the cells (02 §6.3 "… 72 DB")
                    c.text("DB", 180.0f, c.capCentreTop(24.0f, type::kCaption), type::kCaption, th.ink32);
                    c.text("S", 182.0f, c.capCentreTop(50.0f, type::kCaption), type::kCaption, th.ink32);
                }
                const Canvas::Scope s(c, tags::hint, false);
                c.text(logText().c_str(), kLeft, kLogY, type::kMicro, th.ink32);
            }

        private:
            std::array<SegmentedSelector*, 3> sels() { return { &scaleSel_, &spanSel_, &theme_.selector() }; }
            std::array<const SegmentedSelector*, 3> sels() const { return { &scaleSel_, &spanSel_, &theme_.selector() }; }

            static std::string logText()
            {
                const auto& prefs = UiPreferences::get();
                char buf[96];
                std::snprintf(buf, sizeof buf, "THEME %d   SCALE %d   SPAN %d", prefs.theme(),
                              prefs.getInt(kScaleKey, -1, -1000, 1000), prefs.getInt(kSpanKey, -1, -1000, 1000));
                return buf;
            }

            ResetStore        reset_;                    // first: the store is scratch and reset before any cell opens it
            PrefCells         scale_, span_;
            SegmentedSelector scaleSel_, spanSel_;
            ThemeCells        theme_;
            Point pointer_{};
            bool  hasPointer_ = false, ring_ = false, dirty_ = false;
            int   focus_ = -1;
        };

        const Registration kTheme{ SectionInfo{
            "theme",
            [] { return std::make_unique<ThemeSection>(); },
            {
                State{ "rest", {} },
                State{ "hover", [](HeadlessHost& h, Panel&) { h.move(299.0f, 76.0f); } },
                State{ "select", [](HeadlessHost& h, Panel&) { h.click(299.0f, 76.0f); } },
                State{ "same", [](HeadlessHost& h, Panel&) { h.click(232.0f, 76.0f); } },
                State{ "scale", [](HeadlessHost& h, Panel&) { h.click(116.0f, 24.0f); } },
                State{ "span", [](HeadlessHost& h, Panel&) { h.click(166.0f, 50.0f); } },
                State{ "keys", [](HeadlessHost& h, Panel&) { h.keys("tab,right"); } },
                State{ "keys-theme", [](HeadlessHost& h, Panel&) { h.keys("tab,tab,tab,end"); } },
            } } };
    }
}

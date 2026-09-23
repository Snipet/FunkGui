#pragma once

// The widget gallery (FCompressor docs/design/02-funkgui-and-ui.md §3.11; SPRINTS.md §7 D10): self-registering
// sections, one per widget card, and the Panel that shows one of them. tools/GalleryProbe.cpp runs a section headless
// through HeadlessHost as the test fg.gallery.<section>; G7's gallery Standalone runs the same GalleryPanel live, so a
// live capture and a headless frame of one section and state can be compared.
//
// Adding a section (a widget card; nothing else is touched, so no other section's goldens move):
//
//   test/gallery/<Widget>Gallery.cpp, whose FIRST line registers its test (test/CMakeLists.txt compiles nothing for a
//   FUNKGUI_TEST line with exe=; the tools glob compiles every test/gallery/*.cpp into FunkGuiGalleryProbe):
//
//     // FUNKGUI_TEST name=fg.gallery.<section> timeout=600 gpu=0 exe=FunkGuiGalleryProbe args="--section <section>"
//     #include "GalleryPanel.h"
//     namespace {
//     class MySection final : public funkgui::gallery::Section {
//     public:
//         MySection() : Section(360, 120) {}
//         void draw(funkgui::Canvas& c, const funkgui::Theme& th) override { … }       // absolute px from (0, 0)
//         …                                                                            // input, tick, a11y as needed
//     };
//     const funkgui::gallery::Registration reg{ { "<section>", [] { return std::make_unique<MySection>(); },
//         { { "rest", {} },
//           { "hover", [](funkgui::HeadlessHost& h, funkgui::Panel&) { h.move(40, 20); } } } } };
//     }
//
// What GalleryProbe checks per state (fresh section each time; settled before and after the script; dpi 1 and 2;
// theme 0 and 1): the fingerprint as golden rows "<state>.dpi<d>.*" (Fingerprint.h addMetrics), theme invariance,
// zero missing glyphs, the dump v2 round trip (write -> parse -> bit-equal primitives and equal fingerprint) as spec
// rows, and the visible a11y items as the golden lines "a11y.<state>" (a11yDumpLine), which must not vary with dpi or
// theme. A section and state must settle within 600 frames at 1/60 s (a harness error otherwise).
//
// Names: sections and states are [a-z0-9_-]+ (they become test names and golden keys); a section needs at least one
// state. Sections draw at the origin in absolute logical px (no transform stack, HR rule), at their own fixed size.

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Theme.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Input.h>
#include <funkgui/panel/Panel.h>
#include <funkgui/params/GestureController.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace funkgui::gallery
{
    // A section's base: a fixed-size Panel with no-op defaults for everything but draw(), the host it was attached to
    // and a GestureController over that host (poll()ed from idle, closed by closeGestures()).
    class Section : public Panel
    {
    public:
        Section(int w, int h) : w_(w), h_(h) {}

        void attach(HostServices& host) override
        {
            host_ = &host;
            gestures_.emplace(host);
        }
        int  width() const override { return w_; }
        int  height() const override { return h_; }
        void tick(float) override {}
        void idle(double nowSec) override
        {
            if (gestures_)
                gestures_->poll(nowSec);
        }
        bool wantsFullRate() const override { return false; }
        void accessibility(std::vector<A11yItem>&) const override {}
        uint32_t a11yRevision() const override { return 0; }
        void a11yAction(uint32_t, A11yAction, double) override {}
        void closeGestures() override
        {
            if (gestures_)
                gestures_->closeAll();
        }

    protected:
        HostServices* host() const noexcept { return host_; }                 // nullptr before attach()
        GestureController* gestures() noexcept { return gestures_ ? &*gestures_ : nullptr; }

    private:
        int w_, h_;
        HostServices* host_ = nullptr;
        std::optional<GestureController> gestures_;
    };

    // A scripted state: input replayed through the host on a freshly made, settled section.
    struct State
    {
        std::string name;                                            // [a-z0-9_-]+
        std::function<void(HeadlessHost&, Panel&)> script;           // empty: the section at rest
    };

    struct SectionInfo
    {
        std::string name;                                            // [a-z0-9_-]+: fg.gallery.<name>, --section
        std::function<std::unique_ptr<Panel>()> make;
        std::vector<State> states;
    };

    // Registers a section during static initialisation: one namespace-scope Registration per <Widget>Gallery.cpp.
    struct Registration
    {
        explicit Registration(SectionInfo info);
    };

    // Every registered section, sorted by name.
    const std::vector<SectionInfo>& sections();

    // The section called `name`, or nullptr.
    const SectionInfo* findSection(std::string_view name);

    // Whether s is a legal section or state name: [a-z0-9_-]+, at most 48 characters.
    bool validName(std::string_view s);

    // The Panel a host runs: one section at a time, at the origin and at the section's own size. show() makes a fresh
    // section (G7's gallery app switches through them); every call reaches the current section unchanged.
    class GalleryPanel final : public Panel
    {
    public:
        explicit GalleryPanel(const SectionInfo& first);

        // Replaces the section with a fresh one of `info` and attaches it to the host, if there is one yet. Bumps the
        // a11y revision.
        void   show(const SectionInfo& info);
        Panel& section() noexcept { return *section_; }
        const SectionInfo& info() const noexcept { return *info_; }

        void  attach(HostServices&) override;
        int   width() const override;
        int   height() const override;
        void  tick(float dt) override;
        void  idle(double nowSec) override;
        void  draw(Canvas&, const Theme&) override;
        bool  wantsFullRate() const override;
        void  pointerMove(const PointerEvent&) override;
        void  pointerExit() override;
        void  pointerDown(const PointerEvent&) override;
        void  pointerDrag(const PointerEvent&) override;
        void  pointerUp(const PointerEvent&) override;
        void  doubleClick(const PointerEvent&) override;
        bool  wheel(const WheelEvent&) override;
        bool  key(const KeyEvent&) override;
        Cursor cursor() const override;
        void  accessibility(std::vector<A11yItem>&) const override;
        uint32_t a11yRevision() const override;
        void  a11yAction(uint32_t id, A11yAction, double value = 0) override;
        void  closeGestures() override;
        bool  filesInterest(const std::vector<std::string>&) const override;
        void  filesDropped(const std::vector<std::string>&) override;

    private:
        const SectionInfo*     info_ = nullptr;
        std::unique_ptr<Panel> section_;
        HostServices*          host_ = nullptr;
        uint32_t               switches_ = 0;                        // added to the section's a11y revision
    };
}

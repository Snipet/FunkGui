// FUNKGUI_TEST name=fg.gallery.primitives timeout=600 gpu=0 exe=FunkGuiGalleryProbe args="--section primitives"
//
// The gallery machinery (GalleryPanel.h) and its one built-in section, "primitives": HR's primitives as the recorder
// draws them (every rrect variant, hairlines at fractional positions, capsule segments, the type scale with its three
// alignments and the extra glyphs, tags through Canvas::Scope), one live element, and a small interactive latch with a
// hover ease, a click, keyboard focus and an a11y item, so GalleryProbe's states, settle, theme invariance, dump round
// trip and a11y lines are exercised before any widget card adds its own section. The line above registers the test
// fg.gallery.primitives (this file is compiled into FunkGuiGalleryProbe by the tools glob, not by test/CMakeLists.txt).

#include "GalleryPanel.h"

#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Col.h>
#include <funkgui/core/Ease.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/core/TypeScale.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace funkgui::gallery
{
    // ---- registry ---------------------------------------------------------------------------------------------------

    namespace
    {
        std::vector<SectionInfo>& registry()
        {
            static std::vector<SectionInfo> r;           // filled during static initialisation, read from main()
            return r;
        }
    }

    bool validName(std::string_view s)
    {
        if (s.empty() || s.size() > 48)
            return false;
        return std::all_of(s.begin(), s.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        });
    }

    Registration::Registration(SectionInfo info)
    {
        auto& r = registry();
        const auto at = std::lower_bound(r.begin(), r.end(), info.name,
                                         [](const SectionInfo& s, const std::string& n) { return s.name < n; });
        r.insert(at, std::move(info));                   // a duplicate name is reported by GalleryProbe
    }

    const std::vector<SectionInfo>& sections() { return registry(); }

    const SectionInfo* findSection(std::string_view name)
    {
        for (const SectionInfo& s : registry())
            if (s.name == name)
                return &s;
        return nullptr;
    }

    // ---- GalleryPanel -----------------------------------------------------------------------------------------------

    GalleryPanel::GalleryPanel(const SectionInfo& first) : info_(&first), section_(first.make()) {}

    void GalleryPanel::show(const SectionInfo& info)
    {
        if (section_)
            section_->closeGestures();
        info_ = &info;
        section_ = info.make();
        ++switches_;
        if (host_ != nullptr)
            section_->attach(*host_);
    }

    void GalleryPanel::attach(HostServices& host)
    {
        host_ = &host;
        section_->attach(host);
    }

    int   GalleryPanel::width() const { return section_->width(); }
    int   GalleryPanel::height() const { return section_->height(); }
    void  GalleryPanel::tick(float dt) { section_->tick(dt); }
    void  GalleryPanel::idle(double nowSec) { section_->idle(nowSec); }
    void  GalleryPanel::draw(Canvas& c, const Theme& th) { section_->draw(c, th); }
    bool  GalleryPanel::wantsFullRate() const { return section_->wantsFullRate(); }
    void  GalleryPanel::pointerMove(const PointerEvent& e) { section_->pointerMove(e); }
    void  GalleryPanel::pointerExit() { section_->pointerExit(); }
    void  GalleryPanel::pointerDown(const PointerEvent& e) { section_->pointerDown(e); }
    void  GalleryPanel::pointerDrag(const PointerEvent& e) { section_->pointerDrag(e); }
    void  GalleryPanel::pointerUp(const PointerEvent& e) { section_->pointerUp(e); }
    void  GalleryPanel::doubleClick(const PointerEvent& e) { section_->doubleClick(e); }
    bool  GalleryPanel::wheel(const WheelEvent& e) { return section_->wheel(e); }
    bool  GalleryPanel::key(const KeyEvent& e) { return section_->key(e); }
    Cursor GalleryPanel::cursor() const { return section_->cursor(); }
    void  GalleryPanel::accessibility(std::vector<A11yItem>& out) const { section_->accessibility(out); }
    uint32_t GalleryPanel::a11yRevision() const { return section_->a11yRevision() + switches_; }
    void  GalleryPanel::a11yAction(uint32_t id, A11yAction a, double value) { section_->a11yAction(id, a, value); }
    void  GalleryPanel::closeGestures() { section_->closeGestures(); }

    bool GalleryPanel::filesInterest(const std::vector<std::string>& files) const
    {
        return section_->filesInterest(files);
    }

    void GalleryPanel::filesDropped(const std::vector<std::string>& files) { section_->filesDropped(files); }

    // ---- the built-in section: primitives ---------------------------------------------------------------------------

    namespace
    {
        constexpr uint32_t kIdTitle = 1;
        constexpr uint32_t kIdLatch = 2;
        constexpr Rect     kLatch{ 360.0f, 200.0f, 100.0f, 36.0f };

        class PrimitivesSection final : public Section
        {
        public:
            PrimitivesSection() : Section(480, 300) {}

            void tick(float dt) override
            {
                seconds_ += dt;
                hover_ = ease::hover(hover_, hovered_, dt);
                fill_ = ease::toward(fill_, on_ ? 1.0f : 0.0f, dt, 0.06f);
            }

            bool wantsFullRate() const override
            {
                return !ease::sameBits(hover_, hovered_ ? 1.0f : 0.0f) || !ease::sameBits(fill_, on_ ? 1.0f : 0.0f);
            }

            void pointerMove(const PointerEvent& e) override { hovered_ = kLatch.contains({ e.x, e.y }); }
            void pointerExit() override { hovered_ = false; }

            void pointerDown(const PointerEvent& e) override
            {
                focused_ = false;                        // the pointer is the affordance now (HR)
                if (!e.popup && kLatch.contains({ e.x, e.y }))
                    on_ = !on_;
            }

            bool key(const KeyEvent& e) override
            {
                if (e.key == Key::tab)
                {
                    focused_ = !focused_;
                    return true;
                }
                if (focused_ && (e.key == Key::space || e.key == Key::enter))
                {
                    on_ = !on_;
                    return true;
                }
                return false;
            }

            void accessibility(std::vector<A11yItem>& out) const override
            {
                A11yItem title;
                title.id = kIdTitle;
                title.role = A11yRole::staticText;
                title.bounds = { 16.0f, 264.0f, 200.0f, 20.0f };
                title.title = "PRIMITIVES";
                title.readOnly = true;
                out.push_back(title);

                A11yItem latch;
                latch.id = kIdLatch;
                latch.role = A11yRole::toggleButton;
                latch.bounds = kLatch;
                latch.title = "LATCH";
                latch.help = "Toggles the latch";
                latch.checkable = true;
                latch.checked = on_;
                out.push_back(latch);
            }

            void a11yAction(uint32_t id, A11yAction a, double) override
            {
                if (id == kIdLatch && (a == A11yAction::press || a == A11yAction::toggle))
                    on_ = !on_;
            }

            void draw(Canvas& c, const Theme& th) override
            {
                drawRrects(c, th);
                drawLines(c, th);
                drawText(c, th);
                drawLatch(c, th);
                drawLive(c, th);
            }

        private:
            static void drawRrects(Canvas& c, const Theme& th)
            {
                c.rrect(16.0f, 16.0f, 40.0f, 24.0f, 0.0f, th.ink16);                        // square corners
                c.rrect(64.0f, 16.0f, 40.0f, 24.0f, 6.0f, th.ink32);                        // radius
                c.rrect(112.0f, 16.0f, 56.0f, 24.0f, 9999.0f, th.ink52);                    // pill (radius clamped)
                c.rrect(176.0f, 16.0f, 24.0f, 24.0f, 12.0f, th.ink70.withAlpha(0.0f), 1.5f, th.ink70);   // ring
                c.rrect(208.0f, 16.0f, 40.0f, 24.0f, 4.0f, th.accentDim, 1.0f, th.accent);  // fill + border
                c.rrect4(256.0f, 16.0f, 48.0f, 24.0f, 0.0f, 8.0f, 2.0f, 12.0f, th.ink32);   // per corner
                c.rrect(320.5f, 20.25f, 30.0f, 16.0f, 3.0f, th.signal, 0.0f, {}, 4.0f);     // softness, fractional
                c.rrect(372.0f, 16.0f, 0.0f, 24.0f, 2.0f, th.ink100);                        // empty: no primitive
            }

            static void drawLines(Canvas& c, const Theme& th)
            {
                {
                    const Canvas::Scope track(c, tags::slotTrack, false);
                    c.hairlineH(16.0f, 60.3f, 200.0f, th.ink16);                             // floors to a device px
                    c.hairlineV(230.7f, 56.0f, 40.0f, th.ink16);
                    for (int k = 0; k < 12; ++k)                                             // device-px dots
                        c.rrect(c.snapX(250.0f + 6.4f * static_cast<float>(k)), c.snapY(60.3f), 1.0f / c.dpi(),
                                1.0f / c.dpi(), 0.0f, th.ink32);
                }
                c.segment(16.0f, 100.0f, 90.0f, 70.0f, 1.5f, th.ink70);                      // diagonal
                c.segment(100.0f, 72.0f, 100.0f, 104.0f, 2.0f, th.ink52);                    // vertical
                c.segment(200.0f, 88.0f, 120.0f, 88.0f, 1.0f, th.ink100);                    // reversed horizontal
                c.segment(220.0f, 80.0f, 240.0f, 80.0f, 0.0f, th.ink100);                    // zero width: nothing
                const float xs[] = { 250.0f, 270.5f, 290.0f, 310.25f, 330.0f };
                const float ys[] = { 104.0f, 74.0f, 96.5f, 70.0f, 90.0f };
                const Col curve = premix(th.ground, th.ink100, 0.6f);                        // opaque: no beads
                for (int k = 0; k + 1 < 5; ++k)
                    c.segment(xs[k], ys[k], xs[k + 1], ys[k + 1], 1.25f, curve, 0.5f);
            }

            static void drawText(Canvas& c, const Theme& th)
            {
                const Canvas::Scope values(c, tags::slotValue, false);
                // ASCII '-': the atlas has no U+2212 until G4's Glyphs.def (a missing glyph fails the section).
                const float top = 150.0f;
                const float w = c.textWidth("-12.5", type::kDisplay);
                c.text("-12.5", 16.0f, top, type::kDisplay, th.ink100);
                c.text("DB", 16.0f + w + 6.0f, c.sharedBaselineTop(top, type::kDisplay, type::kUnit), type::kUnit,
                       th.ink52);
                c.text("4:1", 200.0f, 124.0f, type::kValueP, th.ink100);
                c.text("120 ms", 200.0f, 156.0f, type::kValueS, th.ink70);
                c.text("0123456789", 200.0f, 184.0f, type::kNumeral, th.ink70);

                const Canvas::Scope labels(c, tags::slotLabel, false);
                c.text("THRESHOLD", 16.0f, 250.0f, type::kLabel, th.ink52);
                c.text("ATTACK", 180.0f, 250.0f, type::kCaption, th.ink52, Align::centre);
                c.text("RATIO", 340.0f, 250.0f, type::kMicro, th.ink32, Align::right);
                c.text("\xC2\xB0 \xC2\xB1 \xC3\x97 \xE2\x80\x93 \xE2\x86\x92 \xE2\x88\x9E \xC2\xB7 \xE2\x86\x91 "
                       "\xE2\x86\x93", 360.0f, 124.0f, type::kLabel, th.ink70);             // HR's nine extras
                c.text("PRIMITIVES", 16.0f, c.capCentreTop(274.0f, type::kWordmark), type::kWordmark, th.ink100);
            }

            void drawLatch(Canvas& c, const Theme& th) const
            {
                const Canvas::Scope latch(c, tags::latch, false);
                c.rrect(kLatch.x, kLatch.y, kLatch.w, kLatch.h, 3.0f, th.ink16.withAlpha(0.0f), 1.0f + hover_,
                        on_ ? th.accent : th.ink52);
                if (fill_ > 0.0f)
                    c.rrect(kLatch.x + 4.0f, kLatch.y + 4.0f, (kLatch.w - 8.0f) * fill_, kLatch.h - 8.0f, 2.0f,
                            th.accentDim);
                c.text("LATCH", kLatch.centreX(), c.capCentreTop(kLatch.centreY(), type::kLatch), type::kLatch,
                       on_ ? th.ink100 : th.ink52, Align::centre);
                if (focused_)
                {
                    const Canvas::Scope ring(c, tags::focusRing, false);
                    const Rect r = kLatch.expanded(3.0f);
                    c.rrect(r.x, r.y, r.w, r.h, 5.0f, th.accent.withAlpha(0.0f), 1.0f, th.accent);
                }
            }

            void drawLive(Canvas& c, const Theme& th) const
            {
                // A meter-like bar driven by the section's own clock: live, so the fingerprint leaves it out.
                const Canvas::Scope live(c, tags::none, true);
                const float h = 25.0f + 20.0f * std::sin(seconds_ * 3.14159265f);
                c.rrect(440.0f, 110.0f - h, 16.0f, h, 0.0f, th.signal);
            }

            float seconds_ = 0.0f;
            float hover_ = 0.0f, fill_ = 0.0f;
            bool  hovered_ = false, on_ = false, focused_ = false;
        };

        constexpr float kLatchX = kLatch.x + 50.0f, kLatchY = kLatch.y + 18.0f;

        const Registration kPrimitives{ SectionInfo{
            "primitives",
            [] { return std::make_unique<PrimitivesSection>(); },
            {
                State{ "rest", {} },
                State{ "hover", [](HeadlessHost& h, Panel&) { h.move(kLatchX, kLatchY); } },
                State{ "pressed", [](HeadlessHost& h, Panel&) { h.click(kLatchX, kLatchY); } },
                State{ "focus", [](HeadlessHost& h, Panel&) { h.keys("tab"); } },
                State{ "keys", [](HeadlessHost& h, Panel&) { h.keys("tab,space"); } },
            } } };
    }
}

// FUNKGUI_TEST name=fg.editorhost.zoom.live timeout=300 gpu=1 labels=live
//
// fg.editorhost.zoom.live: the UI zoom on a real window (G7c, v0.8.0; gpu/EditorHost.h "Zoom"). Labels fg;gpu;live: it
// opens two windows on the desktop and initialises bgfx on Metal, so it needs a window server and is never in
// `verify`. Run it serialised:
//   lockf -k -t 900 /tmp/fcmp-gui.lock ctest --test-dir <build-agent-gui> -L live
//
// Each editor is the content of a juce::DocumentWindow (resize-to-fit, as JUCE's Standalone and the gallery app hold
// theirs), with zoom steps 100 / 125 / 150 / 175 and a preference key in this test's sandbox (FUNKGUI_PREFS_DIR). Frames
// are pumped by calling FramePump::tick() directly, with the run loop turned between them. Per state:
//   open.*        the surface attached at 100 %: the render view covers the editor's area of its peer, and the frame's
//                 dpi is the backing scale
//   z<p>.*        after setZoomPercent(p) from the Panel: zoomPercent() answers at once and the editor keeps its size
//                 until the next frame; that frame's step 1 resizes it to round(W p) x round(H p), the window follows,
//                 the render view follows without a re-attach (same view, no new attach failure), and the FIRST frame
//                 drawn after the click already has dpi = p / 100 * backing scale (the drawable was resized before it
//                 was recorded: no stretched frame); the a11y children sit at the items' rectangles times p / 100;
//                 the second window's editor follows the preference in the same frame
//   back.*        100 % again: every size, the dpi and the a11y bounds are v0.7's
// The pointer mapping, fit, pins and persistence need no window: fg.editorhost.zoom. Spec rows only.

#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>                             // class_getName

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Theme.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/gpu/EditorHost.h>
#include <funkgui/gpu/FramePump.h>
#include <funkgui/gpu/NativeSurface.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Panel.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/test/Harness.h>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace T = funkgui::test;

namespace
{
    constexpr const char* kKey = "fgZoomLive";
    constexpr int kW = 240, kH = 160;                // whole at every step: 300 x 200, 360 x 240, 420 x 280

    class NoOpProcessor final : public juce::AudioProcessor
    {
    public:
        NoOpProcessor()
            : juce::AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
        {
        }

        const juce::String getName() const override { return "fg.editorhost.zoom.live"; }
        void prepareToPlay(double, int) override {}
        void releaseResources() override {}
        void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override { buffer.clear(); }
        double getTailLengthSeconds() const override { return 0.0; }
        bool acceptsMidi() const override { return false; }
        bool producesMidi() const override { return false; }
        bool hasEditor() const override { return false; }
        juce::AudioProcessorEditor* createEditor() override { return nullptr; }
        int getNumPrograms() override { return 1; }
        int getCurrentProgram() override { return 0; }
        void setCurrentProgram(int) override {}
        const juce::String getProgramName(int) override { return {}; }
        void changeProgramName(int, const juce::String&) override {}
        void getStateInformation(juce::MemoryBlock&) override {}
        void setStateInformation(const void*, int) override {}
    };

    // Records the dpi of every frame it draws, and lists two a11y items (one nested in the other).
    class ProbePanel final : public funkgui::Panel
    {
    public:
        funkgui::HostServices* host = nullptr;
        std::vector<float> dpis;                     // one per draw(), in order

        void attach(funkgui::HostServices& h) override { host = &h; }
        int  width() const override { return kW; }
        int  height() const override { return kH; }
        void tick(float) override {}
        void draw(funkgui::Canvas& c, const funkgui::Theme& th) override
        {
            dpis.push_back(c.dpi());
            c.rrect(12.0f, 12.0f, 216.0f, 136.0f, 6.0f, th.ink16, 1.0f, th.ink52);
            c.hairlineH(24.0f, 80.5f, 192.0f, th.ink70);
            c.text("ZOOM", 24.0f, 30.0f, funkgui::type::kCaption, th.ink100);
        }
        bool wantsFullRate() const override { return false; }
        void accessibility(std::vector<funkgui::A11yItem>& out) const override
        {
            funkgui::A11yItem g;
            g.id = 1;
            g.role = funkgui::A11yRole::radioGroup;
            g.title = "Zoom";
            g.bounds = kGroup;
            out.push_back(g);
            funkgui::A11yItem b;
            b.id = 2;
            b.parent = 1;
            b.role = funkgui::A11yRole::radioButton;
            b.title = "150 percent";
            b.bounds = kButton;
            out.push_back(b);
        }
        uint32_t a11yRevision() const override { return 1; }
        void a11yAction(uint32_t, funkgui::A11yAction, double) override {}
        void closeGestures() override {}

        static constexpr funkgui::Rect kGroup{ 20.5f, 100.25f, 120.0f, 16.0f };
        static constexpr funkgui::Rect kButton{ 50.0f, 100.25f, 30.0f, 16.0f };
    };

    NSView* renderViewIn(juce::ComponentPeer* peer)
    {
        if (peer == nullptr)
            return nil;
        NSView* parent = (NSView*) peer->getNativeHandle();
        const char* cls = funkgui::renderViewClassName();
        for (NSView* v in [parent subviews])
            if (std::strcmp(class_getName([v class]), cls) == 0)
                return v;
        return nil;
    }

    void spin(double seconds)
    {
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
    }

    double stamp = 0.0;

    void pumpFrame()
    {
        stamp += 1.0 / 60.0;
        funkgui::FramePump::get().tick(stamp);
        spin(0.02);
    }

    int zoomed(int logical, int percent) { return static_cast<int>(std::lround(logical * percent / 100.0)); }

    juce::Rectangle<int> scaled(const funkgui::Rect& r, float s)
    {
        return juce::Rectangle<float>(r.x * s, r.y * s, r.w * s, r.h * s).getSmallestIntegerContainer();
    }

    // One window holding one editor, as a Standalone holds it.
    struct Win
    {
        ProbePanel* panel = nullptr;
        std::unique_ptr<funkgui::EditorHost> editor;
        std::unique_ptr<juce::DocumentWindow> window;

        Win(juce::AudioProcessor& p, int x, int y)
        {
            funkgui::EditorConfig c;
            c.zoomSteps = { 100, 125, 150, 175 };
            c.defaultZoomPercent = 100;
            c.zoomPrefKey = kKey;
            auto owned = std::make_unique<ProbePanel>();
            panel = owned.get();
            editor = std::make_unique<funkgui::EditorHost>(p, std::move(c), std::move(owned));
            window = std::make_unique<juce::DocumentWindow>("fg.editorhost.zoom.live", juce::Colours::black, 0);
            window->setUsingNativeTitleBar(true);
            window->setContentNonOwned(editor.get(), true);
            window->setTopLeftPosition(x, y);
            window->setVisible(true);
        }

        ~Win()
        {
            // The editor leaves the window first: parentHierarchyChanged() finds no peer and detaches the surface.
            window->clearContentComponent();
            window.reset();
            editor.reset();
        }

        funkgui::HostServices& host() const { return *panel->host; }
    };

    // The render view against the editor's area of its peer; true when all four edges agree.
    bool viewAgrees(T::Probe& P, const std::string& k, funkgui::EditorHost& editor)
    {
        auto* peer = editor.getPeer();
        NSView* v = renderViewIn(peer);
        if (!P.eq(k + ".view_present", v != nil && peer != nullptr, 1))
            return false;
        const auto area = peer->getAreaCoveredBy(editor);
        const NSRect f = [v frame];
        std::printf("INFO     %s: view %.1f %.1f %.1f %.1f, editor area %d %d %d %d\n", k.c_str(), f.origin.x,
                    f.origin.y, f.size.width, f.size.height, area.getX(), area.getY(), area.getWidth(),
                    area.getHeight());
        bool ok = P.near(k + ".view_x", f.origin.x, area.getX(), 0.0);
        ok = P.near(k + ".view_y", f.origin.y, area.getY(), 0.0) && ok;
        ok = P.near(k + ".view_w", f.size.width, area.getWidth(), 0.0) && ok;
        ok = P.near(k + ".view_h", f.size.height, area.getHeight(), 0.0) && ok;
        return ok;
    }

    void a11yRows(T::Probe& P, const std::string& k, funkgui::EditorHost& editor, float s)
    {
        juce::Component* group = nullptr;
        for (auto* c : editor.getChildren())
            if (c->getTitle() == "Zoom")
                group = c;
        juce::Component* button = group != nullptr && group->getNumChildComponents() > 0 ? group->getChildComponent(0)
                                                                                         : nullptr;
        if (!P.eq(k + ".a11y_children", group != nullptr && button != nullptr, 1))
            return;
        std::printf("INFO     %s: a11y group %s, button %s (in the group)\n", k.c_str(),
                    group->getBounds().toString().toRawUTF8(), button->getBounds().toString().toRawUTF8());
        P.eq(k + ".a11y_group", group->getBounds() == scaled(ProbePanel::kGroup, s), 1);
        P.eq(k + ".a11y_button",
             button->getBounds() == scaled(ProbePanel::kButton, s) - scaled(ProbePanel::kGroup, s).getPosition(), 1);
    }

    // The editor's size, the window's, and the first frame's dpi after a change.
    void sizeRows(T::Probe& P, const std::string& k, Win& w, int percent)
    {
        P.eq(k + ".editor_w", w.editor->getWidth(), zoomed(kW, percent));
        P.eq(k + ".editor_h", w.editor->getHeight(), zoomed(kH, percent));
        P.eq(k + ".diag_zoom", w.editor->diagnostics().zoomPercent, percent);
        const auto content = w.window->getContentComponentBorder();
        P.eq(k + ".window_follows_w", w.window->getWidth(), zoomed(kW, percent) + content.getLeftAndRight());
        P.eq(k + ".window_follows_h", w.window->getHeight(), zoomed(kH, percent) + content.getTopAndBottom());
    }
}

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];   // windows, no Dock icon, no menu bar
    T::Probe P("fg.editorhost.zoom.live", "", argc, argv);

    auto& prefs = funkgui::UiPreferences::get();
    prefs.setInt(kKey, 100);                         // the sandbox may hold the last run's choice
    stamp = juce::Time::getMillisecondCounterHiRes() * 0.001;

    NoOpProcessor processor;
    {
        Win a(processor, 160, 160);
        Win b(processor, 700, 160);
        spin(0.3);

        const bool attached = P.eq("open.surface", a.editor->surfaceAttached() && b.editor->surfaceAttached(), 1);
        P.eq("open.no_fallback", a.editor->showingFallback(), 0);
        P.eq("open.capture_unpinned", a.editor->capture().uiZoom == 0 && a.editor->capture().canvasDump.empty(), 1);
        if (attached)
        {
            pumpFrame();
            const double scale = a.editor->diagnostics().scale;
            std::printf("INFO     backing scale %.3f\n", scale);
            P.eq("open.scale_known", scale > 0.0, 1);
            sizeRows(P, "open", a, 100);
            viewAgrees(P, "open", *a.editor);
            P.near("open.dpi", a.panel->dpis.empty() ? 0.0 : a.panel->dpis.back(), scale, 1e-6);
            a11yRows(P, "open", *a.editor, 1.0f);

            NSView* view = renderViewIn(a.editor->getPeer());
            const uint32_t failures = a.editor->diagnostics().attachFailures;
            for (const int z : { 150, 175, 125 })
            {
                const std::string k = "z" + std::to_string(z);
                const int before = a.editor->getWidth();
                a.host().setZoomPercent(z);          // a ZOOM cell's click
                P.eq(k + ".answered_at_once", a.host().zoomPercent(), z);
                P.eq(k + ".size_waits_for_frame", a.editor->getWidth(), before);
                const size_t first = a.panel->dpis.size();
                pumpFrame();
                spin(0.05);
                sizeRows(P, k, a, z);
                sizeRows(P, k + ".second_window", b, z);
                viewAgrees(P, k, *a.editor);
                P.eq(k + ".same_view", renderViewIn(a.editor->getPeer()) == view, 1);
                P.eq(k + ".no_attach_failure", a.editor->diagnostics().attachFailures, failures);
                P.eq(k + ".surface_attached", a.editor->surfaceAttached(), 1);
                const bool drew = P.eq(k + ".frame_drawn", a.panel->dpis.size() > first, 1);
                const double want = scale * z / 100.0;
                P.near(k + ".first_frame_dpi", drew ? a.panel->dpis[first] : 0.0, want, 1e-6);
                P.eq(k + ".overflows", a.editor->diagnostics().overflows, 0);
                pumpFrame();                         // the a11y values sync at <= 10 Hz; the structure is re-placed
                a11yRows(P, k, *a.editor, static_cast<float>(z) / 100.0f);
                viewAgrees(P, k + ".second_window", *b.editor);
            }

            const size_t first = a.panel->dpis.size();
            a.host().setZoomPercent(100);
            pumpFrame();
            spin(0.05);
            sizeRows(P, "back", a, 100);
            sizeRows(P, "back.second_window", b, 100);
            viewAgrees(P, "back", *a.editor);
            P.near("back.first_frame_dpi", a.panel->dpis.size() > first ? a.panel->dpis[first] : 0.0, scale, 1e-6);
            a11yRows(P, "back", *a.editor, 1.0f);
        }
    }
    prefs.setInt(kKey, 100);
    return P.finish();
}

// FUNKGUI_TEST name=fg.editorhost.follow timeout=300 gpu=1 labels=live
//
// fg.editorhost.follow: EditorHost's placement and theme timing on a real window (G7b, v0.7.1; gpu/EditorHost.h
// "Placement", panel/HostServices.h themeIndex()). Labels fg;gpu;live: it opens a borderless window on the desktop and
// initialises bgfx on Metal, so it needs a window server and is never in `verify`. Run it serialised:
//   lockf -k -t 900 /tmp/fcmp-gui.lock ctest --test-dir <build-agent-gui> -L live
//
// The window is built as JUCE's Standalone builds its own: a top-level component holding an outer component holding a
// parent holding the EditorHost, so the editor's area of the peer can move while the editor never moves in its parent.
// The render view is found as the peer NSView's subview whose class is funkgui::renderViewClassName(), and its frame
// (top-left origin: JUCE's peer view is flipped) must equal ComponentPeer::getAreaCoveredBy(editor):
//   attach.*            the surface attached when the window appeared, where the editor is
//   parent_move.*       the editor's parent moves inside the window: JUCE calls no method of the editor (a counting
//                       subclass sees no moved()), and the view follows (the v0.7.0 bug: it stayed where it was)
//   grandparent_move.*  the same, two levels up
//   own_move.*          the editor itself moves: moved() runs, the view follows (unchanged v0.7.0 behaviour)
//   window_move.*       the whole window moves on the screen: nothing inside the peer changes
//   frame.*             a frame is still drawn and submitted after the moves
//   theme.*             a preference write (a theme cell's click) is read back at once through themeIndex(), and the
//                       first frame after it ticks with that index and draws with that theme
// Frames are pumped by calling FramePump::tick() directly, with the run loop turned between them so Core Animation
// can present. The preference writes go to this test's sandbox (FUNKGUI_PREFS_DIR). Spec rows only.

#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>                             // class_getName

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Theme.h>
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
    // The processor an AudioProcessorEditor needs: no audio, no state.
    class NoOpProcessor final : public juce::AudioProcessor
    {
    public:
        NoOpProcessor()
            : juce::AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
        {
        }

        const juce::String getName() const override { return "fg.editorhost.follow"; }
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

    // One recorded frame: the theme index the Panel read in tick() and in draw(), and the theme draw() was handed.
    struct FrameRec
    {
        int  tickIndex = -1;
        int  drawIndex = -1;
        bool drawnPaper = false;
    };

    class ProbePanel final : public funkgui::Panel
    {
    public:
        funkgui::HostServices* host = nullptr;
        std::vector<FrameRec> frames;                // one per draw(), in order

        void attach(funkgui::HostServices& h) override { host = &h; }
        int  width() const override { return 240; }
        int  height() const override { return 150; }
        void tick(float) override { pendingTick_ = host != nullptr ? host->themeIndex() : -1; }
        void draw(funkgui::Canvas& c, const funkgui::Theme& th) override
        {
            FrameRec r;
            r.tickIndex = pendingTick_;
            r.drawIndex = host != nullptr ? host->themeIndex() : -1;
            const funkgui::Col paper = funkgui::Theme::paper().ground;
            r.drawnPaper = th.ground.r == paper.r && th.ground.g == paper.g && th.ground.b == paper.b;
            frames.push_back(r);
            c.rrect(12.0f, 12.0f, 216.0f, 126.0f, 6.0f, th.ink16, 1.0f, th.ink52);
            c.rrect(24.0f, 24.0f, 60.0f, 24.0f, 4.0f, th.accent);
        }
        bool wantsFullRate() const override { return false; }
        void accessibility(std::vector<funkgui::A11yItem>&) const override {}
        uint32_t a11yRevision() const override { return 0; }
        void a11yAction(uint32_t, funkgui::A11yAction, double) override {}
        void closeGestures() override {}

    private:
        int pendingTick_ = -1;
    };

    // An EditorHost that counts JUCE's moved() calls on it, so a row can tell an ancestor's move (none) from its own.
    class CountingEditor final : public funkgui::EditorHost
    {
    public:
        using funkgui::EditorHost::EditorHost;
        int movedCalls = 0;
        void moved() override
        {
            ++movedCalls;
            funkgui::EditorHost::moved();
        }
    };

    // The render view: the peer NSView's subview of FunkGui's render-view class (this binary's registration).
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

    // Turn the run loop for a moment: window ordering, Core Animation commits and presentation.
    void spin(double seconds)
    {
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
    }

    // The view's frame against the editor's area of its peer, one row per edge; true when all four agree.
    bool framesAgree(T::Probe& P, const std::string& k, CountingEditor& editor)
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

    double stamp = 0.0;

    // One frame from the pump, as its display link would give it, then the run loop for presentation.
    void pumpFrame()
    {
        stamp += 1.0 / 60.0;
        funkgui::FramePump::get().tick(stamp);
        spin(0.02);
    }
}

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;          // NSApplication and the message thread, as in a host
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];   // windows, no Dock icon, no menu bar
    T::Probe P("fg.editorhost.follow", "", argc, argv);

    auto& prefs = funkgui::UiPreferences::get();
    prefs.setTheme(0);                               // the sandbox may hold the last run's choice
    stamp = juce::Time::getMillisecondCounterHiRes() * 0.001;

    NoOpProcessor processor;
    {
        auto owned = std::make_unique<ProbePanel>();
        ProbePanel& panel = *owned;
        CountingEditor editor(processor, funkgui::EditorConfig{}, std::move(owned));

        // top (the window) > outer > parent > editor, as JUCE's Standalone nests its editor.
        juce::Component top, outer, parent;
        top.setOpaque(true);
        top.setBounds(160, 160, 520, 380);           // screen coordinates: a borderless window
        outer.setBounds(0, 0, 500, 360);
        parent.setBounds(10, 10, 400, 300);
        editor.setTopLeftPosition(20, 30);
        parent.addAndMakeVisible(editor);
        outer.addAndMakeVisible(parent);
        top.addAndMakeVisible(outer);
        top.setVisible(true);
        top.addToDesktop(0);                         // the peer: parentHierarchyChanged() attaches the surface
        spin(0.2);

        const bool attached = P.eq("attach.surface", editor.surfaceAttached(), 1);
        P.eq("attach.no_fallback", editor.showingFallback(), 0);
        if (attached)
        {
            framesAgree(P, "attach", editor);
            pumpFrame();

            // ---- an ancestor moves: the view must follow without the editor being told ---------------------------
            const int movedBefore = editor.movedCalls;
            const auto areaBefore = editor.getPeer()->getAreaCoveredBy(editor);
            parent.setTopLeftPosition(40, 57);       // +30, +47 inside outer
            const auto areaParent = editor.getPeer()->getAreaCoveredBy(editor);
            P.eq("parent_move.area_moved_x", areaParent.getX() - areaBefore.getX(), 30);
            P.eq("parent_move.area_moved_y", areaParent.getY() - areaBefore.getY(), 47);
            P.eq("parent_move.editor_not_told", editor.movedCalls - movedBefore, 0);
            framesAgree(P, "parent_move", editor);

            outer.setTopLeftPosition(9, 13);         // two levels up
            P.eq("grandparent_move.editor_not_told", editor.movedCalls - movedBefore, 0);
            framesAgree(P, "grandparent_move", editor);

            // ---- the editor itself moves: moved(), as before -----------------------------------------------------
            editor.setTopLeftPosition(3, 5);
            P.eq("own_move.moved_called", editor.movedCalls - movedBefore >= 1, 1);
            framesAgree(P, "own_move", editor);

            // ---- the whole window moves: the view's frame is relative to the peer and must not change -------------
            const NSRect beforeWindow = [renderViewIn(editor.getPeer()) frame];
            top.setTopLeftPosition(200, 190);
            spin(0.05);
            const NSRect afterWindow = [renderViewIn(editor.getPeer()) frame];
            P.eq("window_move.view_unchanged", NSEqualRects(beforeWindow, afterWindow) ? 1 : 0, 1);
            framesAgree(P, "window_move", editor);

            // ---- frames still draw ---------------------------------------------------------------------------------
            const uint32_t framesBefore = editor.diagnostics().frames;
            pumpFrame();
            pumpFrame();
            P.ge("frame.submitted_after_moves", editor.diagnostics().frames - framesBefore, 2);
            P.eq("frame.surface_still_attached", editor.surfaceAttached(), 1);
            P.eq("frame.overflows", editor.diagnostics().overflows, 0);

            // ---- theme: a write is read at once and lands in the next frame ---------------------------------------
            const bool noOverride = P.eq("theme.no_override", editor.capture().uiTheme < 0, 1);
            if (noOverride && panel.host != nullptr)
            {
                P.eq("theme.before_write", panel.host->themeIndex(), 0);
                P.eq("theme.last_frame_graphite", !panel.frames.empty() && !panel.frames.back().drawnPaper, 1);
                const size_t firstAfter = panel.frames.size();
                prefs.setTheme(1);                   // what a theme cell's click does
                P.eq("theme.read_at_write", panel.host->themeIndex(), 1);
                pumpFrame();
                const bool drew = P.eq("theme.frame_after_write", panel.frames.size() > firstAfter, 1);
                const FrameRec r = drew ? panel.frames[firstAfter] : FrameRec{};
                P.eq("theme.first_frame_tick_index", r.tickIndex, 1);
                P.eq("theme.first_frame_draw_index", r.drawIndex, 1);
                P.eq("theme.first_frame_drawn_paper", r.drawnPaper, 1);

                const size_t firstBack = panel.frames.size();
                prefs.setTheme(0);
                pumpFrame();
                const bool drewBack = P.eq("theme.frame_after_restore", panel.frames.size() > firstBack, 1);
                const FrameRec b = drewBack ? panel.frames[firstBack] : FrameRec{ 1, 1, true };
                P.eq("theme.restore_tick_index", b.tickIndex, 0);
                P.eq("theme.restore_draw_index", b.drawIndex, 0);
                P.eq("theme.restore_drawn_graphite", !b.drawnPaper, 1);
            }
        }

        // The editor leaves the window first: parentHierarchyChanged() finds no peer and detaches the surface. (JUCE's
        // removeFromDesktop() tells no child, so the window goes only once nothing in it holds a surface.)
        parent.removeChildComponent(&editor);
        P.eq("teardown.detached", editor.surfaceAttached(), 0);
        top.removeFromDesktop();
        top.removeChildComponent(&outer);
        outer.removeChildComponent(&parent);
    }
    prefs.setTheme(0);
    return P.finish();
}

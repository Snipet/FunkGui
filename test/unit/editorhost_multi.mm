// FUNKGUI_TEST name=fg.editorhost.multi timeout=300 gpu=1 labels=live
//
// fg.editorhost.multi: several editors in one process, as a host opens several instances of a plug-in (v0.21.0;
// gpu/FramePump.h, gpu/BgfxContext.h). Labels fg;gpu;live: it opens borderless windows on the desktop and initialises
// bgfx on Metal, so it needs a window server and is never in `verify`. Run it serialised:
//   lockf -k -t 900 /tmp/fcmp-gui.lock ctest --test-dir <build-agent-gui> -L live
//
// Frames come from the pump's own clock here (its CADisplayLink, its watchdog), not from tick() called by hand: the
// bug was the clock itself. The run loop turns for real time between rows.
//   both.*      two windows, an editor each: both attached, both drawing
//   hidden.*    the first window hidden (a host that shows one plug-in window at a time does it to show the next): the
//               second editor goes on drawing, the link moved to its view (the v0.20.0 bug: the link stayed on the
//               hidden view, stopped, and every editor froze; a host's second instance showed only its background; at
//               v0.20.0 this row draws 0 frames). A view counts as on screen when its window is shown: occlusion is
//               no guide (a covered window's link goes on firing, and the state reads "not visible" for shown ones)
//   shown.*     the first window shown again: it draws again
//   unframed.*  an editor opened and closed with no frame between (its framebuffer's creation still in bgfx's queue):
//               its view held until bgfx has run that command, then let go, and the process lives (the v0.20.0 bug:
//               bgfx made the swapchain on the freed NSView at the next frame, a crash in SwapChainMtl::init)
//   teardown.*  every editor closed: nothing held
// Spec rows only.

#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>                             // class_getName

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Theme.h>
#include <funkgui/gpu/BgfxContext.h>
#include <funkgui/gpu/DisplayLink.h>
#include <funkgui/gpu/NativeSurface.h>
#include <funkgui/gpu/EditorHost.h>
#include <funkgui/gpu/FramePump.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Panel.h>
#include <funkgui/test/Harness.h>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

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

        const juce::String getName() const override { return "fg.editorhost.multi"; }
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

    // A panel that counts its draws.
    class CountingPanel final : public funkgui::Panel
    {
    public:
        int draws = 0;

        void attach(funkgui::HostServices&) override {}
        int  width() const override { return 260; }
        int  height() const override { return 170; }
        void tick(float) override {}
        void draw(funkgui::Canvas& c, const funkgui::Theme& th) override
        {
            ++draws;
            c.rrect(12.0f, 12.0f, 236.0f, 146.0f, 6.0f, th.ink16, 1.0f, th.ink52);
        }
        bool wantsFullRate() const override { return false; }
        void accessibility(std::vector<funkgui::A11yItem>&) const override {}
        uint32_t a11yRevision() const override { return 0; }
        void a11yAction(uint32_t, funkgui::A11yAction, double) override {}
        void closeGestures() override {}
    };

    // A borderless window holding one editor.
    struct Window
    {
        juce::Component top;
        std::unique_ptr<funkgui::EditorHost> editor;
        CountingPanel* panel = nullptr;

        Window(juce::AudioProcessor& p, int x, int y)
        {
            auto owned = std::make_unique<CountingPanel>();
            panel = owned.get();
            editor = std::make_unique<funkgui::EditorHost>(p, funkgui::EditorConfig{}, std::move(owned));
            top.setOpaque(true);
            top.setBounds(x, y, 280, 190);
            editor->setTopLeftPosition(10, 10);
            top.addAndMakeVisible(*editor);
            top.setVisible(true);
            top.addToDesktop(0);                     // the peer: parentHierarchyChanged() attaches the surface
        }

        // The editor leaves the window first (it detaches), then the window goes.
        void close()
        {
            if (editor != nullptr)
            {
                top.removeChildComponent(editor.get());
                editor.reset();
            }
            top.removeFromDesktop();
        }

        ~Window() { close(); }
    };

    // The render view: the peer NSView's subview of FunkGui's render-view class.
    void* renderViewOf(funkgui::EditorHost& e)
    {
        auto* peer = e.getPeer();
        if (peer == nullptr)
            return nullptr;
        NSView* parent = (NSView*) peer->getNativeHandle();
        const char* cls = funkgui::renderViewClassName();
        for (NSView* v in [parent subviews])
            if (std::strcmp(class_getName([v class]), cls) == 0)
                return (void*) v;
        return nullptr;
    }

    void spin(double seconds)
    {
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
    }

    // The draws a window's editor makes over `seconds` of the run loop.
    int drawsOver(Window& w, double seconds)
    {
        const int before = w.panel->draws;
        spin(seconds);
        return w.panel->draws - before;
    }
}

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;          // NSApplication and the message thread, as in a host
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];   // windows, no Dock icon, no menu bar
    T::Probe P("fg.editorhost.multi", "", argc, argv);
    auto& pump = funkgui::FramePump::get();
    auto& ctx = funkgui::BgfxContext::get();

    NoOpProcessor processor;
    {
        // ---- both: two editors, both drawing ---------------------------------------------------------------------------
        Window a(processor, 140, 140);
        spin(0.3);
        Window b(processor, 470, 140);
        spin(0.5);
        P.eq("both.attached", a.editor->surfaceAttached() && b.editor->surfaceAttached() ? 1 : 0, 1);
        P.eq("both.display_linked", pump.isDisplayLinked() ? 1 : 0, 1);
        const int da = a.panel->draws, db = b.panel->draws;
        spin(1.0);
        std::printf("INFO     both: %d and %d draws in a second (the idle cadence is 12 Hz)\n", a.panel->draws - da,
                    b.panel->draws - db);
        P.ge("both.first_draws", a.panel->draws - da, 5);
        P.ge("both.second_draws", b.panel->draws - db, 5);

        // ---- hidden: the first window hidden, the second goes on -------------------------------------------------------
        const int rebinds = pump.linkRebinds();
        a.top.setVisible(false);                     // orders the window out; its editor stays
        spin(0.4);                                   // the watchdog's quarter second, and a turn
        std::printf("INFO     hidden: first on screen %d, second on screen %d, linked %d\n",
                    funkgui::viewOnScreen(renderViewOf(*a.editor)) ? 1 : 0, funkgui::viewOnScreen(renderViewOf(*b.editor)) ? 1 : 0,
                    pump.isDisplayLinked() ? 1 : 0);
        const int hiddenDraws = drawsOver(b, 1.0);
        std::printf("INFO     hidden: the second drew %d times in a second; the link moved %d time(s)\n", hiddenDraws,
                    pump.linkRebinds() - rebinds);
        P.ge("hidden.second_draws", hiddenDraws, 5);
        P.ge("hidden.link_moved", pump.linkRebinds() - rebinds, 1);
        P.eq("hidden.still_linked", pump.isDisplayLinked() ? 1 : 0, 1);

        // ---- shown: the first window again ---------------------------------------------------------------------------
        a.top.setVisible(true);
        spin(0.3);
        P.ge("shown.first_draws", drawsOver(a, 1.0), 5);
        P.ge("shown.second_draws", drawsOver(b, 0.5), 2);

        // ---- unframed: opened and closed with no frame between -----------------------------------------------------
        {
            Window c(processor, 800, 140);           // attaches at once: its framebuffer is made at the next frame
            P.eq("unframed.attached", c.editor->surfaceAttached() ? 1 : 0, 1);
            c.close();                               // no frame has run: bgfx's queue still names the view
        }
        P.ge("unframed.view_held", ctx.retiringViews(), 1);
        spin(0.5);                                   // frames run: the swapchain is made on the held view, then dropped
        P.eq("unframed.let_go", ctx.retiringViews(), 0);
        P.eq("unframed.survived", 1, 1);             // (a crash ends the test before this row)
        P.ge("unframed.others_draw", drawsOver(b, 0.5), 2);

        // ---- the same while every frame waits: the first window hidden, the second closed unframed -----------------
        {
            Window d(processor, 800, 380);
            d.close();
        }
        a.top.setVisible(false);
        spin(0.3);
        b.top.setVisible(false);                     // nothing on screen: no frames at all
        spin(0.3);
        b.top.setVisible(true);                      // the host shows the other one again, and frames run
        spin(0.6);
        P.eq("waiting.survived", 1, 1);
        P.eq("waiting.let_go", ctx.retiringViews(), 0);

        a.close();
        b.close();
        spin(0.1);
    }
    P.eq("teardown.nothing_held", ctx.retiringViews(), 0);
    P.eq("teardown.bgfx_down", ctx.valid() ? 0 : 1, 1);
    return P.finish();
}

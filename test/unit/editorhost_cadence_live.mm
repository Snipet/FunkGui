// FUNKGUI_TEST name=fg.editorhost.cadence.live timeout=300 gpu=1 labels=live
//
// fg.editorhost.cadence.live: two editors on real windows, each at its own cadence (0.25.0; gpu/FramePump.h "Each
// editor its own cadence", gpu/EditorHost.h "Cadence"). Labels fg;gpu;live: it opens borderless windows on the desktop
// and initialises bgfx on Metal, so it needs a window server and is never in `verify`. Run it serialised:
//   lockf -k -t 900 /tmp/fcmp-gui.lock ctest --test-dir <build-agent-gui> -R '^fg\.editorhost\.cadence\.live$'
//
// Frames come from the pump's own clock (its CADisplayLink) while the run loop turns. Each Panel counts its ticks and
// sums their dts; one is busy (wantsFullRate() true) where a row says so, the other calm. Rates are over a second, with
// margins for a display link's own pace.
//   open.*      both editors attached, the pump on its display link
//   busy.*      the first busy, the second calm: the first ticks at full rate, the second at about 12 Hz with dts of
//               about a twelfth of a second (before 0.25.0 it ticked at the busy one's rate); each editor's
//               Diagnostics::fps reports its own rate
//   nudge.*     the calm editor's Panel nudged a hundred times a second (HostServices::nudgeFullRate, as pointer moves
//               do): it ticks at full rate too
//   idle.*      both calm: both at about 12 Hz, the pump idle
// Spec rows only.

#import <Cocoa/Cocoa.h>

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Col.h>
#include <funkgui/core/Theme.h>
#include <funkgui/gpu/BgfxContext.h>
#include <funkgui/gpu/EditorHost.h>
#include <funkgui/gpu/FramePump.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Panel.h>
#include <funkgui/test/Harness.h>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <cstdio>
#include <memory>

namespace T = funkgui::test;

namespace
{
    class NoOpProcessor final : public juce::AudioProcessor
    {
    public:
        NoOpProcessor()
            : juce::AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
        {
        }

        const juce::String getName() const override { return "fg.editorhost.cadence.live"; }
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

    // A Panel that counts its ticks and their dts; busy (a new shade every frame, full rate) when told.
    class CountingPanel final : public funkgui::Panel
    {
    public:
        funkgui::HostServices* host = nullptr;
        bool   busy = false;
        int    ticks = 0;
        double dtSum = 0.0;

        void attach(funkgui::HostServices& h) override { host = &h; }
        int  width() const override { return 260; }
        int  height() const override { return 170; }
        void tick(float dt) override
        {
            ++ticks;
            dtSum += static_cast<double>(dt);
            if (busy)
                ++shade_;
        }
        void draw(funkgui::Canvas& c, const funkgui::Theme& th) override
        {
            c.rrect(12.0f, 12.0f, 236.0f, 146.0f, 6.0f, funkgui::Col{ shade_, 60, 90, 255 }, 1.0f, th.ink52);
        }
        bool wantsFullRate() const override { return busy; }
        void accessibility(std::vector<funkgui::A11yItem>&) const override {}
        uint32_t a11yRevision() const override { return 0; }
        void a11yAction(uint32_t, funkgui::A11yAction, double) override {}
        void closeGestures() override {}

    private:
        uint8_t shade_ = 40;
    };

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
            top.addToDesktop(0);
        }

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

    // Pointer moves over a Panel, as far as the pump is concerned: its host nudged a hundred times a second.
    struct Mover final : juce::Timer
    {
        funkgui::HostServices* host = nullptr;
        void timerCallback() override
        {
            if (host != nullptr)
                host->nudgeFullRate();
        }
    };

    void spin(double seconds)
    {
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
    }

    struct Over
    {
        int    ticks = 0;
        double meanDt = 0.0;
    };

    // Two panels' ticks over `seconds` of the run loop.
    void measure(Window& a, Window& b, double seconds, Over& oa, Over& ob)
    {
        const int ta = a.panel->ticks, tb = b.panel->ticks;
        const double da = a.panel->dtSum, db = b.panel->dtSum;
        spin(seconds);
        oa.ticks = a.panel->ticks - ta;
        ob.ticks = b.panel->ticks - tb;
        oa.meanDt = oa.ticks > 0 ? (a.panel->dtSum - da) / oa.ticks : 0.0;
        ob.meanDt = ob.ticks > 0 ? (b.panel->dtSum - db) / ob.ticks : 0.0;
    }
}

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    T::Probe P("fg.editorhost.cadence.live", "", argc, argv);
    auto& pump = funkgui::FramePump::get();
    auto& ctx = funkgui::BgfxContext::get();

    NoOpProcessor processor;
    {
        Window a(processor, 140, 140);
        Window b(processor, 470, 140);
        spin(0.5);
        P.eq("open.attached", a.editor->surfaceAttached() && b.editor->surfaceAttached() ? 1 : 0, 1);
        P.eq("open.display_linked", pump.isDisplayLinked() ? 1 : 0, 1);

        // ---- busy: the first busy, the second calm ------------------------------------------------------------------
        a.panel->busy = true;
        a.panel->host->nudgeFullRate();
        spin(1.2);                                   // Diagnostics::fps is over windows of a second: one wholly busy
        Over oa, ob;
        measure(a, b, 1.0, oa, ob);
        const float fa = a.editor->diagnostics().fps, fb = b.editor->diagnostics().fps;
        std::printf("INFO     busy: %d ticks (fps %.1f) beside %d calm ones (fps %.1f, mean dt %.4f s) in a second\n",
                    oa.ticks, static_cast<double>(fa), ob.ticks, static_cast<double>(fb), ob.meanDt);
        P.ge("busy.busy_ticks", oa.ticks, 40);
        P.in("busy.calm_ticks", ob.ticks, 8, 16);
        P.in("busy.calm_dt", ob.meanDt, 0.06, 0.12);
        P.le("busy.busy_dt", oa.meanDt, 0.03);
        P.ge("busy.busy_fps", fa, 40.0);
        P.in("busy.calm_fps", fb, 8.0, 16.0);
        P.eq("busy.pump_full_rate", pump.wantedFullRate() ? 1 : 0, 1);

        // ---- nudge: the calm editor's Panel nudged as pointer moves would -------------------------------------------
        {
            Mover mover;
            mover.host = b.panel->host;
            mover.startTimerHz(100);
            spin(0.2);
            measure(a, b, 1.0, oa, ob);
            mover.stopTimer();
            std::printf("INFO     nudge: %d and %d ticks in a second\n", oa.ticks, ob.ticks);
            P.ge("nudge.nudged_ticks", ob.ticks, 40);
            P.ge("nudge.busy_ticks", oa.ticks, 40);
        }

        // ---- idle: both calm ----------------------------------------------------------------------------------------
        a.panel->busy = false;
        spin(0.3);
        measure(a, b, 1.0, oa, ob);
        std::printf("INFO     idle: %d and %d ticks in a second\n", oa.ticks, ob.ticks);
        P.in("idle.first_ticks", oa.ticks, 8, 16);
        P.in("idle.second_ticks", ob.ticks, 8, 16);
        P.eq("idle.pump_idle", pump.wantedFullRate() ? 1 : 0, 0);

        a.close();
        b.close();
        spin(0.1);
    }
    P.eq("teardown.bgfx_down", ctx.valid() ? 0 : 1, 1);
    return P.finish();
}

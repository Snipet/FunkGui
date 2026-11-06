// FUNKGUI_TEST name=fg.editorhost.cadence timeout=120 gpu=1
//
// fg.editorhost.cadence (0.25.0; gpu/FramePump.h "Each editor its own cadence", src/gpu/ClientCadence.h,
// gpu/EditorHost.h "Cadence"): several editors in one FramePump, each at its own cadence. Frames are ticks of
// FramePump::tick() by hand at 60 Hz (a twelfth of a second apart where a row says idle); no window, no bgfx frame.
// fg.framepump.cadence holds the rules; fg.editorhost.cadence.live runs two editors on real windows.
//   two.*      a busy client (it asks for full rate) and a calm one: the busy one runs at every tick, the calm one at
//              every fifth (its dt the five ticks), each with its own rate request and measured rate; the pump asks
//              for full rate
//   idle.*     the busy one calm too: the pump asks for the idle rate and every client runs at every tick, as before
//              0.25.0; busy again, the calm one is back to every fifth tick
//   nudge.*    nudgeFullRate(c) runs that client at the next tick and no other; nudgeFullRate() runs every client at
//              the next tick; a client that nudges itself during its own frame runs at the next tick too
//   change.*   a client removed during another's frame does not run and is forgotten; one removed by its own frame
//              is forgotten; one added during a frame runs at the next tick
//   unknown.*  a client the pump does not hold: no rate, no request; nudging it nudges only the pump's rate
//   editors.*  two EditorHosts (never on screen) beside a busy client: both calm at 12 fps (Diagnostics::fps); the
//              pointer over one (EditorHost::mouseMove) or its Panel's HostServices::nudgeFullRate() runs that one at
//              the next tick, the other keeps 12 fps; a zoom chosen under a preference key nudges both, and the other
//              follows it at the next tick
// Spec rows only.

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Env.h>
#include <funkgui/core/Theme.h>
#include <funkgui/gpu/EditorHost.h>
#include <funkgui/gpu/FramePump.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Panel.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/test/Harness.h>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace T = funkgui::test;

namespace
{
    constexpr const char* kKey = "fgCadenceZoom";
    const double kTick = static_cast<double>(static_cast<float>(1.0 / 60.0));   // the pump's dt, a float

    double stamp = 1000.0;
    int    tickIndex = 0;

    // One tick of the pump, `seconds` after the last.
    void pumpTick(double seconds = 1.0 / 60.0)
    {
        stamp += seconds;
        ++tickIndex;
        funkgui::FramePump::get().tick(stamp);
    }

    // A FrameClient with no view: it records the ticks it ran at and the dt of each.
    class FakeClient final : public funkgui::FrameClient
    {
    public:
        explicit FakeClient(bool wantsFullRate) : wants(wantsFullRate) {}

        bool wants;
        std::vector<int>   ranAt;
        std::vector<float> dts;
        std::function<void()> during;                // runs inside its frame, once

        Result submitFrame(float dt) override
        {
            ranAt.push_back(tickIndex);
            dts.push_back(dt);
            if (during)
            {
                auto f = std::move(during);
                during = nullptr;
                f();
            }
            Result r;
            r.wantsFullRate = wants;
            return r;
        }
        void* frameClockView() const override { return nullptr; }

        int  frames() const { return static_cast<int>(ranAt.size()); }
        bool ranLast() const { return !ranAt.empty() && ranAt.back() == tickIndex; }
    };

    // Gaps between the client's frames from its `from`th frame on that are not `gap`.
    int64_t otherGaps(const FakeClient& c, std::size_t from, int gap)
    {
        int64_t n = 0;
        for (std::size_t i = std::max<std::size_t>(from, 1); i < c.ranAt.size(); ++i)
            n += (c.ranAt[i] - c.ranAt[i - 1]) != gap ? 1 : 0;
        return n;
    }

    double maxDtError(const FakeClient& c, std::size_t from, double want)
    {
        double e = 0.0;
        for (std::size_t i = from; i < c.dts.size(); ++i)
            e = std::max(e, std::fabs(static_cast<double>(c.dts[i]) - want));
        return e;
    }

    // The processor an AudioProcessorEditor needs: no audio, no state.
    class NoOpProcessor final : public juce::AudioProcessor
    {
    public:
        NoOpProcessor()
            : juce::AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
        {
        }

        const juce::String getName() const override { return "fg.editorhost.cadence"; }
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

    // A calm Panel that keeps its HostServices.
    class CalmPanel final : public funkgui::Panel
    {
    public:
        funkgui::HostServices* host = nullptr;
        int moves = 0;

        void attach(funkgui::HostServices& h) override { host = &h; }
        int  width() const override { return 240; }
        int  height() const override { return 160; }
        void tick(float) override {}
        void draw(funkgui::Canvas&, const funkgui::Theme&) override {}
        bool wantsFullRate() const override { return false; }
        void pointerMove(const funkgui::PointerEvent&) override { ++moves; }
        void accessibility(std::vector<funkgui::A11yItem>&) const override {}
        uint32_t a11yRevision() const override { return 0; }
        void a11yAction(uint32_t, funkgui::A11yAction, double) override {}
        void closeGestures() override {}
    };

    struct Rig
    {
        CalmPanel* panel = nullptr;
        std::unique_ptr<funkgui::EditorHost> editor;

        explicit Rig(juce::AudioProcessor& p)
        {
            funkgui::EditorConfig config;
            config.zoomSteps = { 100, 125, 150 };
            config.defaultZoomPercent = 100;
            config.zoomPrefKey = kKey;
            auto owned = std::make_unique<CalmPanel>();
            panel = owned.get();
            editor = std::make_unique<funkgui::EditorHost>(p, std::move(config), std::move(owned));
        }
        funkgui::HostServices& host() const { return *panel->host; }
        // The editor as the pump holds it. FrameClient is a private base of EditorHost: a C-style cast is the one
        // conversion the language allows to it from outside.
        const funkgui::FrameClient* client() const { return (const funkgui::FrameClient*) editor.get(); }
    };

    juce::MouseEvent mouseAt(juce::Component& c, float x, float y)
    {
        const auto now = juce::Time::getCurrentTime();
        return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), { x, y }, juce::ModifierKeys(),
                                0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, { x, y }, now, 1, false);
    }
}

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    T::Probe P("fg.editorhost.cadence", "", argc, argv);
    {
        const std::string dump = std::string(FUNKGUI_ENV_PREFIX) + "CANVAS_DUMP";   // input is taken, the clock free
        T::unsetEnv(dump.c_str());
        funkgui::envReload();
    }
    auto& pump = funkgui::FramePump::get();

    // ---- two: a busy client and a calm one --------------------------------------------------------------------------
    FakeClient busy(true), calm(false);
    pump.add(&busy);
    pump.add(&calm);
    pumpTick();                                      // both new: both run
    P.eq("two.first_tick_both", busy.frames() == 1 && calm.frames() == 1 ? 1 : 0, 1);
    for (int i = 0; i < 600; ++i)
        pumpTick();
    std::printf("INFO     two: %d busy frames and %d calm ones in 600 ticks\n", busy.frames() - 1, calm.frames() - 1);
    P.eq("two.busy_every_tick", busy.frames() - 1, 600);
    P.eq("two.calm_fifth_tick", calm.frames() - 1, 120);
    P.eq("two.calm_other_gaps", otherGaps(calm, 1, 5), 0);
    P.le("two.calm_dt", maxDtError(calm, 1, 5.0 * kTick), 1e-6);
    P.le("two.busy_dt", maxDtError(busy, 1, kTick), 1e-6);
    P.eq("two.pump_full_rate", pump.wantedFullRate(), 1);
    P.eq("two.busy_asks", pump.wantedFullRate(&busy), 1);
    P.eq("two.calm_does_not", pump.wantedFullRate(&calm), 0);
    P.near("two.busy_fps", pump.measuredFps(&busy), 60.0, 0.5);
    P.near("two.calm_fps", pump.measuredFps(&calm), 12.0, 0.5);
    P.near("two.pump_fps", pump.measuredFps(), 60.0, 0.5);

    // ---- idle: nobody asks for full rate ----------------------------------------------------------------------------
    {
        busy.wants = false;
        pumpTick();                                  // busy's last full-rate frame asks for idle
        P.eq("idle.pump_idle", pump.wantedFullRate(), 0);
        const int b0 = busy.frames(), c0 = calm.frames();
        const std::size_t from = calm.dts.size();
        for (int i = 0; i < 24; ++i)
            pumpTick(1.0 / 12.0);
        P.eq("idle.busy_every_tick", busy.frames() - b0, 24);
        P.eq("idle.calm_every_tick", calm.frames() - c0, 24);
        P.le("idle.calm_dt", maxDtError(calm, from + 1, static_cast<double>(static_cast<float>(1.0 / 12.0))), 1e-6);

        busy.wants = true;
        pumpTick();                                  // every client runs; busy asks for full rate
        P.eq("idle.back_full", pump.wantedFullRate(), 1);
        const int c1 = calm.frames();
        for (int i = 0; i < 60; ++i)
            pumpTick();
        P.eq("idle.back_calm_fifth", calm.frames() - c1, 12);
    }
    pump.remove(&calm);

    // ---- nudge: one client, every client, during its own frame ------------------------------------------------------
    {
        FakeClient a(false), b(false);
        pump.add(&a);
        pumpTick();                                  // s0: a new, runs
        pumpTick();                                  // s1
        pump.add(&b);
        pumpTick();                                  // s2: b new, runs
        for (int i = 0; i < 5; ++i)
            pumpTick();                              // s3 .. s7: a at s5, b at s7
        P.eq("nudge.setup_b_ran", b.ranLast(), 1);
        P.eq("nudge.setup_a_ran_two_ago", a.ranAt.back(), tickIndex - 2);
        pump.nudgeFullRate(&a);
        P.eq("nudge.asks", pump.wantedFullRate(&a), 1);
        P.eq("nudge.other_does_not_ask", pump.wantedFullRate(&b), 0);
        pumpTick();                                  // s8
        P.eq("nudge.runs_next_tick", a.ranLast(), 1);
        P.eq("nudge.other_waits", b.ranLast(), 0);
        P.near("nudge.dt_since_own_frame", a.dts.back(), 3.0 * kTick, 1e-6);
        P.eq("nudge.consumed", pump.wantedFullRate(&a), 0);
        const int at = tickIndex;
        for (int i = 0; i < 5; ++i)
            pumpTick();
        P.eq("nudge.then_own_cadence", a.ranAt.back(), at + 5);

        pumpTick();                                  // neither is due at the next tick unless nudged
        pump.nudgeFullRate();
        P.eq("nudge.all_ask", pump.wantedFullRate(&a) && pump.wantedFullRate(&b) && pump.wantedFullRate(&busy), 1);
        pumpTick();
        P.eq("nudge.all_a_runs", a.ranLast(), 1);
        P.eq("nudge.all_b_runs", b.ranLast(), 1);

        pumpTick();
        a.during = [&] { pump.nudgeFullRate(&a); };  // a change its own frame wants shown soon
        pump.nudgeFullRate(&a);
        pumpTick();
        P.eq("nudge.in_frame_ran", a.ranLast(), 1);
        P.eq("nudge.in_frame_holds", pump.wantedFullRate(&a), 1);
        pumpTick();
        P.eq("nudge.in_frame_next_tick", a.ranLast(), 1);
        P.eq("nudge.in_frame_then_calm", pump.wantedFullRate(&a), 0);
        pumpTick();
        P.eq("nudge.in_frame_then_waits", a.ranLast(), 0);

        // ---- change: removed and added during a tick ---------------------------------------------------------------
        FakeClient c(false);
        pump.nudgeFullRate();                        // a and b both run at the next tick: a first (added first)
        a.during = [&]
        {
            pump.remove(&b);
            pump.add(&c);
        };
        const int bFrames = b.frames();
        pumpTick();
        P.eq("change.removed_not_run", b.frames(), bFrames);
        P.eq("change.removed_forgotten", pump.measuredFps(&b) == 0.0f && !pump.wantedFullRate(&b) ? 1 : 0, 1);
        P.eq("change.added_not_this_tick", c.frames(), 0);
        pumpTick();
        P.eq("change.added_next_tick", c.ranLast(), 1);
        pump.nudgeFullRate(&a);
        a.during = [&] { pump.remove(&a); };
        pumpTick();
        P.eq("change.self_removed_ran", a.ranLast(), 1);
        P.eq("change.self_removed_forgotten", pump.measuredFps(&a) == 0.0f && !pump.wantedFullRate(&a) ? 1 : 0, 1);
        pump.remove(&c);

        // ---- unknown ------------------------------------------------------------------------------------------------
        FakeClient stranger(true);
        P.eq("unknown.no_fps", pump.measuredFps(&stranger) == 0.0f ? 1 : 0, 1);
        P.eq("unknown.no_request", pump.wantedFullRate(&stranger), 0);
        busy.wants = false;
        pumpTick();
        pumpTick();
        P.eq("unknown.pump_idle", pump.wantedFullRate(), 0);
        pump.nudgeFullRate(&stranger);
        P.eq("unknown.nudges_pump", pump.wantedFullRate(), 1);
        P.eq("unknown.still_no_request", pump.wantedFullRate(&stranger), 0);
        P.eq("unknown.not_run", stranger.frames(), 0);
        busy.wants = true;
        pumpTick();
    }

    // ---- editors: two EditorHosts beside the busy client ------------------------------------------------------------
    {
        auto& prefs = funkgui::UiPreferences::get();
        prefs.setInt(kKey, 100);                     // the sandbox may hold the last run's choice
        NoOpProcessor processor;
        Rig a(processor), b(processor);
        pumpTick();                                  // both new: both run
        for (int i = 0; i < 180; ++i)
            pumpTick();
        const float fa = a.editor->diagnostics().fps, fb = b.editor->diagnostics().fps;
        std::printf("INFO     editors: %.2f and %.2f fps beside a busy client at %.2f\n", static_cast<double>(fa),
                    static_cast<double>(fb), static_cast<double>(pump.measuredFps(&busy)));
        P.near("editors.calm_a_fps", fa, 12.0, 0.5);
        P.near("editors.calm_b_fps", fb, 12.0, 0.5);
        P.eq("editors.calm_a_asks_nothing", pump.wantedFullRate(a.client()), 0);

        // The pointer over A, before every tick: A at every tick, B at its own.
        a.editor->mouseMove(mouseAt(*a.editor, 20.0f, 20.0f));
        P.eq("editors.input_nudges_own", pump.wantedFullRate(a.client()), 1);
        P.eq("editors.input_not_other", pump.wantedFullRate(b.client()), 0);
        for (int i = 0; i < 180; ++i)
        {
            a.editor->mouseMove(mouseAt(*a.editor, 20.0f + static_cast<float>(i % 10), 20.0f));
            pumpTick();
        }
        P.ge("editors.input_reached_panel", a.panel->moves, 181);
        const float ia = a.editor->diagnostics().fps, ib = b.editor->diagnostics().fps;
        std::printf("INFO     editors: the pointer over A: %.2f and %.2f fps\n", static_cast<double>(ia),
                    static_cast<double>(ib));
        P.near("editors.input_a_fps", ia, 60.0, 0.5);
        P.near("editors.input_b_fps", ib, 12.0, 0.5);

        // A Panel's own nudge (HostServices): that editor at the next tick (its request consumed), the other not.
        pumpTick();
        a.host().nudgeFullRate();
        P.eq("editors.host_nudge_own", pump.wantedFullRate(a.client()), 1);
        P.eq("editors.host_nudge_not_other", pump.wantedFullRate(b.client()), 0);
        pumpTick();
        P.eq("editors.host_nudge_ran", pump.wantedFullRate(a.client()), 0);

        // A zoom chosen under a preference key: every editor follows it, so both run at the next tick.
        P.eq("editors.zoom_b_before", b.editor->getWidth(), 240);
        a.host().setZoomPercent(150);
        P.eq("editors.zoom_nudges_own", pump.wantedFullRate(a.client()), 1);
        P.eq("editors.zoom_nudges_other", pump.wantedFullRate(b.client()), 1);
        pumpTick();
        P.eq("editors.zoom_a_follows", a.editor->getWidth(), 360);
        P.eq("editors.zoom_b_follows", b.editor->getWidth(), 360);
        P.eq("editors.zoom_both_ran", pump.wantedFullRate(a.client()) || pump.wantedFullRate(b.client()), 0);
        prefs.setInt(kKey, 100);
    }
    pump.remove(&busy);
    return P.finish();
}

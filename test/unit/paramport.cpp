// FUNKGUI_TEST name=fg.paramport timeout=300 gpu=0
//
// fg.paramport: JuceParamPort (params/JuceParamPort.h; 02 §5.2, K1 #33, K2 #27) over real JUCE 8.0.4 parameters owned
// by a minimal juce::AudioProcessor: every ParamPort call maps to its RangedAudioParameter call, and a
// GestureController driving the ports produces exactly the host-side gesture and value notifications JUCE sends
// (recorded by an AudioProcessorParameter::Listener). Spec rows only.

#include <funkgui/panel/HostServices.h>
#include <funkgui/params/GestureController.h>
#include <funkgui/params/JuceParamPort.h>
#include <funkgui/test/Harness.h>

#include <juce_audio_processors/juce_audio_processors.h>

#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace T = funkgui::test;

namespace
{
    // The smallest processor that owns parameters (they need one for gestures: JUCE asserts processor != nullptr).
    class Processor final : public juce::AudioProcessor
    {
    public:
        Processor()
        {
            auto g = std::make_unique<juce::AudioParameterFloat>(juce::ParameterID { "gain", 1 }, "Gain",
                                                                 juce::NormalisableRange<float>(-60.0f, 12.0f), 0.0f);
            auto m = std::make_unique<juce::AudioParameterChoice>(juce::ParameterID { "mode", 1 }, "Mode",
                                                                  juce::StringArray { "A", "B", "C" }, 1);
            auto b = std::make_unique<juce::AudioParameterBool>(juce::ParameterID { "bypass", 1 }, "Bypass", false);
            gain = g.get();
            mode = m.get();
            bypass = b.get();
            addParameter(g.release());
            addParameter(m.release());
            addParameter(b.release());
        }

        using juce::AudioProcessor::processBlock;       // the double overload stays JUCE's

        const juce::String getName() const override { return "fg.paramport"; }
        void prepareToPlay(double, int) override {}
        void releaseResources() override {}
        void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
        double getTailLengthSeconds() const override { return 0.0; }
        bool acceptsMidi() const override { return false; }
        bool producesMidi() const override { return false; }
        juce::AudioProcessorEditor* createEditor() override { return nullptr; }
        bool hasEditor() const override { return false; }
        int getNumPrograms() override { return 1; }
        int getCurrentProgram() override { return 0; }
        void setCurrentProgram(int) override {}
        const juce::String getProgramName(int) override { return {}; }
        void changeProgramName(int, const juce::String&) override {}
        void getStateInformation(juce::MemoryBlock&) override {}
        void setStateInformation(const void*, int) override {}

        juce::AudioParameterFloat*  gain = nullptr;
        juce::AudioParameterChoice* mode = nullptr;
        juce::AudioParameterBool*   bypass = nullptr;
    };

    // What the host sees: g<index>+ / g<index>- for gesture begin/end, v<index>=<value> for a value change.
    class Recorder final : public juce::AudioProcessorParameter::Listener
    {
    public:
        void parameterValueChanged(int index, float v) override
        {
            char buf[48];
            std::snprintf(buf, sizeof buf, "v%d=%g", index, static_cast<double>(v));
            add(buf);
        }

        void parameterGestureChanged(int index, bool starting) override
        {
            add("g" + std::to_string(index) + (starting ? "+" : "-"));
        }

        std::string take()
        {
            std::string s;
            s.swap(events_);
            return s;
        }

    private:
        void add(const std::string& e)
        {
            if (!events_.empty())
                events_ += ' ';
            events_ += e;
        }

        std::string events_;
    };

    class NullHost final : public funkgui::HostServices
    {
    public:
        void   setUnboundedDrag(bool on) override { unbounded = on; }
        void   showParamMenu(funkgui::ParamPort&, float, float) override {}
        void   nudgeFullRate() override {}
        double nowSeconds() const override { return 0.0; }
        void   beginBatch() override { ++batches; }
        void   endBatch() override {}

        bool unbounded = false;
        int  batches = 0;
    };

    bool sameBits(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }

    void expect(T::Probe& P, std::string_view key, Recorder& r, std::string_view want)
    {
        const std::string got = r.take();
        if (got != want)
            std::printf("INFO     %.*s: got \"%s\", want \"%.*s\"\n", static_cast<int>(key.size()), key.data(),
                        got.c_str(), static_cast<int>(want.size()), want.data());
        P.eq(key, got == want, 1);
    }
}

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    T::Probe P("fg.paramport", "", argc, argv);

    Processor proc;
    Recorder rec;
    for (auto* p : proc.getParameters())
        p->addListener(&rec);

    // The processor owns the parameters and (here) the ports, declared after it so they are destroyed first (K2 #27).
    funkgui::JuceParamPort gain(*proc.gain), mode(*proc.mode), bypass(*proc.bypass);
    const int gi = proc.gain->getParameterIndex();
    const int mi = proc.mode->getParameterIndex();
    // The derived parameter classes make getValue() & co. private; the base interface the port wraps is public.
    const juce::RangedAudioParameter& gainP = *proc.gain;
    const juce::RangedAudioParameter& modeP = *proc.mode;

    // ---- the mapping ------------------------------------------------------------------------------------------------
    P.eq("id.gain", std::strcmp(gain.id(), "gain") == 0, 1);
    P.eq("id.mode", std::strcmp(mode.id(), "mode") == 0, 1);
    P.eq("id.bypass", std::strcmp(bypass.id(), "bypass") == 0, 1);
    P.eq("native.is_the_parameter", gain.native() == static_cast<void*>(proc.gain), 1);
    P.eq("native.round_trips",
         static_cast<juce::RangedAudioParameter*>(mode.native()) == static_cast<juce::RangedAudioParameter*>(proc.mode),
         1);
    P.eq("parameter.accessor", &gain.parameter() == proc.gain, 1);
    P.eq("value01.gain", sameBits(gain.value01(), gainP.getValue()), 1);
    P.eq("value01.mode", sameBits(mode.value01(), modeP.getValue()), 1);
    P.eq("default01.gain", sameBits(gain.default01(), gainP.getDefaultValue()), 1);
    P.near("default01.gain_is_0db", gain.default01(), 60.0 / 72.0, 1e-6);
    P.near("default01.mode_is_index_1", mode.default01(), 0.5, 0.0);
    P.eq("num_steps.gain", gain.numSteps(), gainP.getNumSteps());
    P.eq("num_steps.mode", mode.numSteps(), 3);
    P.eq("num_steps.bypass", bypass.numSteps(), 2);
    P.eq("no_notification_from_reads", rec.take().empty(), 1);

    // ---- begin / set / end reach the host as JUCE's own calls -------------------------------------------------------
    gain.beginGesture();
    gain.setValue01(0.25f);
    gain.endGesture();
    char want[96];
    std::snprintf(want, sizeof want, "g%d+ v%d=0.25 g%d-", gi, gi, gi);
    expect(P, "port.begin_set_end", rec, want);
    P.near("port.set_value01_stored", gainP.getValue(), 0.25, 1e-7);
    P.near("port.plain_value", proc.gain->get(), -60.0 + 0.25 * 72.0, 1e-4);

    // ---- GestureController over JuceParamPorts: the notifications a host records ------------------------------------
    NullHost host;
    {
        funkgui::GestureController g(host);

        g.tap(mode, 1.0f);                               // index 2
        std::snprintf(want, sizeof want, "g%d+ v%d=1 g%d-", mi, mi, mi);
        expect(P, "gesture.tap_choice", rec, want);
        P.eq("gesture.tap_choice_index", proc.mode->getIndex(), 2);
        g.tap(mode, 1.0f);
        expect(P, "gesture.tap_unchanged_sends_nothing", rec, "");

        // A choice quantises what it stores (0.4 -> index 1 -> 0.5); the drag compares with its own last write, so a
        // repeated pointer position is not rewritten although getValue() differs from it.
        g.beginDrag(mode);
        P.eq("gesture.drag_unbounded", host.unbounded, 1);
        g.dragTo(0.4f);
        g.dragTo(0.4f);
        g.endDrag();
        std::snprintf(want, sizeof want, "g%d+ v%d=0.4 g%d-", mi, mi, mi);   // JUCE notifies the written value
        expect(P, "gesture.drag_on_quantising_choice", rec, want);
        P.eq("gesture.drag_choice_index", proc.mode->getIndex(), 1);
        P.eq("gesture.drag_released", host.unbounded, 0);

        g.wheelTo(gain, 0.3f, 0.0);
        g.wheelTo(gain, 0.35f, 0.1);
        g.poll(1.0);
        std::snprintf(want, sizeof want, "g%d+ v%d=0.3 v%d=0.35 g%d-", gi, gi, gi, gi);
        expect(P, "gesture.wheel_burst", rec, want);

        const std::pair<funkgui::ParamPort*, float> writes[] = { { &mode, 0.0f }, { &gain, 0.5f } };
        g.tapMany(writes);
        std::snprintf(want, sizeof want, "g%d+ v%d=0 g%d- g%d+ v%d=0.5 g%d-", mi, mi, mi, gi, gi, gi);
        expect(P, "gesture.tap_many", rec, want);
        P.eq("gesture.tap_many_one_batch", host.batches, 1);

        g.beginDrag(gain);
        g.dragTo(0.75f);
        rec.take();
    }                                                    // destroyed mid-drag: the gesture is ended (HR :163-175)
    std::snprintf(want, sizeof want, "g%d-", gi);
    expect(P, "gesture.destructor_ends_drag", rec, want);

    for (auto* p : proc.getParameters())
        p->removeListener(&rec);
    return P.finish();
}

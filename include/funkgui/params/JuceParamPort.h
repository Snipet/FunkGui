#pragma once

// A ParamPort over a JUCE parameter (02 §5.2; K1 #33). Core, no GPU: it needs juce_audio_processors, which FunkGuiCore
// links. The parameter is a reference owned by the processor, which also owns the port (K2 #27): construct one port per
// APVTS parameter after the processor's parameters exist, and destroy the ports before the parameters. The header
// needs only a forward declaration of the JUCE type; JuceParamPort.cpp includes JUCE.
//
// Mapping: value01 = getValue(), default01 = getDefaultValue(), numSteps = getNumSteps(), beginGesture =
// beginChangeGesture(), setValue01 = setValueNotifyingHost(), endGesture = endChangeGesture(), id = the parameter ID,
// native = &parameter (EditorHost's host menu, getContextMenuForParameter). Message thread only.

#include <funkgui/params/ParamPort.h>

#include <string>

namespace juce
{
    class RangedAudioParameter;
}

namespace funkgui
{
    class JuceParamPort final : public ParamPort
    {
    public:
        explicit JuceParamPort(juce::RangedAudioParameter&);

        JuceParamPort(const JuceParamPort&) = delete;
        JuceParamPort& operator=(const JuceParamPort&) = delete;

        float value01() const override;
        float default01() const override;
        int   numSteps() const override;
        void  beginGesture() override;
        void  setValue01(float) override;
        void  endGesture() override;
        const char* id() const override;
        void* native() const override;

        juce::RangedAudioParameter& parameter() const noexcept { return param_; }

    private:
        juce::RangedAudioParameter& param_;
        std::string id_;                                 // the parameter ID, copied once (id() stays valid)
    };
}

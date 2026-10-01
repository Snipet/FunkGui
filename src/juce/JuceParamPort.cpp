#include <funkgui/params/JuceParamPort.h>

#include <juce_audio_processors/juce_audio_processors.h>

namespace funkgui
{
    JuceParamPort::JuceParamPort(juce::RangedAudioParameter& p)
        : param_(p), id_(p.getParameterID().toStdString())
    {
    }

    float JuceParamPort::value01() const { return param_.getValue(); }

    float JuceParamPort::default01() const { return param_.getDefaultValue(); }

    int JuceParamPort::numSteps() const { return param_.getNumSteps(); }

    void JuceParamPort::beginGesture() { param_.beginChangeGesture(); }

    void JuceParamPort::setValue01(float v) { param_.setValueNotifyingHost(v); }

    void JuceParamPort::endGesture() { param_.endChangeGesture(); }

    const char* JuceParamPort::id() const { return id_.c_str(); }

    void* JuceParamPort::native() const { return &param_; }
}

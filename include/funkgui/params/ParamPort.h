#pragma once

// One host parameter as the widgets see it (02 §5.2). Values are host-normalised 0..1 ("host01"). A port is a
// reference owned elsewhere (K2 #27): FCompressor's processor owns one JuceParamPort per APVTS parameter and hands them
// out through ProcessorFacade::port(), so they outlive every editor; a probe's fake facade owns in-memory ports.
// Widgets never write directly: every write goes through GestureController, which keeps the begin/set/end discipline
// (A §3.5). There is no undo here (K2 #7): hosts record the gestures.
//
// Every call is message-thread only, like the JUCE calls it wraps.

namespace funkgui
{
    class ParamPort
    {
    public:
        virtual ~ParamPort() = default;

        virtual float value01() const = 0;               // host-normalised
        virtual float default01() const = 0;             // host default (NOT the Mode default: that is ValueModel's)
        virtual int   numSteps() const = 0;
        virtual void  beginGesture() = 0;
        virtual void  setValue01(float) = 0;             // setValueNotifyingHost
        virtual void  endGesture() = 0;
        virtual const char* id() const = 0;              // the parameter ID; valid for the port's lifetime
        virtual void* native() const = 0;                // juce::RangedAudioParameter* (host menu); nullptr in fakes
    };
}

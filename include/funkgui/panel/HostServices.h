#pragma once

// What a Panel may ask of its host (02 §3.5). EditorHost implements it over JUCE and the frame pump; HeadlessHost over
// a simulated clock and a call log; a Panel reaches it through attach() and GestureController.

namespace funkgui
{
    class ParamPort;

    class HostServices
    {
    public:
        virtual ~HostServices() = default;

        virtual void   setUnboundedDrag(bool on) = 0;    // enableUnboundedMouseMovement(true, true) in EditorHost
        virtual void   showParamMenu(ParamPort&, float x, float y) = 0;   // host context menu for a parameter
        virtual void   nudgeFullRate() = 0;              // FramePump::nudgeFullRate
        virtual double nowSeconds() const = 0;           // wall clock (EditorHost) or simulated (HeadlessHost)

        // A batch of writes to several parameters (K2 #23; G2 addition). GestureController::tapMany brackets its
        // writes with these, so the product can hold the audio thread on the previous parameter set until the whole
        // set is written (FCompressor: ProcessorFacade::beginBatch/endBatch, where endBatch raises the engine snap).
        // Calls nest, and every beginBatch is matched by one endBatch.
        virtual void   beginBatch() = 0;
        virtual void   endBatch() = 0;
    };
}

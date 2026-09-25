#pragma once

// What a Panel may ask of its host (02 §3.5). EditorHost implements it over JUCE and the frame pump; HeadlessHost over
// a simulated clock and a call log; a Panel reaches it through attach() and GestureController.

namespace juce
{
    class Component;                                 // ownerComponent() only: this header stays JUCE-free
}

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

        // ---- G7b additions (v0.7.1). Not pure: a host written before them keeps compiling and gets the defaults. ----

        // The index of the Theme the Panel's next draw() receives (Theme::byIndex; 0 .. Theme::kCount - 1). Valid from
        // attach() on and at any time: in tick(), in draw(), in an input handler. EditorHost: its <PREFIX>UI_THEME
        // capture override when one is set, else UiPreferences::theme(), which the next frame applies before it ticks
        // the Panel; so a theme cell that writes the preference reads the new index at once, and the frame after the
        // click ticks and draws with it (no matching the Theme's colours, no frame of lag). HeadlessHost: its
        // constructor's themeIdx (0 when that names no theme, as Theme::byIndex draws). Default: 0.
        virtual int themeIndex() const { return 0; }

        // The juce::Component that owns the Panel's window, to anchor a juce::PopupMenu
        // (PopupMenu::Options::withTargetComponent) or parent a juce::FileChooser. EditorHost returns itself, valid for
        // as long as the Panel is attached. nullptr when there is no window (HeadlessHost; the default): the Panel
        // then shows no menu and no chooser.
        virtual juce::Component* ownerComponent() { return nullptr; }
    };
}

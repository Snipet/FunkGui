#pragma once

// What a Panel may ask of its host (02 §3.5). EditorHost implements it over JUCE and the frame pump; HeadlessHost over
// a simulated clock and a call log; a Panel reaches it through attach() and GestureController.

#include <span>

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
        // then shows no menu and no chooser. Its coordinates are editor px (G7c): under a UI zoom, the Panel's
        // position (x, y) is (x, y) * ownerComponent()->getWidth() / width() there. Anchoring to the component itself
        // needs no conversion.
        virtual juce::Component* ownerComponent() { return nullptr; }

        // ---- G7c additions (v0.8.0; FCompressor ADR-68 UI zoom). Not pure: defaults 100 / no-op / empty. ------------
        //
        // A UI zoom scales the whole window uniformly (its size, the render density, input and accessibility
        // coordinates) while the Panel keeps its fixed logical size and draws, hit-tests and lists accessibility in
        // its own px: nothing a Panel receives or records changes with the zoom. These calls exist for a ZOOM control.

        // The zoom the window is drawn at, in percent. EditorHost: its <PREFIX>UI_ZOOM capture pin when one is set
        // (100 under CANVAS_DUMP without one); else 100 when zoomSteps() is empty; else the machine-wide preference (a
        // missing or unlisted value reads as EditorConfig::defaultZoomPercent) reduced to the largest step whose
        // window fits the user area of the editor's display. Like themeIndex(), it answers a setZoomPercent() at
        // once, and the next frame resizes the editor before it ticks the Panel. HeadlessHost: HeadlessHost::setZoom.
        virtual int zoomPercent() const { return 100; }

        // Choose a zoom step (a ZOOM cell's click). EditorHost ignores a value that is not one of zoomSteps(); it
        // stores any other as the machine-wide preference (EditorConfig::zoomPrefKey), and every open editor in the
        // process follows through UiPreferences::revision(), like the theme. A step that does not fit the display is
        // kept as the preference and drawn at the largest step that fits (zoomPercent() says which).
        virtual void setZoomPercent(int /*percent*/) {}

        // The steps a ZOOM control offers, in percent, ascending; empty when the host has no zoom (the default: the
        // window is the Panel's own size, exactly as before v0.8.0). Valid while the Panel is attached.
        virtual std::span<const int> zoomSteps() const { return {}; }
    };
}

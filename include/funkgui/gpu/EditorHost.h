#pragma once

// The live host of a Panel (02 §5.1; A §6.5 "EditorShell"): a juce::AudioProcessorEditor with no painted children
// that runs one Panel on the GPU. It owns the surface lifecycle (render view, BgfxContext registration, fallback screen
// and retry, re-parenting, backing scale), is a FramePump client (one bgfx::frame() per vsync for every editor in the
// process), records each frame with a member Canvas and submits it through BgfxSink, converts JUCE's mouse, wheel,
// key and file-drag events into the Panel's plain input structs, mirrors the Panel's A11yItems through A11yBridge, and
// implements HostServices for the Panel. Every HR lifecycle fix listed in 02 §5.1 "Ported behaviours" lives here.
//
// Placement (G7b, v0.7.1): the render view covers the editor's area of its peer. It follows the editor's own moves and
// resizes (moved(), resized()), a new peer (parentHierarchyChanged()) and, through a juce::ComponentMovementWatcher on
// the editor, every move of an ANCESTOR inside the same peer, which JUCE reports to no method of the editor: JUCE's
// Standalone window lays its content out under the title bar and the "input muted" bar after the view is attached, and
// the view then sat 27 px too high (FCompressor U7). A product needs no watcher of its own.
//
// Zoom (G7c, v0.8.0; FCompressor ADR-68, which revises ADR-06's "one setSize"): EditorConfig::zoomSteps turns on a
// machine-wide UI zoom that scales the whole panel uniformly while the Panel keeps its fixed logical size W x H. At an
// effective zoom z the editor is round(W * z) x round(H * z) px; the drawable is sized in device px at z * backing
// scale and the frame's dpi is that product, so the sink's view transform scales the logical frame by z and every
// pixel snap (hairlines, text) lands on device px exactly as it does for the backing scale alone. The Canvas, the
// PrimList, dump v2 and fingerprints stay logical. Pointer, wheel, drag and file-drag positions reach the Panel
// divided by z; showParamMenu positions are multiplied back; the A11yBridge's children are placed at z times their
// items (A11yBridge::setScale). The effective zoom is, in order: the <PREFIX>UI_ZOOM capture pin; 100 % under
// CANVAS_DUMP (so captures, gui-live and the legacy-hr parity are unchanged); else the preference
// (EditorConfig::zoomPrefKey; a missing or unlisted value reads as the default) reduced to the largest step whose
// window fits the user area of the editor's display (the display under the pointer until the editor has a peer),
// with the preference kept. HostServices::zoomPercent() answers a change at once; the editor resizes (setSize: the
// host resizes its window, and the render view and drawable follow in resized()) at the start of the next frame,
// before that frame ticks, records and submits, the theme's timing. Every open editor follows a preference write
// through UiPreferences::revision(). The fit is evaluated when the zoom is chosen or read and when the editor gets a
// peer. With no zoomSteps (HardwareReverb) none of this runs: the editor is W x H, sized once, as in v0.7.
//
// Capture (02 §5.1 "Diagnostics environment"): CaptureConfig is read once from the <ENV_PREFIX> environment at
// construction. CANVAS_DUMP pins the FramePump to its fallback clock and ignores the real pointer (HR's captureMode_:
// a pointer resting over the new window must not hover a widget); UI_FIXED_DT ticks the Panel with a fixed dt, and
// nowSeconds() then returns that simulated clock, so a capture of a settled state equals HeadlessHost's frame of it.
//
// Teardown (K2 #27): ~EditorHost runs after the derived editor's members are gone, so it touches only what it owns:
// panel->closeGestures(), setUiAttached(false), FramePump::remove, detachSurface(), then the A11yBridge children, then
// the Panel. A product's Panel stops its own workers in its destructor or in the derived editor's (02 §5.1).
//
// Message thread only.

#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Theme.h>
#include <funkgui/gpu/A11yBridge.h>
#include <funkgui/gpu/BgfxSink.h>
#include <funkgui/gpu/FramePump.h>
#include <funkgui/panel/CaptureConfig.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Panel.h>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace funkgui
{
    struct EditorConfig
    {
        int width = 0, height = 0;                   // the Panel's fixed logical size; 0 = its width()/height()
        const char* fallbackTitle = nullptr;         // wordmark on the no-GPU screen; nullptr = FUNKGUI_PRODUCT_NAME
        std::function<void(bool)> setUiAttached;     // telemetry gate keyed to the editor's lifetime (B §3)
        // HostServices::beginBatch/endBatch (K2 #23; a G2 addition to HostServices): the product's batch bracket
        // (FCompressor: ProcessorFacade::beginBatch/endBatch). Empty = no-op.
        std::function<void()> beginBatch, endBatch;

        // UI zoom (G7c, v0.8.0; see "Zoom" above). The defaults are no zoom: the editor is width x height, exactly as
        // in v0.7 (HardwareReverb passes nothing). FCompressor: { 100, 125, 150, 175 }, 125, "uiZoom".
        std::vector<int> zoomSteps;                  // percent, ascending; values outside 25..400 are dropped
        int         defaultZoomPercent = 100;        // the step for a missing or unlisted preference (the nearest
                                                     // step when this one is not listed)
        const char* zoomPrefKey = nullptr;           // UiPreferences int key; nullptr = not persisted (this editor
                                                     // only: no other editor follows its choice)
    };

    class EditorHost : public juce::AudioProcessorEditor, public juce::FileDragAndDropTarget,
                       private FrameClient, private juce::Timer, private HostServices
    {
    public:
        EditorHost(juce::AudioProcessor&, EditorConfig, std::unique_ptr<Panel>);
        ~EditorHost() override;

        EditorHost(const EditorHost&) = delete;
        EditorHost& operator=(const EditorHost&) = delete;

        Panel& panel() noexcept { return *panel_; }

        struct Diagnostics
        {
            uint32_t frames = 0;                     // frames drawn and submitted
            uint32_t overflows = 0;                  // frames BgfxSink dropped (transient buffer full, 02 §4.5)
            uint32_t attachFailures = 0;             // failed surface attaches (render view or BgfxContext::acquire)
            bool     displayLinked = false;          // the FramePump runs off a CADisplayLink (else its timer)
            float    fps = 0.0f;                     // the FramePump's measured rate
            double   scale = 0.0;                    // the backing scale the drawable is sized for (0: no surface)
            int      zoomPercent = 100;              // G7c: the zoom the editor is sized and drawn at (after the
                                                     // display fit and any capture pin; the preference may differ)
        };
        Diagnostics diagnostics() const;
        const CaptureConfig& capture() const noexcept { return capture_; }
        bool surfaceAttached() const noexcept { return surfaceOk_; }
        bool showingFallback() const noexcept { return bgfxFailed_; }

        void paint(juce::Graphics&) override;        // the no-GPU fallback screen only (HR :816-840)
        void resized() override;
        void moved() override;
        void parentHierarchyChanged() override;
        void mouseMove(const juce::MouseEvent&) override;
        void mouseExit(const juce::MouseEvent&) override;
        void mouseDown(const juce::MouseEvent&) override;
        void mouseDrag(const juce::MouseEvent&) override;
        void mouseUp(const juce::MouseEvent&) override;
        void mouseDoubleClick(const juce::MouseEvent&) override;
        void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
        bool keyPressed(const juce::KeyPress&) override;   // Panel::key's result: true keeps Logic/Live from eating keys
        bool isInterestedInFileDrag(const juce::StringArray&) override;
        void filesDropped(const juce::StringArray&, int, int) override;
        // G7b (v0.7.1): Panel::filesDragEnter / filesDragMove / filesDragExit, each followed by a full-rate nudge.
        void fileDragEnter(const juce::StringArray&, int, int) override;
        void fileDragMove(const juce::StringArray&, int, int) override;
        void fileDragExit(const juce::StringArray&) override;

    private:
        // FrameClient
        Result submitFrame(float dt) override;
        void*  frameClockView() const override { return renderView_; }
        // juce::Timer, 10 Hz: Panel::idle(now) even while the display link is stopped (closes wheel gestures)
        void   timerCallback() override;
        // HostServices
        void   setUnboundedDrag(bool on) override;
        void   showParamMenu(ParamPort&, float x, float y) override;
        void   nudgeFullRate() override;
        double nowSeconds() const override;
        void   beginBatch() override;
        void   endBatch() override;
        int    themeIndex() const override;          // G7b: UI_THEME, else UiPreferences::theme() (HostServices.h)
        juce::Component* ownerComponent() override;  // G7b: this editor
        int    zoomPercent() const override;         // G7c: the zoom the next frame draws at ("Zoom" above)
        void   setZoomPercent(int percent) override; // G7c: a listed step: the preference, then the next frame
        std::span<const int> zoomSteps() const override;   // G7c: EditorConfig::zoomSteps, cleaned
        bool   zoomFits(int percent) const override;         // lead (v0.8.0): a listed step that fits (HostServices.h)

        class ParentWatcher;                         // G7b: followPlacement() on an ancestor's move (EditorHost.cpp)

        void   attachSurfaceIfPossible();
        void   detachSurface();
        void   followPlacement();                    // the view onto the editor's area of its peer, then the scale
        void   refreshDrawableScale();
        double backingScaleFor(void* view) const;    // honours UI_SCALE / UI_SCALE_AFTER
        void   applyTheme(int idx);
        void   replayKeys(const std::string& spec);  // UI_KEYS, through keyPressed() with real juce::KeyPresses
        void   syncAccessibility();                  // Panel::accessibility -> A11yBridge (rebuilds on a new structure)
        bool   writeA11yDump(const std::string& path) const;
        bool   writeCanvasDump(const PrimList&) const;  // dump v2, written beside the path and renamed onto it
        void   onOverflow();
        void   updateCursor();
        void   gpuLog(const juce::String&) const;
        PointerEvent pointer(const juce::MouseEvent&) const;

        // G7c zoom ("Zoom" above).
        void   initZoom();                           // steps, default, pin and preference -> zoomTarget_
        bool   isZoomStep(int percent) const noexcept;
        int    readZoomPreference() const;           // the preference as a listed step, else the default
        void   followZoomPreference();               // a preference revision: re-read it, re-target on a change
        void   updateZoomTarget();                   // the pin, or the chosen step fitted to the display
        int    fitZoom(int chosen) const;            // the largest step <= chosen whose window fits the user area
        juce::Rectangle<int> fitArea() const;        // the user area of the editor's display (empty: unknown)
        void   refitZoom();                          // the display may have changed: re-fit, resize now if needed
        void   applyZoom();                          // zoomApplied_ = zoomTarget_: setSize, a11y scale
        int    zoomedSize(int logical, int percent) const noexcept;

        // Declared first, destroyed last: the Canvas, the bridge's children and the gestures refer to it.
        std::unique_ptr<Panel> panel_;
        EditorConfig  config_;
        CaptureConfig capture_;
        bool          captureMode_ = false;          // CANVAS_DUMP: fallback clock, the real pointer ignored
        Canvas        canvas_;
        BgfxSink      sink_;
        A11yBridge    a11y_;
        std::vector<A11yItem> a11yItems_;            // scratch for Panel::accessibility, capacity kept
        uint32_t      a11yRevision_ = 0;
        double        a11ySyncedAt_ = -1.0;

        Theme    theme_ = Theme::graphite();
        int      themeIdx_ = 0;
        uint32_t prefsRevision_ = 0;

        void*  renderView_ = nullptr;
        bool   surfaceOk_ = false;
        bool   bgfxFailed_ = false;                  // a soft state: the fallback shows while the retry continues
        bool   fallbackPainted_ = false;
        int    retryCount_ = 0;
        float  retrySec_ = 0.0f;
        int    physW_ = 0, physH_ = 0;
        double attachedScale_ = 0.0;
        void*  attachedPeer_ = nullptr;
        std::optional<juce::MouseInputSource> dragSource_;   // from mouseDown to mouseUp (unbounded drag)
        bool   unbounded_ = false;

        bool     firstFrameDone_ = false;
        uint32_t ticks_ = 0;                         // Panel::tick calls
        double   seconds_ = 0.0;                     // the Panel's clock: the sum of the dts it was ticked with
        bool     overflowLogged_ = false;
        bool     dumpAttempted_ = false;
        Diagnostics diag_{};

        // G7c zoom. The logical size is EditorConfig's (the Panel's); zoom percents are whole.
        int   logicalW_ = 0, logicalH_ = 0;
        std::vector<int> zoomSteps_;                 // EditorConfig::zoomSteps in 25..400, sorted, unique
        int   zoomDefault_ = 100;                    // a listed step (100 without steps)
        int   zoomPin_ = 0;                          // UI_ZOOM, or 100 under CANVAS_DUMP; 0 = none
        int   zoomChosen_ = 100;                     // the preference as a listed step (or the last setZoomPercent)
        int   zoomTarget_ = 100;                     // what the next frame applies: HostServices::zoomPercent()
        int   zoomApplied_ = 100;                    // what the editor is sized and drawn at: Diagnostics
        float zoomScale_ = 1.0f;                     // zoomApplied_ / 100: editor px per logical px

        // Declared last, destroyed first (and reset first in ~EditorHost): it calls back into the members above.
        std::unique_ptr<ParentWatcher> parentWatcher_;

        JUCE_LEAK_DETECTOR(EditorHost)
    };
}

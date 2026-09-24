#pragma once

// The live host of a Panel (02 §5.1; A §6.5 "EditorShell"): a juce::AudioProcessorEditor with no painted children
// that runs one Panel on the GPU. It owns the surface lifecycle (render view, BgfxContext registration, fallback screen
// and retry, re-parenting, backing scale), is a FramePump client (one bgfx::frame() per vsync for every editor in the
// process), records each frame with a member Canvas and submits it through BgfxSink, converts JUCE's mouse, wheel,
// key and file-drop events into the Panel's plain input structs, mirrors the Panel's A11yItems through A11yBridge, and
// implements HostServices for the Panel. Every HR lifecycle fix listed in 02 §5.1 "Ported behaviours" lives here.
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
#include <string>
#include <vector>

namespace funkgui
{
    struct EditorConfig
    {
        int width = 0, height = 0;                   // fixed size, set once; 0 = the Panel's width()/height()
        const char* fallbackTitle = nullptr;         // wordmark on the no-GPU screen; nullptr = FUNKGUI_PRODUCT_NAME
        std::function<void(bool)> setUiAttached;     // telemetry gate keyed to the editor's lifetime (B §3)
        // HostServices::beginBatch/endBatch (K2 #23; a G2 addition to HostServices): the product's batch bracket
        // (FCompressor: ProcessorFacade::beginBatch/endBatch). Empty = no-op.
        std::function<void()> beginBatch, endBatch;
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

        void   attachSurfaceIfPossible();
        void   detachSurface();
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

        JUCE_LEAK_DETECTOR(EditorHost)
    };
}

#pragma once

// The live host of a Panel in a browser (v0.13.0; FunkGui::web, Emscripten only; FCompressor ADR-93, web Sprint C):
// EditorHost's counterpart (gpu/EditorHost.h) over a canvas element. It owns the canvas's one WebGlSink, sizes the
// canvas, runs the frame clock on requestAnimationFrame, records each frame with a member Canvas and submits it
// through the sink, converts the browser's pointer, wheel and key events into the Panel's plain input structs, and
// implements HostServices for the Panel. The Panel is the caller's and outlives the host (HeadlessHost's rule); the
// constructor attaches it and the clock runs from start().
//
// The frame (EditorHost::submitFrame's order): (1) a change of UiPreferences::revision() is followed (the theme,
// unless pinned; the zoom preference), then a zoom chosen since the last frame is applied: the canvas takes its new
// size before this frame ticks; (2) nothing more happens while the document is hidden; (3) the drawing buffer follows
// the device pixel ratio; (4) Panel::tick(dt), or the pinned dt; (5) the frame is recorded: FrameInfo as EditorHost
// fills it, dpi = physical height / logical height; (6) WebGlSink::submit; a lost context or no context at all is a
// frame not submitted, and the Panel was ticked all the same; (7) Panel::wantsFullRate() is the clock's next rate.
// Panel::idle(nowSeconds()) runs from a 10 Hz timer of its own, started and stopped with the clock.
//
// The clock (web/WebClock.h FrameCadence): single-shot requestAnimationFrame requests, each cancellable, never
// Emscripten's main loop. 60 Hz while the Panel wants full rate (an even divisor of a faster display's rate), 12 Hz
// while it does not, a frame at the next vsync after any input (nudgeFullRate()), nothing while the tab is hidden.
//
// Size. The Panel's logical size W x H is fixed. The canvas's CSS size is the zoomed size, round(W z) x round(H z)
// CSS px, set on the element's style; its drawing buffer is that times the device pixel ratio (the ResizeObserver's
// device-pixel-content-box where the browser has one and it is current, else rounded), set by the sink. A page should
// give the canvas no border and no padding. Zoom is EditorHost's (gpu/EditorHost.h "Zoom"): the capture pin; else
// 100 % without steps; else the preference (a missing or unlisted value is the default) reduced to the largest step
// that fits the window's inner size less the config's margins (the smallest step when none fits, and so in a window
// no larger than the margins; no fit at all where the browser gives the window no size). The fit is evaluated when the
// zoom is chosen or read and when the window is resized. HostServices::zoomPercent() answers a change at once and the
// next frame applies it.
//
// Input (web/WebInput.h has the rules; they are JUCE's): the host's own DOM listeners on the canvas, under one
// AbortController, deliver each event to the Panel synchronously, inside the DOM handler, so a Panel that copies to
// the clipboard does so within the user's gesture. Pointer events with pointer capture (a drag keeps arriving outside
// the canvas) and fractional client coordinates; the middle button is ignored; a release with a click count of two or
// more is pointerUp and then doubleClick. A press whose release never reaches the page (the capture was lost, the
// pointer that pressed moves with no button down or presses again, the document is hidden) ends as a cancelled one:
// pointerUp where the press last was, no doubleClick. preventDefault is called on a wheel or a keydown only when the
// Panel consumed it (the page scrolls and the browser's shortcuts work otherwise), and always on contextmenu. Text
// comes from keydown's `key` alone: no IME, no dead keys. The canvas is given tabindex 0 when it has none (it gets
// keys only while focused, and a press focuses it), touch-action none, user-select none and no outline. Every event
// nudges the clock and is followed by a cursor update (the canvas's CSS cursor). Not here: file drops (a Panel's
// paths mean nothing in a browser), an accessibility mirror, and pointer lock (setUnboundedDrag only records the
// flag; capture already gives positions outside the canvas).
//
// When the document is hidden the Panel's gestures are closed (a hidden tab's timers are throttled, so the 10 Hz idle
// cannot close a wheel gesture there); when it is shown again the preferences are reloaded and the clock nudged.
//
// HostServices: themeIndex() and the zoom calls as EditorHost; services() = menus | clipboard, forwarded to the web
// services (src/web/WebServices.h: a menu in the DOM anchored with the Panel's logical width, navigator.clipboard);
// chooseFiles refuses; commandKeyIsMeta() is the browser's platform (the compile-time default is never Apple's in a
// wasm build); nowSeconds() is the performance clock (performance.now(), the clock of requestAnimationFrame's
// timestamps), or the Panel's simulated clock under a pinned dt. No ownerComponent().
//
// Capture pins come from the config as a value, never from the environment (a browser has none): fixedDt, uiTheme,
// uiZoom and uiScale (with uiScaleAfter). The other CaptureConfig fields are not read. A test drives frame() itself.
//
// Teardown: the destructor stops the clock, removes the listeners, lets the services go (no menu calls back into a
// Panel that is going), closes the Panel's gestures, reports setUiAttached(false), and only then destroys the sink.
//
// This header is plain C++ (it compiles on every host); the class is defined only in FunkGui::web. Main thread only.

#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/core/Theme.h>
#include <funkgui/panel/CaptureConfig.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Input.h>
#include <funkgui/panel/Panel.h>
#include <funkgui/web/WebClock.h>
#include <funkgui/web/WebGlSink.h>
#include <funkgui/web/WebInput.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace funkgui
{
    namespace web
    {
        class WebServices;                           // src/web/WebServices.h: the menu and the clipboard
    }

    struct WebHostConfig
    {
        const char* canvasSelector = "#canvas";      // document.querySelector: the canvas the Panel is drawn on

        // UI zoom, as EditorConfig's: no steps = no zoom, the canvas is the Panel's size.
        std::vector<int> zoomSteps;                  // percent, ascending; values outside 25..400 are dropped
        int         defaultZoomPercent = 100;        // the step for a missing or unlisted preference
        const char* zoomPrefKey = nullptr;           // UiPreferences int key; nullptr = not persisted
        // CSS px of page around the canvas: the zoom is fitted to the window's inner size less these (a window no
        // larger than them takes the smallest step).
        int fitMarginX = 0, fitMarginY = 0;

        std::function<void(bool)> setUiAttached;     // telemetry gate keyed to the host's lifetime
        std::function<void()> beginBatch, endBatch;  // HostServices::beginBatch/endBatch; empty = no-op

        // v0.14.0. Called once in every frame that ticks the Panel, just before Panel::tick: where a product brings
        // in what the frame is about to show (FCompressor pulls its telemetry from the audio thread's module here, so
        // a pull follows the host's own cadence: 60 Hz, 12 Hz idle, none while the document is hidden). It runs on
        // the main thread, inside the frame, so it must not destroy or stop the host. Empty = nothing.
        std::function<void()> beforeTick;

        // The capture pins, as a value (never CaptureConfig::fromEnv()): fixedDt, uiTheme, uiZoom, uiScale and
        // uiScaleAfter are honoured, with EditorHost's meaning; the rest is not read.
        CaptureConfig capture{};
    };

    class WebHost final : private HostServices
    {
    public:
        // Constructs the sink on the config's canvas, sizes the canvas, installs the listeners and attaches the
        // Panel. Never throws and never aborts: without the canvas or without WebGL2, ok() is false, error() says why
        // and frames tick and record without drawing. The clock does not run until start().
        WebHost(Panel&, WebHostConfig);
        ~WebHost() override;

        WebHost(const WebHost&) = delete;
        WebHost& operator=(const WebHost&) = delete;

        bool ok() const noexcept { return sink_.ok(); }                  // the sink's: frames are drawn
        const char* error() const noexcept { return sink_.error(); }

        // The requestAnimationFrame clock and the 10 Hz idle. stop() also closes the Panel's gestures (no idle call
        // will). Both are idempotent.
        void start();
        void stop();
        bool running() const noexcept { return running_; }

        struct FrameResult
        {
            bool submitted = false;                  // the sink drew (or cleared for) this frame
            bool wantsFullRate = false;              // Panel::wantsFullRate() after it
        };

        // One frame at `nowMs` (requestAnimationFrame's timestamp: ms on the performance clock). The clock's entry
        // point; public so that a test can drive frames itself, as FramePump::tick is.
        FrameResult frame(double nowMs);

        // The frame last recorded: valid until the next frame(), empty before the first.
        const PrimList& lastFrame() const;

        WebGlSink& sink() noexcept { return sink_; }
        Panel& panel() noexcept { return panel_; }
        const CaptureConfig& capture() const noexcept { return capture_; }

        struct Diagnostics
        {
            uint32_t frames = 0;                     // frames drawn and submitted
            uint32_t lost = 0;                       // frames not drawn because the context was lost
            uint32_t restores = 0;                   // context restores the sink survived
            float    fps = 0.0f;                     // the clock's measured rate
            double   scale = 0.0;                    // device px per CSS px the drawing buffer is sized for
            int      zoomPercent = 100;              // the zoom the canvas is sized and drawn at
            int      physW = 0, physH = 0;           // the drawing buffer of the last frame, device px
        };
        Diagnostics diagnostics() const;

    private:
        // HostServices
        void   setUnboundedDrag(bool on) override;   // a flag: no pointer lock
        void   showParamMenu(ParamPort&, float x, float y) override;   // nothing: a browser has no host menu
        void   nudgeFullRate() override;
        double nowSeconds() const override;
        void   beginBatch() override;
        void   endBatch() override;
        int    themeIndex() const override;
        int    zoomPercent() const override;
        void   setZoomPercent(int percent) override;
        std::span<const int> zoomSteps() const override;
        bool   zoomFits(int percent) const override;
        unsigned services() const override;          // menus and the clipboard; no file chooser
        bool   showMenu(const MenuRequest&, MenuCallback) override;
        void   dismissMenus() override;
        bool   copyText(std::string_view utf8) override;
        bool   commandKeyIsMeta() const override;

        // The DOM's side (src/web/WebHost.cpp): called through function pointers from the listeners' JavaScript.
        static int  onPointer(void* self, int type, double clientX, double clientY, double left, double top,
                              double width, double height, int button, int buttons, int domMods, double timeMs,
                              int pointerId);
        static int  onWheel(void* self, double clientX, double clientY, double left, double top, double width,
                            double height, double deltaX, double deltaY, int deltaMode, int domMods);
        static int  onKey(void* self, int domMods);
        static void onResize(void* self);
        static void onVisibility(void* self, int hidden);
        static bool onAnimationFrame(double timeMs, void* self);
        static void onWaitEnded(void* self);
        static void onIdle(void* self);

        struct DomPoint
        {
            double clientX, clientY, left, top, width, height;
        };
        PointerEvent pointer(const DomPoint&, int domMods, bool rightButton, int clicks) const;
        Mods   modsOf(int domMods) const noexcept;
        void   endPress(const PointerEvent& at, bool doubleClick);
        void   updateCursor();
        void   applyTheme(int idx);
        void   refreshDrawable();                    // the drawing buffer's size for this frame
        void   requestFrame();                       // one animation frame, unless one is requested
        void   scheduleNext();                       // after a callback: the next request, now or after a wait
        void   stopClock();

        void   initZoom();
        bool   isZoomStep(int percent) const noexcept;
        int    readZoomPreference() const;
        void   followZoomPreference();
        void   updateZoomTarget();
        int    fitZoom(int chosen) const;
        void   applyZoom();

        Panel&        panel_;
        WebHostConfig config_;
        CaptureConfig capture_;
        std::string   selector_;
        std::string   zoomPrefKey_;
        // Declared before the Canvas and the services: destroyed after them, last of all.
        WebGlSink     sink_;
        Canvas        canvas_;
        std::unique_ptr<web::WebServices> services_;

        Theme    theme_ = Theme::graphite();
        int      themeIdx_ = 0;
        uint32_t prefsRevision_ = 0;
        bool     apple_ = false;                     // the browser's platform is Apple's: Meta is the command key

        web::FrameCadence cadence_;
        bool     running_ = false;
        bool     inFrame_ = false;
        int      frameRequest_ = 0;                  // requestAnimationFrame id; 0 = none pending
        int      waitTimer_ = 0;                     // the idle wait's timeout id; 0 = none
        int      idleTimer_ = 0;                     // the 10 Hz interval id; 0 = none
        uint32_t ticks_ = 0;                         // Panel::tick calls
        double   seconds_ = 0.0;                     // the Panel's clock: the sum of the dts it was ticked with
        Diagnostics diag_{};
        int      physW_ = 0, physH_ = 0;
        double   scale_ = 0.0;

        web::ClickCounter clicks_;
        bool     pressed_ = false;                   // between a press the host took and its release
        int      pressedButton_ = 0;
        int      pressedPointer_ = 0;                // its PointerEvent.pointerId
        bool     unbounded_ = false;
        PointerEvent lastPress_{};                   // the last event of the press: where a lost press ends
        Cursor   cursor_ = Cursor::normal;
        bool     cursorSet_ = false;
        char     keyText_[64] = {};                  // the keydown's `key`, written by the listener

        int   logicalW_ = 0, logicalH_ = 0;
        std::vector<int> zoomSteps_;                 // the config's, cleaned
        int   zoomDefault_ = 100;
        int   zoomPin_ = 0;                          // capture.uiZoom; 0 = none
        int   zoomChosen_ = 100;                     // the preference as a listed step (or the last setZoomPercent)
        int   zoomTarget_ = 100;                     // what the next frame applies: HostServices::zoomPercent()
        int   zoomApplied_ = 0;                      // what the canvas is sized at; 0 = not sized yet
    };
}

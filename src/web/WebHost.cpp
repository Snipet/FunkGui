#include <funkgui/web/WebHost.h>

#include "WebServices.h"

#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/text/FontService.h>

#include <emscripten/em_js.h>
#include <emscripten/em_macros.h>
#include <emscripten/emscripten.h>
#include <emscripten/eventloop.h>
#include <emscripten/html5.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

// WebHost (v0.13.0; FCompressor ADR-93, docs/sprints/web/c-webhost.md): EditorHost.cpp's frame, sizing, input and
// HostServices around a canvas element. The rules that need no browser are in web/WebInput.h and web/WebClock.h
// (fg.web.input, fg.web.clock); this file is the DOM around them, and test/web/host.cpp runs it in a browser. Choices
// where the card is silent:
//
// - Listeners. Html5.h's callbacks are not used for input: they give whole-pixel coordinates and no capture, and
//   Emscripten removes a canvas's callbacks with its GL context. The listeners are the host's own, added by
//   funkgui_web_host_install under one AbortController and removed by funkgui_web_host_remove; they call back through
//   function pointers. Their state (the canvas, the controller, the ResizeObserver and its last box) lives in a Map of
//   this module instance keyed by the host's address, Module['funkguiWebHosts'].
// - Capture. A press the host takes (the left or the right button) focuses the canvas and captures the pointer, both
//   inside try blocks: a synthetic event's pointer cannot be captured. A press that could not be captured ends when
//   the pointer leaves the canvas (pointerUp at the last position of the press, then pointerExit), since its release
//   would never arrive; pointercancel ends a press the same way. Neither is a double click.
// - Buttons. pointerdown is the first button to go down and pointerup the last to come up, so a press is one button
//   from the Panel's side whatever else is pressed during it. A release without a press the host took is ignored.
// - Click counts are ClickCounter's, over client px and the events' timeStamp. A move with no press counts 1.
// - A zoom change (a click, a preference revision, a window resize) is applied at the start of the next frame, as in
//   EditorHost; only the constructor sizes the canvas at once. A window resize always nudges: it is also how a
//   browser reports a new device pixel ratio.
// - The device-pixel box is used only when it was measured for the CSS size and the device pixel ratio the canvas has
//   now; in the frame that changes either, and where there is no such box (Safari), the buffer is round(CSS * ratio).
// - FrameInfo::displayLinked says the frames come from requestAnimationFrame (the clock is running), false for a
//   frame a caller drove itself.
// - "The host lets go of the Panel" (HostServices' rule) is ~WebHost: a canvas does not leave its window.
// - EM_JS bodies pass through the C preprocessor, which reads an apostrophe as the start of a character constant: no
//   comments in them.

using FunkGuiWebPointerFn = int (*)(void*, int, double, double, double, double, double, double, int, int, int, double);
using FunkGuiWebWheelFn = int (*)(void*, double, double, double, double, double, double, double, double, int, int);
using FunkGuiWebKeyFn = int (*)(void*, int);
using FunkGuiWebResizeFn = void (*)(void*);
using FunkGuiWebVisibilityFn = void (*)(void*, int);

EM_JS_DEPS(funkgui_web_host_deps, "$UTF8ToString,$stringToUTF8,$getWasmTableEntry");

// Adds the listeners; 1 when the selector names a canvas, 0 when it does not (the window's and the document's
// listeners are added all the same, so the host follows a resize and the tab's visibility).
EM_JS(int, funkgui_web_host_install,
      (void* host, const char* selector, char* keyText, int keyTextSize, FunkGuiWebPointerFn onPointer,
       FunkGuiWebWheelFn onWheel, FunkGuiWebKeyFn onKey, FunkGuiWebResizeFn onResize,
       FunkGuiWebVisibilityFn onVisibility),
{
    if (!Module['funkguiWebHosts']) Module['funkguiWebHosts'] = new Map();
    let canvas = null;
    try {
        canvas = document.querySelector(UTF8ToString(selector));
    } catch (e) {
        canvas = null;
    }
    if (!(canvas instanceof HTMLCanvasElement)) canvas = null;
    const state = { canvas: canvas, abort: new AbortController(), observer: null, box: null };
    Module['funkguiWebHosts'].set(host, state);
    const signal = state.abort.signal;
    const pointer = getWasmTableEntry(onPointer);
    const wheel = getWasmTableEntry(onWheel);
    const key = getWasmTableEntry(onKey);
    const resized = getWasmTableEntry(onResize);
    const visibility = getWasmTableEntry(onVisibility);

    window.addEventListener('resize', () => { resized(host); }, { signal: signal });
    document.addEventListener('visibilitychange', () => { visibility(host, document.hidden ? 1 : 0); },
                              { signal: signal });
    if (!canvas) return 0;

    if (!canvas.hasAttribute('tabindex')) canvas.setAttribute('tabindex', '0');
    canvas.style.touchAction = 'none';
    canvas.style.userSelect = 'none';
    canvas.style.webkitUserSelect = 'none';
    canvas.style.outline = 'none';

    const mods = (e) => (e.shiftKey ? 1 : 0) | (e.ctrlKey ? 2 : 0) | (e.altKey ? 4 : 0) | (e.metaKey ? 8 : 0);
    const send = (type, e) => {
        const r = canvas.getBoundingClientRect();
        return pointer(host, type, e.clientX, e.clientY, r.left, r.top, r.width, r.height, e.button, e.buttons,
                       mods(e), e.timeStamp);
    };
    canvas.addEventListener('pointerdown', (e) => {
        if (!e.isPrimary) return;
        if (send(0, e)) {
            try {
                canvas.focus({ preventScroll: true });
            } catch (x) {
            }
            try {
                canvas.setPointerCapture(e.pointerId);
            } catch (x) {
            }
        }
    }, { signal: signal });
    canvas.addEventListener('pointermove', (e) => { if (e.isPrimary) send(1, e); }, { signal: signal });
    canvas.addEventListener('pointerup', (e) => { if (e.isPrimary) send(2, e); }, { signal: signal });
    canvas.addEventListener('pointercancel', (e) => { if (e.isPrimary) send(3, e); }, { signal: signal });
    canvas.addEventListener('pointerleave', (e) => { if (e.isPrimary) send(4, e); }, { signal: signal });
    canvas.addEventListener('contextmenu', (e) => { e.preventDefault(); }, { signal: signal });
    canvas.addEventListener('wheel', (e) => {
        const r = canvas.getBoundingClientRect();
        if (wheel(host, e.clientX, e.clientY, r.left, r.top, r.width, r.height, e.deltaX, e.deltaY, e.deltaMode,
                  mods(e)))
            e.preventDefault();
    }, { signal: signal, passive: false });
    canvas.addEventListener('keydown', (e) => {
        if (e.isComposing) return;
        stringToUTF8(String(e.key || ""), keyText, keyTextSize);
        if (key(host, mods(e))) e.preventDefault();
    }, { signal: signal });

    if (typeof ResizeObserver !== 'undefined') {
        state.observer = new ResizeObserver((entries) => {
            const entry = entries[entries.length - 1];
            const device = entry.devicePixelContentBoxSize && entry.devicePixelContentBoxSize[0];
            const content = entry.contentBoxSize && entry.contentBoxSize[0];
            state.box = device && content ? { cssW: content.inlineSize, cssH: content.blockSize,
                                              w: device.inlineSize, h: device.blockSize,
                                              ratio: window.devicePixelRatio }
                                          : null;
        });
        try {
            state.observer.observe(canvas, { box: 'device-pixel-content-box' });
        } catch (x) {
            state.observer.disconnect();
            state.observer = null;
        }
    }
    return 1;
});

EM_JS(void, funkgui_web_host_remove, (void* host), {
    const hosts = Module['funkguiWebHosts'];
    const state = hosts && hosts.get(host);
    if (!state) return;
    state.abort.abort();
    if (state.observer) state.observer.disconnect();
    hosts.delete(host);
});

EM_JS(void, funkgui_web_host_css_size, (void* host, int width, int height), {
    const state = Module['funkguiWebHosts'].get(host);
    if (!state || !state.canvas) return;
    state.canvas.style.width = width + 'px';
    state.canvas.style.height = height + 'px';
});

// The canvas in device px as the browser lays it out, packed as width * 65536 + height, when that was measured for
// this CSS size at the device pixel ratio of now; 0 when there is no such measurement.
EM_JS(double, funkgui_web_host_device_box, (void* host, int cssWidth, int cssHeight), {
    const state = Module['funkguiWebHosts'].get(host);
    const box = state && state.box;
    if (!box || box.cssW !== cssWidth || box.cssH !== cssHeight || box.ratio !== window.devicePixelRatio) return 0;
    if (!(box.w >= 1 && box.w < 65536 && box.h >= 1 && box.h < 65536)) return 0;
    return Math.round(box.w) * 65536 + Math.round(box.h);
});

EM_JS(void, funkgui_web_host_cursor, (void* host, const char* name), {
    const state = Module['funkguiWebHosts'].get(host);
    if (state && state.canvas) state.canvas.style.cursor = UTF8ToString(name);
});

EM_JS(int, funkgui_web_host_hidden, (), { return document.hidden ? 1 : 0; });
EM_JS(int, funkgui_web_host_inner_width, (), { return window.innerWidth | 0; });
EM_JS(int, funkgui_web_host_inner_height, (), { return window.innerHeight | 0; });

EM_JS(void, funkgui_web_host_platform, (char* out, int size), {
    const data = navigator.userAgentData;
    stringToUTF8(String((data && data.platform) || navigator.platform || ""), out, size);
});

namespace funkgui
{
    namespace
    {
        constexpr double kIdlePeriodMs = 100.0;      // Panel::idle at 10 Hz, as EditorHost's timer

        // The DOM listeners' event kinds and modifier bits (funkgui_web_host_install).
        enum : int { kDown = 0, kMove = 1, kUp = 2, kCancel = 3, kLeave = 4 };
        constexpr int kDomShift = 1, kDomCtrl = 2, kDomAlt = 4, kDomMeta = 8;
        constexpr int kButtonsRight = 2;             // MouseEvent.buttons: the secondary button
    }

    //==================================================================================================================
    // Lifetime
    //==================================================================================================================

    WebHost::WebHost(Panel& panel, WebHostConfig config)
        : panel_(panel), config_(std::move(config)), capture_(config_.capture),
          selector_(config_.canvasSelector != nullptr ? config_.canvasSelector : ""),
          zoomPrefKey_(config_.zoomPrefKey != nullptr ? config_.zoomPrefKey : ""), sink_(selector_.c_str()),
          canvas_(FontService::get().atlas()), services_(std::make_unique<web::WebServices>(selector_.c_str()))
    {
        // One logical size, the Panel's; the canvas is that times the zoom and changes only with the zoom.
        logicalW_ = panel_.width();
        logicalH_ = panel_.height();

        char platform[64] = {};
        funkgui_web_host_platform(platform, static_cast<int>(sizeof platform));
        apple_ = web::applePlatform(platform);

        // The preference may have been written since this page first read it (another tab); the theme pin overrides
        // it without persisting.
        auto& prefs = UiPreferences::get();
        prefs.reload();
        prefsRevision_ = prefs.revision();
        applyTheme(capture_.uiTheme >= 0 ? capture_.uiTheme : prefs.theme());

        funkgui_web_host_install(this, selector_.c_str(), keyText_, static_cast<int>(sizeof keyText_),
                                 &WebHost::onPointer, &WebHost::onWheel, &WebHost::onKey, &WebHost::onResize,
                                 &WebHost::onVisibility);
        initZoom();
        applyZoom();

        panel_.attach(*this);
        if (config_.setUiAttached)
            config_.setUiAttached(true);
    }

    WebHost::~WebHost()
    {
        stopClock();
        funkgui_web_host_remove(this);               // no event reaches a Panel that is going
        services_->letGo();                          // and no menu calls back into it
        // A page can drop the host with a button still down, in which case the release never arrives; leaving the
        // gesture open would strand the product's parameter write (EditorHost's rule).
        panel_.closeGestures();
        if (config_.setUiAttached)
            config_.setUiAttached(false);
        // The members go in reverse order: the services, the Canvas, and the sink last.
    }

    WebHost::Diagnostics WebHost::diagnostics() const
    {
        Diagnostics d = diag_;
        d.restores = sink_.restoreCount();
        d.fps = cadence_.fps();
        d.scale = scale_;
        d.zoomPercent = zoomApplied_;
        d.physW = physW_;
        d.physH = physH_;
        return d;
    }

    const PrimList& WebHost::lastFrame() const
    {
        // Canvas::end() only returns the list it recorded into (valid until the next begin), so reading it through a
        // const host changes nothing (HeadlessHost::writeDump's idiom).
        return const_cast<Canvas&>(canvas_).end();
    }

    void WebHost::applyTheme(int idx)
    {
        themeIdx_ = std::clamp(idx, 0, Theme::kCount - 1);
        theme_ = Theme::byIndex(themeIdx_);
    }

    //==================================================================================================================
    // The clock
    //==================================================================================================================

    void WebHost::start()
    {
        if (running_)
            return;
        running_ = true;
        idleTimer_ = emscripten_set_interval(&WebHost::onIdle, kIdlePeriodMs, this);
        requestFrame();
    }

    void WebHost::stop()
    {
        if (!running_)
            return;
        stopClock();
        panel_.closeGestures();                      // no idle call is left to close a wheel gesture
    }

    void WebHost::stopClock()
    {
        running_ = false;
        if (frameRequest_ != 0)
            emscripten_cancel_animation_frame(frameRequest_);
        if (waitTimer_ != 0)
            emscripten_clear_timeout(waitTimer_);
        if (idleTimer_ != 0)
            emscripten_clear_interval(idleTimer_);
        frameRequest_ = waitTimer_ = idleTimer_ = 0;
        cadence_.reset();                            // the next start's first frame has no frame before it
    }

    void WebHost::requestFrame()
    {
        if (running_ && frameRequest_ == 0)
            frameRequest_ = emscripten_request_animation_frame(&WebHost::onAnimationFrame, this);
    }

    // After an animation frame: the next one at once while the Panel wants full rate, else after the cadence's wait
    // (a timer, which a nudge cuts short).
    void WebHost::scheduleNext()
    {
        if (!running_ || frameRequest_ != 0 || waitTimer_ != 0)
            return;
        const double wait = cadence_.waitMs(emscripten_get_now());
        if (wait > 0.0)
            waitTimer_ = emscripten_set_timeout(&WebHost::onWaitEnded, wait, this);
        else
            requestFrame();
    }

    bool WebHost::onAnimationFrame(double timeMs, void* self)
    {
        auto& host = *static_cast<WebHost*>(self);
        host.frameRequest_ = 0;
        if (host.cadence_.due(timeMs, funkgui_web_host_hidden() != 0))
            host.frame(timeMs);
        host.scheduleNext();
        return false;                                // single-shot: the next request is scheduleNext's
    }

    void WebHost::onWaitEnded(void* self)
    {
        auto& host = *static_cast<WebHost*>(self);
        host.waitTimer_ = 0;
        host.requestFrame();
    }

    void WebHost::onIdle(void* self)
    {
        auto& host = *static_cast<WebHost*>(self);
        host.panel_.idle(host.nowSeconds());
    }

    //==================================================================================================================
    // The frame (EditorHost::submitFrame's order)
    //==================================================================================================================

    WebHost::FrameResult WebHost::frame(double nowMs)
    {
        FrameResult result;
        if (inFrame_)
            return result;                           // re-entrancy guard (FramePump::tick's)
        inFrame_ = true;
        const float dt = cadence_.advance(nowMs);

        // 1. Follow the preferences when something in this page changes them: the theme, skipped under the theme pin,
        //    which would otherwise be undone at once; the zoom. Then apply a zoom chosen since the last frame: the
        //    canvas takes the new size before this frame ticks, records and submits.
        {
            auto& prefs = UiPreferences::get();
            if (prefs.revision() != prefsRevision_)
            {
                prefsRevision_ = prefs.revision();
                if (capture_.uiTheme < 0)
                    applyTheme(prefs.theme());
                followZoomPreference();
            }
            if (zoomTarget_ != zoomApplied_)
                applyZoom();
        }

        // 2. A hidden tab draws nothing and its Panel is not ticked.
        if (funkgui_web_host_hidden() != 0)
        {
            cadence_.requestRate(false);
            inFrame_ = false;
            return result;
        }

        // 3. Follow the device pixel ratio (another display, the browser's own zoom).
        refreshDrawable();

        // 4. Advance the Panel: seconds, never frames; the pinned dt when one is set.
        const float tickDt = capture_.fixedDt > 0.0f ? capture_.fixedDt : dt;
        panel_.tick(tickDt);
        seconds_ += static_cast<double>(tickDt);
        ++ticks_;

        // 5. Record the frame.
        FrameInfo info;
        info.logicalW = panel_.width();
        info.logicalH = panel_.height();
        // Device px per logical px: the device pixel ratio times the zoom. The drawing buffer is the zoomed canvas
        // in device px, and the logical height is the Panel's.
        float dpi = logicalH_ > 0 ? static_cast<float>(physH_) / static_cast<float>(logicalH_) : 1.0f;
        if (!(dpi > 0.05f))
            dpi = 1.0f;
        info.dpi = dpi;
        info.clear = theme_.ground;
        info.textGamma = theme_.textGamma;
        info.theme = themeIdx_;
        info.seconds = static_cast<float>(seconds_);
        info.frame = ticks_;
        info.dt = tickDt;
        info.fixedClock = capture_.fixedDt > 0.0f;
        info.displayLinked = running_;
        info.fps = info.fixedClock ? 0.0f : cadence_.fps();
        info.fullRate = cadence_.wantedFullRate();   // the request before this frame, as FramePump reports it
        canvas_.begin(info);
        panel_.draw(canvas_, theme_);
        const PrimList& list = canvas_.end();

        // 6. Submit. The browser presents the canvas when this task ends.
        const uint32_t drawnBefore = sink_.frameCount();
        const WebGlSink::Result drawn = sink_.submit(list, physW_, physH_);
        if (sink_.frameCount() != drawnBefore)
        {
            ++diag_.frames;
            result.submitted = true;
        }
        else if (drawn == WebGlSink::Result::lost)
            ++diag_.lost;

        // 7. Full rate only while something moves.
        result.wantsFullRate = panel_.wantsFullRate();
        cadence_.requestRate(result.wantsFullRate);
        inFrame_ = false;
        return result;
    }

    // The drawing buffer for this frame: the canvas's CSS size in device px. The scale is the browser's device pixel
    // ratio, or the capture's uiScale from frame uiScaleAfter on (EditorHost::backingScaleFor).
    void WebHost::refreshDrawable()
    {
        const int cssW = web::zoomedSize(logicalW_, zoomApplied_), cssH = web::zoomedSize(logicalH_, zoomApplied_);
        const bool pinned = capture_.uiScale > 0.0f && diag_.frames >= static_cast<uint32_t>(capture_.uiScaleAfter);
        double scale = pinned ? static_cast<double>(capture_.uiScale) : emscripten_get_device_pixel_ratio();
        if (!(scale > 0.0))
            scale = 1.0;
        scale_ = scale;
        // What the browser itself measured, where it did: round(CSS * ratio) can be a pixel off the true box.
        const double box = pinned ? 0.0 : funkgui_web_host_device_box(this, cssW, cssH);
        if (box > 0.0)
        {
            physW_ = static_cast<int>(box / 65536.0);
            physH_ = static_cast<int>(box - static_cast<double>(physW_) * 65536.0);
            return;
        }
        physW_ = static_cast<int>(std::lround(static_cast<double>(cssW) * scale));
        physH_ = static_cast<int>(std::lround(static_cast<double>(cssH) * scale));
    }

    //==================================================================================================================
    // Input: DOM events -> the Panel's plain structs (web/WebInput.h has the rules)
    //==================================================================================================================

    Mods WebHost::modsOf(int domMods) const noexcept
    {
        return web::modsFromDom((domMods & kDomShift) != 0, (domMods & kDomCtrl) != 0, (domMods & kDomAlt) != 0,
                                (domMods & kDomMeta) != 0, apple_);
    }

    // Client px -> the Panel's logical px, through the canvas's box.
    PointerEvent WebHost::pointer(const DomPoint& at, int domMods, bool rightButton, int clicks) const
    {
        PointerEvent p;
        p.x = web::toLogical(at.clientX, at.left, at.width, logicalW_);
        p.y = web::toLogical(at.clientY, at.top, at.height, logicalH_);
        p.mods = modsOf(domMods);
        p.clicks = clicks;
        p.popup = web::popupFromDom(rightButton, (domMods & kDomCtrl) != 0, apple_);
        return p;
    }

    void WebHost::updateCursor()
    {
        const Cursor c = panel_.cursor();
        if (cursorSet_ && c == cursor_)
            return;                                  // the style is written only when it changes
        cursor_ = c;
        cursorSet_ = true;
        funkgui_web_host_cursor(this, web::cssCursor(c));
    }

    // The press ends at `at`: pointerUp, then (JUCE's order) doubleClick for a release that counts two or more.
    void WebHost::endPress(const PointerEvent& at, bool doubleClick)
    {
        pressed_ = false;
        panel_.pointerUp(at);
        unbounded_ = false;                          // the widget ended its drag (or never started one)
        updateCursor();
        if (doubleClick)
        {
            nudgeFullRate();
            panel_.doubleClick(at);
            updateCursor();
        }
    }

    int WebHost::onPointer(void* self, int type, double clientX, double clientY, double left, double top, double width,
                           double height, int button, int buttons, int domMods, double timeMs)
    {
        auto& host = *static_cast<WebHost*>(self);
        const DomPoint at{ clientX, clientY, left, top, width, height };
        switch (type)
        {
            case kDown:
            {
                if (!web::takesButton(button) || host.pressed_)
                    return 0;                        // the middle button; a press during a press
                host.nudgeFullRate();
                host.pressed_ = true;
                host.pressedButton_ = button;
                const int clicks = host.clicks_.down(timeMs, clientX, clientY, button);
                host.lastPress_ = host.pointer(at, domMods, button == web::kButtonRight, clicks);
                host.panel_.pointerDown(host.lastPress_);
                host.updateCursor();
                return 1;                            // the listener focuses the canvas and captures the pointer
            }
            case kMove:
            {
                // The panel idles at a low rate; ask for the full one the moment the pointer arrives.
                host.nudgeFullRate();
                if (host.pressed_)
                {
                    host.clicks_.moved(clientX, clientY);
                    host.lastPress_ = host.pointer(at, domMods, host.pressedButton_ == web::kButtonRight,
                                                   host.clicks_.count(timeMs));
                    host.panel_.pointerDrag(host.lastPress_);
                }
                else
                    host.panel_.pointerMove(host.pointer(at, domMods, (buttons & kButtonsRight) != 0, 1));
                host.updateCursor();
                return 0;
            }
            case kUp:
            {
                if (!host.pressed_)
                    return 0;                        // the release of a press the host did not take
                const int clicks = host.clicks_.count(timeMs);
                host.endPress(host.pointer(at, domMods, host.pressedButton_ == web::kButtonRight, clicks),
                              clicks >= 2);
                return 0;
            }
            case kCancel:
            case kLeave:
            {
                // With the pointer captured no leave arrives during a press, so a press that is still open here
                // will never see its release: it ends where it last was.
                if (host.pressed_)
                    host.endPress(host.lastPress_, false);
                host.panel_.pointerExit();
                host.updateCursor();
                return 0;
            }
            default: break;
        }
        return 0;
    }

    int WebHost::onWheel(void* self, double clientX, double clientY, double left, double top, double width,
                         double height, double deltaX, double deltaY, int deltaMode, int domMods)
    {
        auto& host = *static_cast<WebHost*>(self);
        WheelEvent w = web::wheelFromDom(deltaX, deltaY, deltaMode);
        w.x = web::toLogical(clientX, left, width, host.logicalW_);
        w.y = web::toLogical(clientY, top, height, host.logicalH_);
        w.mods = host.modsOf(domMods);
        host.nudgeFullRate();
        // Hit-tested by the widgets: a wheel the Panel does not use is the page's (it scrolls), so the listener
        // prevents the default only for one that was used.
        const bool used = host.panel_.wheel(w);
        host.updateCursor();
        return used ? 1 : 0;
    }

    int WebHost::onKey(void* self, int domMods)
    {
        auto& host = *static_cast<WebHost*>(self);
        host.keyText_[sizeof host.keyText_ - 1] = '\0';
        KeyEvent ev;
        if (!web::keyFromDom(host.keyText_, host.modsOf(domMods), ev))
            return 0;                                // a modifier, a dead key, a function key: the browser's
        host.nudgeFullRate();
        const bool used = host.panel_.key(ev);       // true keeps the browser's own shortcut from running
        host.updateCursor();
        return used ? 1 : 0;
    }

    void WebHost::onResize(void* self)
    {
        auto& host = *static_cast<WebHost*>(self);
        if (host.zoomPin_ == 0 && !host.zoomSteps_.empty())
            host.updateZoomTarget();                 // the window may fit another step now; the next frame applies it
        host.nudgeFullRate();
    }

    void WebHost::onVisibility(void* self, int hidden)
    {
        auto& host = *static_cast<WebHost*>(self);
        if (hidden != 0)
        {
            host.panel_.closeGestures();             // a hidden tab's timers are throttled: no idle call closes them
            return;
        }
        UiPreferences::get().reload();               // another tab may have written them meanwhile
        host.nudgeFullRate();
    }

    //==================================================================================================================
    // HostServices
    //==================================================================================================================

    void WebHost::setUnboundedDrag(bool on)
    {
        // No pointer lock: a browser shows a notice for it at every drag. Pointer capture already keeps the drag
        // arriving outside the canvas, to the edge of the screen.
        unbounded_ = on;
    }

    void WebHost::showParamMenu(ParamPort&, float, float) {}

    void WebHost::nudgeFullRate()
    {
        cadence_.nudge();
        if (running_ && waitTimer_ != 0)
        {
            emscripten_clear_timeout(waitTimer_);    // the idle wait ends now: the next vsync draws
            waitTimer_ = 0;
            requestFrame();
        }
    }

    double WebHost::nowSeconds() const
    {
        return capture_.fixedDt > 0.0f ? seconds_ : emscripten_get_now() * 0.001;
    }

    void WebHost::beginBatch()
    {
        if (config_.beginBatch)
            config_.beginBatch();
    }

    void WebHost::endBatch()
    {
        if (config_.endBatch)
            config_.endBatch();
    }

    // What the next frame draws with: frame()'s step 1 sets themeIdx_ to the preference before the Panel is ticked.
    int WebHost::themeIndex() const
    {
        return capture_.uiTheme >= 0 ? themeIdx_ : UiPreferences::get().theme();
    }

    unsigned WebHost::services() const
    {
        return hostservice::menus | hostservice::clipboard;
    }

    bool WebHost::showMenu(const MenuRequest& request, MenuCallback done)
    {
        // The services place the anchor themselves: the canvas's CSS width over this width is CSS px per Panel px.
        return services_->showMenu(request, std::move(done), static_cast<float>(logicalW_));
    }

    void WebHost::dismissMenus()
    {
        services_->dismissMenus();
    }

    bool WebHost::copyText(std::string_view utf8)
    {
        return services_->copyText(utf8);
    }

    bool WebHost::commandKeyIsMeta() const
    {
        return apple_;
    }

    //==================================================================================================================
    // Zoom (EditorHost's, over web/WebClock.h's helpers)
    //==================================================================================================================

    int WebHost::zoomPercent() const
    {
        return zoomTarget_;
    }

    std::span<const int> WebHost::zoomSteps() const
    {
        return zoomSteps_;
    }

    void WebHost::setZoomPercent(int percent)
    {
        if (!isZoomStep(percent))
            return;                                  // not a step this product offers: nothing written, nothing moves
        if (!zoomPrefKey_.empty())
            UiPreferences::get().setInt(zoomPrefKey_.c_str(), percent);
        zoomChosen_ = percent;
        updateZoomTarget();
        nudgeFullRate();                             // the next frame, soon, applies it
    }

    bool WebHost::zoomFits(int percent) const
    {
        if (!isZoomStep(percent))
            return false;
        return zoomPin_ > 0 || fitZoom(percent) == percent;
    }

    bool WebHost::isZoomStep(int percent) const noexcept
    {
        return std::binary_search(zoomSteps_.begin(), zoomSteps_.end(), percent);
    }

    void WebHost::initZoom()
    {
        zoomSteps_ = web::cleanZoomSteps(config_.zoomSteps);
        zoomDefault_ = web::nearestZoomStep(zoomSteps_, config_.defaultZoomPercent);
        const int pin = capture_.uiZoom;
        zoomPin_ = pin >= web::kMinZoomPercent && pin <= web::kMaxZoomPercent ? pin : 0;
        zoomChosen_ = readZoomPreference();
        updateZoomTarget();
    }

    int WebHost::readZoomPreference() const
    {
        if (zoomSteps_.empty())
            return 100;
        if (zoomPrefKey_.empty())
            return zoomDefault_;                     // not persisted: every host opens at the default
        const int v = UiPreferences::get().getInt(zoomPrefKey_.c_str(), zoomDefault_, std::numeric_limits<int>::min(),
                                                  std::numeric_limits<int>::max());
        return isZoomStep(v) ? v : zoomDefault_;
    }

    void WebHost::followZoomPreference()
    {
        if (zoomSteps_.empty() || zoomPrefKey_.empty())
            return;
        const int chosen = readZoomPreference();
        if (chosen == zoomChosen_)
            return;
        zoomChosen_ = chosen;
        updateZoomTarget();
    }

    void WebHost::updateZoomTarget()
    {
        if (zoomPin_ > 0)
            zoomTarget_ = zoomPin_;                  // a pin is applied whatever the steps and never fitted
        else if (zoomSteps_.empty())
            zoomTarget_ = 100;
        else
            zoomTarget_ = fitZoom(zoomChosen_);
    }

    // The largest step <= chosen whose canvas fits the window less the page's margins around it.
    int WebHost::fitZoom(int chosen) const
    {
        return web::fitZoom(zoomSteps_, chosen, logicalW_, logicalH_,
                            funkgui_web_host_inner_width() - config_.fitMarginX,
                            funkgui_web_host_inner_height() - config_.fitMarginY);
    }

    void WebHost::applyZoom()
    {
        zoomApplied_ = zoomTarget_;
        funkgui_web_host_css_size(this, web::zoomedSize(logicalW_, zoomApplied_),
                                  web::zoomedSize(logicalH_, zoomApplied_));
    }
}

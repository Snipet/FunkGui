// The browser check of FunkGui's WebHost (v0.13.0; FCompressor ADR-93, docs/sprints/web-c.md G-D). Not a
// self-registered test (this file has no test line): node has neither a DOM nor WebGL. test/web/CMakeLists.txt builds
// it, in the `web` preset only, into <build>/test/web/{host.html, funkgui_web_host.js, funkgui_web_host.wasm};
// tools/web/check-page.mjs <dir> --page host (CTest fg.web.host, label live) serves that directory on 127.0.0.1 and
// runs it in headless Chrome. Or serve it and open it:
//
//   python3 -m http.server 8137 --bind 127.0.0.1 --directory <build>/test/web      http://127.0.0.1:8137/host.html
//
// The verdict is the sink page's (test/web/page.cpp): document.title is "RUNNING" until the end, then "PASS" or
// "FAIL: <the first thing that failed>"; every line goes to console.log and to the page's <pre>. A failure of the
// page's own hooks (host.html: an uncaught error, an unhandled rejection, an abort; a host's listeners, animation
// frames and timers run outside this file's calls, so a trap in one arrives there) is kept and is part of the verdict
// as a failed claim is: no PASS replaces it. PASS means all of:
//
// 1. Parity. Per gallery case (tools/GalleryApp/GalleryLive.cpp's, less the two a web host cannot share: the overflow
//    of bgfx's transient buffer, and the services section, which draws the file chooser a web host does not report;
//    then every other gallery section at rest), a WebHost over the section under the pins {dt 1/60 s, scale 2,
//    theme 0, zoom 100}, driven by frame() and by synthetic keydown events for the case's keys, records the frame a
//    HeadlessHost(dpi 2, theme 0) draws after the same script: every fingerprint field equal, the static primitives
//    equal bit for bit, dpi exactly 2. The frame as WebGL drew it through the host is within the sink page's bounds
//    of SoftRaster's image of that list.
// 2. Sizing. At zoom 150 a 240 x 160 Panel's canvas is 360 x 240 CSS px with a 720 x 480 drawing buffer under scale 2,
//    and the frame's dpi is 480 / 160 = 3. A zoom chosen by the Panel answers at once and resizes the canvas and its
//    buffer in the next frame; another writer of the preference is followed; the fit follows the window and the
//    config's margins, and a window resize refits; a scale that changes resizes the buffer alone.
// 3. Input. Synthetic DOM events on the canvas at zoom 150 reach a recording Panel at the exact logical position,
//    with the modifiers, the popup rule and the click count of web/WebInput.h, in JUCE's order (up, then doubleClick);
//    a press asks the canvas to capture the event's pointer, and a drag goes on outside the canvas; the middle button
//    and a second pointer are ignored; a press that cannot be captured ends when the pointer leaves; a press whose
//    release never reaches the page (the pointer that pressed moves with no button down or presses again, the canvas
//    loses the capture, the document is hidden) ends where it last was; the wheel arrives in JUCE's units; keys
//    arrive by EditorHost's table; preventDefault is what the Panel returned for a wheel and a key and always for
//    contextmenu; the canvas's CSS cursor follows Panel::cursor().
// 4. HostServices. services() is menus | clipboard; chooseFiles refuses and drops its callback; commandKeyIsMeta() is
//    the browser's platform; the theme and zoom calls answer as EditorHost's. showMenu, dismissMenus and copyText
//    reach the web services with the request, the Panel's LOGICAL width whatever the zoom, and the callback, and what
//    the services answer is what the Panel gets back; a Panel that copies inside a press does so inside the DOM
//    event's handler. The services here are a recording double: this file defines funkgui::web::WebServices, the seam
//    of src/web/WebServices.h, in place of src/web/WebServices.cpp, so these rows hold whatever is behind it.
// 5. Teardown. ~WebHost removes the listeners, then lets the services go, then closes the Panel's gestures, then
//    reports setUiAttached(false), and destroys the sink last; nothing reaches the Panel afterwards, and none of the
//    host's listeners is left on the window or the document (the page counts the calls of every resize and
//    visibilitychange listener registered while it runs).
// 6. What a host asks of its clock, driven by frame() with no real time: FrameInfo::fullRate is the request before
//    the frame; a press, a wheel, a key and the document shown again each ask for full rate until the next frame; the
//    preferences and a chosen zoom are followed by a frame of a hidden document, which ticks and draws nothing.
//    Then the clock itself (the only rows that depend on real time; their bounds are loose and one-sided where a busy
//    machine could be late): start() draws at the idle rate from requestAnimationFrame, with no animation frame
//    requested while it waits, and calls Panel::idle from its timer; a nudge is drawn before an idle frame would be,
//    and so is the first frame of a document shown again; a Panel that wants full rate gets it and no more than the
//    cap; a hidden document draws nothing and closes the gestures; stop() stops both and start() starts them again.
// 7. A context loss forced with WEBGL_lose_context: frame() ticks the Panel, submits nothing and counts the frame as
//    lost; after the restore the host draws again, the same pixels byte for byte.
// 8. The product's hook (WebHostConfig::beforeTick, v0.14.0; FCompressor docs/sprints/web-d.md G-F). Driven by
//    frame(): the hook is called exactly once in every frame that ticks the Panel, before that tick, so the tick sees
//    what the hook brought in the same frame; a frame that ticks without drawing (no canvas) calls it all the same; a
//    frame of a hidden document, which ticks nothing, does not; it runs inside the frame (a frame() from it is
//    refused); the teardown does not call it, and the host takes it along; an empty hook is nothing. Then under the
//    clock (counts, never times): one call before each tick at the idle rate and at full rate and none from the idle
//    timer; none while the document is hidden, none after stop(), again after start(), none after the host is
//    destroyed.

#include "../../src/web/WebServices.h"
#include "../gallery/GalleryPanel.h"

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/Fingerprint.h>
#include <funkgui/canvas/Prim.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/canvas/SoftRaster.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/core/Theme.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Input.h>
#include <funkgui/panel/Panel.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/text/FontService.h>
#include <funkgui/web/WebClock.h>
#include <funkgui/web/WebHost.h>
#include <funkgui/web/WebInput.h>

#include <emscripten/em_js.h>
#include <emscripten/em_macros.h>
#include <emscripten/emscripten.h>
#include <emscripten/eventloop.h>
#include <emscripten/html5.h>

#include <algorithm>
#include <bit>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace G = funkgui::gallery;
namespace web = funkgui::web;
using funkgui::Cursor;
using funkgui::Image;
using funkgui::Key;
using funkgui::KeyEvent;
using funkgui::MenuRequest;
using funkgui::PointerEvent;
using funkgui::PrimList;
using funkgui::Theme;
using funkgui::UiPreferences;
using funkgui::WebHost;
using funkgui::WebHostConfig;
using funkgui::WheelEvent;

// ---- the page's side of the check, in JavaScript -------------------------------------------------------------------
EM_JS_DEPS(funkgui_web_host_page_deps, "$UTF8ToString,$stringToUTF8");

EM_JS(void, fg_host_line, (const char* text), {
    const line = UTF8ToString(text);
    console.log(line);
    const pre = document.getElementById('funkgui-log');
    if (pre) pre.textContent += line + '\n';
});

EM_JS(void, fg_host_title, (const char* text), { document.title = UTF8ToString(text); });

// The page's state, and its watch on what the module under test asks of the browser from here on. Each call still
// reaches the browser as it was made: the resize listeners of the window and the visibilitychange listeners of the
// document are counted when they run (with their options, so the signal that removes them, passed on), the canvas's
// setPointerCapture calls are listed, and the animation frames requested and not yet delivered or cancelled are kept.
EM_JS(void, fg_host_setup, (const char* selector), {
    const page = globalThis.funkguiHostPage = { canvas: document.querySelector(UTF8ToString(selector)),
                                                dispatching: 0, inner: null, ext: null, heard: [0, 0], captures: [],
                                                requested: new Set() };
    const count = (target, type, slot) => {
        const add = target.addEventListener;
        target.addEventListener = function (name, listener, options) {
            const counted = name === type && typeof listener === "function"
                ? function (event) { page.heard[slot]++; return listener.call(this, event); }
                : listener;
            return add.call(this, name, counted, options);
        };
    };
    count(window, "resize", 0);
    count(document, "visibilitychange", 1);
    const capture = page.canvas.setPointerCapture;
    page.canvas.setPointerCapture = function (id) {
        page.captures.push(id);
        return capture.call(this, id);
    };
    const request = window.requestAnimationFrame, cancel = window.cancelAnimationFrame;
    window.requestAnimationFrame = function (callback) {
        const id = request.call(window, function (time) {
            page.requested.delete(id);
            return callback(time);
        });
        page.requested.add(id);
        return id;
    };
    window.cancelAnimationFrame = function (id) {
        page.requested.delete(id);
        return cancel.call(window, id);
    };
});

// How often a listener of the window's resize (0) or of the document's visibilitychange (1) has run.
EM_JS(int, fg_host_heard, (int what), { return globalThis.funkguiHostPage.heard[what]; });

// The canvas's setPointerCapture calls: how many, and the pointer id of the last (-1 before the first).
EM_JS(int, fg_host_captures, (), { return globalThis.funkguiHostPage.captures.length; });
EM_JS(int, fg_host_captured, (), {
    const captures = globalThis.funkguiHostPage.captures;
    return captures.length > 0 ? captures[captures.length - 1] : -1;
});

// The animation frames requested and neither delivered nor cancelled yet.
EM_JS(int, fg_host_frames_requested, (), { return globalThis.funkguiHostPage.requested.size; });

// The failure the page's own hooks kept (host.html), "" when there is none.
EM_JS(void, fg_host_hook_failure, (char* out, int size), {
    stringToUTF8(String(globalThis.funkguiFailure || ""), out, size);
});

// A synthetic PointerEvent on the canvas at (x, y) CSS px from its corner. mods: shift 1, ctrl 2, alt 4, meta 8;
// 16 makes the pointer a second one (not primary), 32 another primary one (a pen, pointer id 7, where the mouse is
// pointer id 1). Returns whether the default was prevented.
EM_JS(int, fg_host_pointer, (const char* type, double x, double y, int button, int buttons, int mods), {
    const page = globalThis.funkguiHostPage;
    const box = page.canvas.getBoundingClientRect();
    const event = new PointerEvent(UTF8ToString(type), {
        clientX: box.left + x, clientY: box.top + y, button: button, buttons: buttons,
        shiftKey: (mods & 1) !== 0, ctrlKey: (mods & 2) !== 0, altKey: (mods & 4) !== 0, metaKey: (mods & 8) !== 0,
        pointerId: (mods & 16) !== 0 ? 2 : (mods & 32) !== 0 ? 7 : 1, pointerType: (mods & 32) !== 0 ? "pen" : "mouse",
        isPrimary: (mods & 16) === 0, bubbles: true, cancelable: true });
    page.dispatching++;
    try {
        page.canvas.dispatchEvent(event);
    } finally {
        page.dispatching--;
    }
    return event.defaultPrevented ? 1 : 0;
});

EM_JS(int, fg_host_wheel, (double x, double y, double deltaX, double deltaY, int deltaMode, int mods), {
    const page = globalThis.funkguiHostPage;
    const box = page.canvas.getBoundingClientRect();
    const event = new WheelEvent("wheel", {
        clientX: box.left + x, clientY: box.top + y, deltaX: deltaX, deltaY: deltaY, deltaMode: deltaMode,
        shiftKey: (mods & 1) !== 0, ctrlKey: (mods & 2) !== 0, altKey: (mods & 4) !== 0, metaKey: (mods & 8) !== 0,
        bubbles: true, cancelable: true });
    page.dispatching++;
    try {
        page.canvas.dispatchEvent(event);
    } finally {
        page.dispatching--;
    }
    return event.defaultPrevented ? 1 : 0;
});

EM_JS(int, fg_host_key, (const char* key, int mods), {
    const page = globalThis.funkguiHostPage;
    const event = new KeyboardEvent("keydown", {
        key: UTF8ToString(key), shiftKey: (mods & 1) !== 0, ctrlKey: (mods & 2) !== 0, altKey: (mods & 4) !== 0,
        metaKey: (mods & 8) !== 0, bubbles: true, cancelable: true });
    page.dispatching++;
    try {
        page.canvas.dispatchEvent(event);
    } finally {
        page.dispatching--;
    }
    return event.defaultPrevented ? 1 : 0;
});

// A plain MouseEvent of `type` on the canvas ("contextmenu").
EM_JS(int, fg_host_mouse, (const char* type), {
    const event = new MouseEvent(UTF8ToString(type), { bubbles: true, cancelable: true });
    globalThis.funkguiHostPage.canvas.dispatchEvent(event);
    return event.defaultPrevented ? 1 : 0;
});

// An Event of `type` on the window (target 0) or on the document (target 1).
EM_JS(void, fg_host_event, (int target, const char* type), {
    (target ? document : window).dispatchEvent(new Event(UTF8ToString(type)));
});

EM_JS(int, fg_host_dispatching, (), { return globalThis.funkguiHostPage.dispatching > 0 ? 1 : 0; });

// The canvas as the browser has it: 0, 1 its style's width and height in px; 2, 3 its laid-out box; 4, 5 its drawing
// buffer; 6, 7 its box's left and top.
EM_JS(double, fg_host_canvas, (int what), {
    const canvas = globalThis.funkguiHostPage.canvas;
    const box = canvas.getBoundingClientRect();
    switch (what) {
        case 0: return parseFloat(canvas.style.width);
        case 1: return parseFloat(canvas.style.height);
        case 2: return box.width;
        case 3: return box.height;
        case 4: return canvas.width;
        case 5: return canvas.height;
        case 6: return box.left;
        default: return box.top;
    }
});

// Text about the canvas: 0 its CSS cursor, 1 its tabindex attribute, 2 its touch-action, 3 its user-select.
EM_JS(void, fg_host_canvas_text, (int what, char* out, int size), {
    const canvas = globalThis.funkguiHostPage.canvas;
    const text = what === 0 ? canvas.style.cursor
               : what === 1 ? canvas.getAttribute("tabindex")
               : what === 2 ? canvas.style.touchAction
                            : canvas.style.userSelect;
    stringToUTF8(text === null ? "" : String(text), out, size);
});

EM_JS(int, fg_host_focused, (), { return document.activeElement === globalThis.funkguiHostPage.canvas ? 1 : 0; });
EM_JS(void, fg_host_blur, (), { globalThis.funkguiHostPage.canvas.blur(); });
EM_JS(int, fg_host_pointer_locked, (), { return document.pointerLockElement ? 1 : 0; });
EM_JS(int, fg_host_sink_mark, (), { return globalThis.funkguiHostPage.canvas["funkguiWebGlSink"] ? 1 : 0; });
EM_JS(double, fg_host_ratio, (), { return window.devicePixelRatio; });

// The platform as a page reads it, judged here in JavaScript and not by the code under test.
EM_JS(int, fg_host_apple, (), {
    const data = navigator.userAgentData;
    return /mac|iphone|ipad|ipod/i.test(String((data && data.platform) || navigator.platform)) ? 1 : 0;
});

// The window's inner size as the page reads it: (w, h) overrides it, (0, 0) gives the browser's back, and a negative
// pair makes it a window with no size (0 x 0: what a frame that is not displayed reads).
EM_JS(void, fg_host_inner, (int width, int height), {
    const page = globalThis.funkguiHostPage;
    if (!page.inner)
        page.inner = { w: Object.getOwnPropertyDescriptor(window, "innerWidth"),
                       h: Object.getOwnPropertyDescriptor(window, "innerHeight") };
    if (width !== 0 && height !== 0) {
        Object.defineProperty(window, "innerWidth", { value: Math.max(width, 0), configurable: true, writable: true });
        Object.defineProperty(window, "innerHeight", { value: Math.max(height, 0), configurable: true,
                                                       writable: true });
    } else {
        Object.defineProperty(window, "innerWidth", page.inner.w);
        Object.defineProperty(window, "innerHeight", page.inner.h);
    }
});

// document.hidden as the page reads it: overridden to true, or the browser's again.
EM_JS(void, fg_host_hidden, (int hidden), {
    if (hidden) Object.defineProperty(document, "hidden", { value: true, configurable: true });
    else delete document.hidden;
});

// WEBGL_lose_context on the canvas's context: 1 when the loss was forced, 0 when the extension is missing.
EM_JS(int, fg_host_lose, (), {
    const page = globalThis.funkguiHostPage;
    const gl = page.canvas.getContext("webgl2");
    page.ext = gl && gl.getExtension("WEBGL_lose_context");
    if (!page.ext) return 0;
    page.ext.loseContext();
    return 1;
});

EM_JS(void, fg_host_restore, (), { globalThis.funkguiHostPage.ext.restoreContext(); });

namespace
{
    constexpr const char* kCanvas = "#funkgui-canvas";
    constexpr float  kDt = 1.0f / 60.0f;
    constexpr double kFrameMs = 1000.0 / 60.0;
    constexpr int    kMaxSettle = 600;

    // A frame against SoftRaster: the sink page's bounds (test/web/page.cpp has the measurements they come from).
    constexpr int    kTolerance = 2;
    constexpr int    kWorst = 16;
    constexpr double kOverPerMille = 10.0;

    constexpr int kShift = 1, kCtrl = 2, kAlt = 4, kMeta = 8, kSecondPointer = 16, kPen = 32;   // fg_host_pointer's

    constexpr const char* kZoomKey = "fgWebHostZoom";
    const std::vector<int> kSteps{ 100, 125, 150, 175 };

    // ---- output -----------------------------------------------------------------------------------------------------
    std::string firstFailure;                    // "" while everything passed

    __attribute__((format(printf, 1, 2))) void say(const char* format, ...)
    {
        char text[1024];
        va_list args;
        va_start(args, format);
        std::vsnprintf(text, sizeof text, format, args);
        va_end(args);
        fg_host_line(text);
    }

    void fail(const std::string& why)
    {
        say("FAIL     %s", why.c_str());
        if (firstFailure.empty())
            firstFailure = why;
    }

    // A claim the run makes: printed as it stands when it holds, the failure when it does not.
    bool check(bool ok, const std::string& claim)
    {
        if (ok)
            say("ok       %s", claim.c_str());
        else
            fail("it is not true that " + claim);
        return ok;
    }

    void verdict()
    {
        // What the page's hooks kept failed the run as a claim does, whatever the claims after it said.
        char hook[512];
        fg_host_hook_failure(hook, static_cast<int>(sizeof hook));
        if (firstFailure.empty() && hook[0] != '\0')
            firstFailure = hook;
        if (firstFailure.empty())
        {
            say("VERDICT  PASS");
            fg_host_title("PASS");
        }
        else
        {
            say("VERDICT  FAIL: %s", firstFailure.c_str());
            fg_host_title(("FAIL: " + firstFailure).c_str());
        }
    }

    bool sameBits(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }

    std::string num(double v)
    {
        char text[48];
        std::snprintf(text, sizeof text, "%.9g", v);
        return text;
    }

    std::string canvasText(int what)
    {
        char text[64];
        fg_host_canvas_text(what, text, static_cast<int>(sizeof text));
        return text;
    }

    std::string cssSize() { return num(fg_host_canvas(0)) + " x " + num(fg_host_canvas(1)); }
    std::string boxSize() { return num(fg_host_canvas(2)) + " x " + num(fg_host_canvas(3)); }
    std::string bufferSize() { return num(fg_host_canvas(4)) + " x " + num(fg_host_canvas(5)); }

    bool canvasIs(double cssW, double cssH)
    {
        return fg_host_canvas(0) == cssW && fg_host_canvas(1) == cssH && fg_host_canvas(2) == cssW
            && fg_host_canvas(3) == cssH;
    }

    bool bufferIs(double w, double h) { return fg_host_canvas(4) == w && fg_host_canvas(5) == h; }

    // ---- what the hosts and the Panels say happened, in order -------------------------------------------------------
    std::vector<std::string> journal;

    std::string journalText(size_t from = 0)
    {
        std::string out;
        for (size_t i = from; i < journal.size(); ++i)
            out += (i > from ? " " : "") + journal[i];
        return out;
    }

    // ---- the services behind the seam: a recording double -----------------------------------------------------------
    struct Seam
    {
        int   constructed = 0, destroyed = 0, letGoCalls = 0, menus = 0, dismissals = 0, copies = 0;
        std::string selector;
        MenuRequest lastMenu;
        float lastLogicalWidth = 0.0f;
        std::string lastCopy;
        funkgui::MenuCallback pending;               // the callback showMenu was handed (when it took the menu)
        bool  answerMenu = true, answerCopy = true;  // what the services answer
        bool  inEvent = false;                       // the last call arrived inside a DOM event's dispatch
        int   sinkMarkAtDestroy = -1;                // the canvas still had its sink when the services went
        std::function<void()> onLetGo;               // run inside letGo()
    };
    Seam seam;
}

// funkgui::web::WebServices as src/web/WebServices.h declares it, recording (this page does not link
// src/web/WebServices.cpp).
namespace funkgui::web
{
    struct WebServices::Impl {};

    WebServices::WebServices(const char* canvasSelector) : impl_(std::make_unique<Impl>())
    {
        ++seam.constructed;
        seam.selector = canvasSelector != nullptr ? canvasSelector : "";
    }

    WebServices::~WebServices()
    {
        ++seam.destroyed;
        seam.sinkMarkAtDestroy = fg_host_sink_mark();
        journal.emplace_back("services-destroyed");
    }

    bool WebServices::showMenu(const MenuRequest& request, MenuCallback done, float logicalWidth)
    {
        ++seam.menus;
        seam.lastMenu = request;
        seam.lastLogicalWidth = logicalWidth;
        seam.inEvent = fg_host_dispatching() != 0;
        seam.pending = seam.answerMenu ? std::move(done) : MenuCallback{};
        journal.emplace_back("menu");
        return seam.answerMenu;
    }

    void WebServices::dismissMenus()
    {
        ++seam.dismissals;
        seam.pending = nullptr;
        journal.emplace_back("dismiss");
    }

    bool WebServices::copyText(std::string_view utf8)
    {
        ++seam.copies;
        seam.lastCopy.assign(utf8);
        seam.inEvent = fg_host_dispatching() != 0;
        journal.emplace_back("copy");
        return seam.answerCopy;
    }

    void WebServices::letGo()
    {
        ++seam.letGoCalls;
        seam.pending = nullptr;
        journal.emplace_back("let-go");
        if (seam.onLetGo)
            seam.onLetGo();
    }

    bool WebServices::menuOpen() const noexcept { return static_cast<bool>(seam.pending); }
}

namespace
{
    // ---- a Panel that records what reaches it -----------------------------------------------------------------------
    class Recorder final : public funkgui::Panel
    {
    public:
        Recorder(int w, int h) : w_(w), h_(h) {}

        funkgui::HostServices* host = nullptr;
        PointerEvent move, down, drag, up, dbl;
        WheelEvent   wheelEv;
        KeyEvent     keyEv;
        int   moves = 0, downs = 0, drags = 0, ups = 0, doubles = 0, exits = 0, wheels = 0, keys = 0;
        int   ticks = 0, idles = 0, closes = 0;
        int   brought = 0;                           // what a beforeTick hook brought in: it counts its calls here
        int   broughtSeen = 0;                       // `brought` as the last tick saw it
        int   fedTicks = 0;                          // the ticks that came after as many calls as ticks, theirs made
        float firstDt = 0.0f, lastDt = 0.0f, smallestDt = 1.0e9f, largestDt = 0.0f;
        float markedDt = -1.0f;                      // the dt of the first tick after markDt(); -1 until it comes
        double lastIdleNow = -1.0;
        bool  wantFull = false, takeWheel = true, takeKey = true;
        Cursor shown = Cursor::normal;
        std::function<void()> onDown, onKey;         // what a widget does inside the event

        int events() const { return moves + downs + drags + ups + doubles + exits + wheels + keys; }
        void markDt()
        {
            markedDt = -1.0f;
            marked_ = true;
        }

        void attach(funkgui::HostServices& h) override { host = &h; }
        int  width() const override { return w_; }
        int  height() const override { return h_; }
        void tick(float dt) override
        {
            if (ticks == 0)
                firstDt = dt;
            ++ticks;
            lastDt = dt;
            ++allTicks_;
            fedTicks += brought == allTicks_ ? 1 : 0;            // one hook call a tick, and this tick's was made
            broughtSeen = brought;
            if (marked_)
                markedDt = dt;
            marked_ = false;
            smallestDt = std::min(smallestDt, dt);
            largestDt = std::max(largestDt, dt);
        }
        void idle(double nowSec) override
        {
            ++idles;
            lastIdleNow = nowSec;
        }
        void draw(funkgui::Canvas& c, const Theme& th) override
        {
            const float w = static_cast<float>(w_), h = static_cast<float>(h_);
            c.rrect(8.0f, 8.0f, w - 16.0f, h - 16.0f, 6.0f, th.ink16, 1.0f, th.ink52);
            c.text("WEBHOST", 20.0f, 20.0f, funkgui::type::kLabel, th.ink100);
            c.rrect(20.0f, 60.0f, 100.0f, 40.0f, 4.0f, th.accentDim, 1.5f, th.accent);
            c.segment(140.0f, 60.0f, w - 20.0f, 100.0f, 2.0f, th.signal);
        }
        bool wantsFullRate() const override { return wantFull; }
        void pointerMove(const PointerEvent& e) override
        {
            move = e;
            ++moves;
            shown = e.x < static_cast<float>(w_) * 0.5f ? Cursor::leftRight : Cursor::pointingHand;
            journal.emplace_back("move");
        }
        void pointerExit() override
        {
            ++exits;
            shown = Cursor::normal;
            journal.emplace_back("exit");
        }
        void pointerDown(const PointerEvent& e) override
        {
            down = e;
            ++downs;
            shown = Cursor::crosshair;
            journal.emplace_back("down");
            if (onDown)
                onDown();
        }
        void pointerDrag(const PointerEvent& e) override
        {
            drag = e;
            ++drags;
            journal.emplace_back("drag");
        }
        void pointerUp(const PointerEvent& e) override
        {
            up = e;
            ++ups;
            shown = Cursor::upDown;
            journal.emplace_back("up");
        }
        void doubleClick(const PointerEvent& e) override
        {
            dbl = e;
            ++doubles;
            journal.emplace_back("double");
        }
        bool wheel(const WheelEvent& e) override
        {
            wheelEv = e;
            ++wheels;
            journal.emplace_back("wheel");
            return takeWheel;
        }
        bool key(const KeyEvent& e) override
        {
            keyEv = e;
            ++keys;
            journal.emplace_back("key");
            if (onKey)
                onKey();
            return takeKey;
        }
        Cursor cursor() const override { return shown; }
        void accessibility(std::vector<funkgui::A11yItem>&) const override {}
        uint32_t a11yRevision() const override { return 0; }
        void a11yAction(uint32_t, funkgui::A11yAction, double) override {}
        void closeGestures() override
        {
            ++closes;
            journal.emplace_back("close");
        }

    private:
        int  w_, h_;
        bool marked_ = false;
        int  allTicks_ = 0;                          // `ticks`, which a test may reset, without the resets
    };

    // shift | cmd << 1 | alt << 2 | ctrl << 3
    int bits(funkgui::Mods m) { return (m.shift ? 1 : 0) | (m.cmd ? 2 : 0) | (m.alt ? 4 : 0) | (m.ctrl ? 8 : 0); }
    constexpr int kModShift = 1, kModCmd = 2, kModAlt = 4, kModCtrl = 8;

    bool at(const PointerEvent& e, float x, float y) { return sameBits(e.x, x) && sameBits(e.y, y); }

    std::string where(const PointerEvent& e)
    {
        return "(" + num(e.x) + ", " + num(e.y) + "), mods " + std::to_string(bits(e.mods)) + ", clicks "
             + std::to_string(e.clicks) + (e.popup ? ", popup" : "");
    }

    // ---- preferences the page can look into ------------------------------------------------------------------------
    struct Store
    {
        std::map<std::string, std::string> values;
        int reloads = 0;
    };
    Store store;

    class StoreBackend final : public UiPreferences::Backend
    {
    public:
        bool read(const char* key, std::string& value) const override
        {
            const auto it = store.values.find(key);
            if (it == store.values.end())
                return false;
            value = it->second;
            return true;
        }
        void write(const char* key, const std::string& value) override { store.values[key] = value; }
        bool reload() override
        {
            ++store.reloads;
            return false;
        }
    };

    // ---- the run's state --------------------------------------------------------------------------------------------
    bool   apple = false;                        // the browser's platform, as JavaScript judged it
    double stamp = 1000.0;                       // the timestamps frame() is driven with

    // The pins of a capture: every frame the same wherever and whenever the page runs.
    WebHostConfig pinned(int zoomPercent)
    {
        WebHostConfig c;
        c.canvasSelector = kCanvas;
        c.capture.fixedDt = kDt;
        c.capture.uiScale = 2.0f;
        c.capture.uiTheme = 0;
        c.capture.uiZoom = zoomPercent;
        return c;
    }

    WebHost::FrameResult frame(WebHost& host)
    {
        stamp += kFrameMs;
        return host.frame(stamp);
    }

    // ---- 1. parity --------------------------------------------------------------------------------------------------
    struct Case
    {
        const char* key;
        const char* section;
        const char* keys;                        // HeadlessHost::keys' spec
        std::vector<const char*> domKeys;        // the same keys as KeyboardEvent.key
    };

    bool sameFingerprint(const funkgui::Fingerprint& a, const funkgui::Fingerprint& b)
    {
        return a.geometry == b.geometry && a.text == b.text && a.statics == b.statics && a.live == b.live
            && a.texts == b.texts && a.rrects == b.rrects && a.segments == b.segments && a.areas == b.areas
            && sameBits(a.maxX, b.maxX) && sameBits(a.maxY, b.maxY) && a.tagCounts == b.tagCounts;
    }

    bool isLive(const funkgui::Prim& p)
    {
        return (static_cast<uint32_t>(p.d2[3] + 0.5f) & funkgui::pflag::live) != 0u;
    }

    // The non-live primitives of both lists, in order, byte for byte; the index of the first difference.
    bool staticBitEqual(const PrimList& a, const PrimList& b, long& firstDiff)
    {
        std::vector<const funkgui::Prim*> sa, sb;
        for (const auto& p : a.prims)
            if (!isLive(p))
                sa.push_back(&p);
        for (const auto& p : b.prims)
            if (!isLive(p))
                sb.push_back(&p);
        firstDiff = -1;
        const size_t n = std::min(sa.size(), sb.size());
        for (size_t i = 0; i < n; ++i)
            if (std::memcmp(sa[i], sb[i], sizeof(funkgui::Prim)) != 0)
            {
                firstDiff = static_cast<long>(i);
                return false;
            }
        if (sa.size() != sb.size())
        {
            firstDiff = static_cast<long>(n);
            return false;
        }
        return true;
    }

    struct Diff
    {
        bool   sameSize = false;
        int    worst = 0;
        size_t samples = 0, over = 0;
    };

    Diff compare(const Image& gl, const Image& soft)
    {
        Diff d;
        d.sameSize = gl.w == soft.w && gl.h == soft.h && gl.w > 0 && gl.rgba.size() == soft.rgba.size();
        if (!d.sameSize)
            return d;
        d.samples = gl.rgba.size();
        for (size_t i = 0; i < gl.rgba.size(); ++i)
        {
            const int delta = std::abs(static_cast<int>(gl.rgba[i]) - static_cast<int>(soft.rgba[i]));
            d.over += delta > kTolerance ? 1u : 0u;
            d.worst = std::max(d.worst, delta);
        }
        return d;
    }

    bool within(const Diff& d)
    {
        return d.sameSize && d.worst <= kWorst
            && static_cast<double>(d.over) * 1000.0 <= kOverPerMille * static_cast<double>(d.samples);
    }

    size_t drawnPixels(const Image& image, funkgui::Col clear)
    {
        size_t n = 0;
        for (size_t i = 0; i + 3 < image.rgba.size(); i += 4)
            if (image.rgba[i] != clear.r || image.rgba[i + 1] != clear.g || image.rgba[i + 2] != clear.b)
                ++n;
        return n;
    }

    // The frame the host last drew, read back in this task, against SoftRaster's image of the list it recorded.
    void pixelsMatch(const std::string& label, WebHost& host)
    {
        const PrimList& list = host.lastFrame();
        const Image soft = funkgui::rasterise(list, funkgui::FontService::get().atlas(), 1);
        const Image gl = host.sink().readPixels();
        const Diff d = compare(gl, soft);
        const size_t drawn = drawnPixels(gl, list.info.clear);
        check(within(d) && drawn > 0,
              label + ": WebGL through the host is within the sink page's bounds of SoftRaster (" + std::to_string(gl.w)
                  + " x " + std::to_string(gl.h) + " px against " + std::to_string(soft.w) + " x "
                  + std::to_string(soft.h) + ", " + std::to_string(drawn) + " px drawn, largest channel difference "
                  + std::to_string(d.worst) + ", " + std::to_string(d.over) + " of " + std::to_string(d.samples)
                  + " samples over " + std::to_string(kTolerance) + ")");
    }

    void parity()
    {
        // GalleryLive's cases, then every other section at rest.
        std::vector<Case> cases{
            { "primitives", "primitives", "", {} },
            { "area", "area", "", {} },
            { "ruleslider", "ruleslider", "", {} },
            { "ruleslider_keys", "ruleslider", "tab,tab,right", { "Tab", "Tab", "ArrowRight" } },
        };
        for (const G::SectionInfo& s : G::sections())
        {
            const bool listed = std::any_of(cases.begin(), cases.end(),
                                            [&s](const Case& c) { return s.name == c.section; });
            if (!listed && s.name != "services")     // its frame shows the file chooser a web host does not report
                cases.push_back({ s.name.c_str(), s.name.c_str(), "", {} });
        }
        for (const Case& c : cases)
        {
            const std::string key = c.key;
            const G::SectionInfo* info = G::findSection(c.section);
            if (info == nullptr)
            {
                fail(key + ": no gallery section '" + c.section + "'");
                continue;
            }

            // The reference, as GalleryLive's: settle, the keys, one tick, settle, draw.
            PrimList reference;
            int headlessFrames = 0;
            {
                G::GalleryPanel panel(*info);
                funkgui::HeadlessHost host(panel, 0, 2.0f);
                const int before = host.settle(kMaxSettle, kDt);
                if (c.keys[0] != '\0')
                    host.keys(c.keys);
                host.tick(1, kDt);
                const int after = host.settle(kMaxSettle, kDt);
                if (before > kMaxSettle || after > kMaxSettle)
                {
                    fail(key + ": the headless section did not settle");
                    continue;
                }
                headlessFrames = before + 1 + after;
                reference = host.draw();
            }

            // The same script through WebHost: frames until the Panel settles, the keys as DOM events, a frame,
            // frames until it settles again.
            G::GalleryPanel panel(*info);
            WebHost host(panel, pinned(100));
            if (!check(host.ok(), key + ": a WebHost on the page's canvas is ok() (" + host.error() + ")"))
                continue;
            int frames = 0;
            const auto settle = [&] {
                int n = 0;
                do
                {
                    frame(host);
                    ++n;
                } while (panel.wantsFullRate() && n <= kMaxSettle);
                frames += n;
                return n <= kMaxSettle;
            };
            bool settled = settle();
            for (const char* domKey : c.domKeys)
                fg_host_key(domKey, 0);
            settled = settle() && settled;
            if (!settled)
            {
                fail(key + ": the section did not settle under WebHost");
                continue;
            }
            const PrimList& live = host.lastFrame();
            const funkgui::Fingerprint fh = funkgui::fingerprint(reference), fl = funkgui::fingerprint(live);
            say("         %-16s headless %d frames, %016llx/%016llx, %d statics; WebHost %d frames, %016llx/%016llx, "
                "%d statics, %d live",
                c.key, headlessFrames, static_cast<unsigned long long>(fh.geometry),
                static_cast<unsigned long long>(fh.text), fh.statics, frames,
                static_cast<unsigned long long>(fl.geometry), static_cast<unsigned long long>(fl.text), fl.statics,
                fl.live);
            long firstDiff = -1;
            const bool statics = staticBitEqual(reference, live, firstDiff);
            check(sameFingerprint(fh, fl) && statics && fl.statics > 0,
                  key + ": the frame WebHost recorded has HeadlessHost's fingerprint, and its "
                      + std::to_string(fl.statics) + " static primitives are equal bit for bit (first difference: "
                      + std::to_string(firstDiff) + ")");
            check(sameBits(live.info.dpi, 2.0f) && live.info.fixedClock && sameBits(live.info.dt, kDt)
                      && live.info.logicalW == reference.info.logicalW && live.info.logicalH == reference.info.logicalH
                      && live.info.theme == 0 && live.missingGlyphs == 0 && sameBits(live.info.fps, 0.0f),
                  key + ": its frame info is the pins' (dpi exactly 2: " + num(live.info.dpi)
                      + "; clock fixed, dt 1/60, " + std::to_string(live.info.logicalW) + " x "
                      + std::to_string(live.info.logicalH)
                      + ", theme 0, no glyph missing)");
            check(bufferIs(live.info.logicalW * 2.0, live.info.logicalH * 2.0)
                      && canvasIs(live.info.logicalW, live.info.logicalH),
                  key + ": the canvas is " + cssSize() + " CSS px with a " + bufferSize() + " buffer");
            pixelsMatch(key, host);
        }
    }

    // ---- 2 .. 5: a recording Panel at zoom 150 ----------------------------------------------------------------------

    // Events at a logical position: the canvas is 1.5 CSS px per logical px.
    int pointerAt(const char* type, float x, float y, int button, int buttons, int mods)
    {
        return fg_host_pointer(type, static_cast<double>(x) * 1.5, static_cast<double>(y) * 1.5, button, buttons, mods);
    }

    void sizingAndInput()
    {
        journal.clear();
        seam = Seam{};
        Recorder panel(240, 160);
        int batchDepth = 0, batches = 0;
        WebHostConfig config = pinned(150);
        config.zoomSteps = kSteps;
        config.setUiAttached = [](bool on) { journal.emplace_back(on ? "attached" : "detached"); };
        config.beginBatch = [&] { ++batches; ++batchDepth; };
        config.endBatch = [&] { --batchDepth; };
        auto host = std::make_unique<WebHost>(panel, std::move(config));
        if (!check(host->ok() && panel.host != nullptr && journalText() == "attached" && !host->running(),
                   "a WebHost over a 240 x 160 Panel pinned to zoom 150 is ok(), attached its Panel, reported "
                   "setUiAttached(true) and has not started its clock"))
            return;
        funkgui::HostServices& hs = *panel.host;

        // ---- sizing
        check(canvasIs(360.0, 240.0),
              "the canvas is 360 x 240 CSS px at once (style " + cssSize() + ", box " + boxSize() + ")");
        check(panel.ticks == 0, "constructing the host ticks nothing");
        const WebHost::FrameResult first = frame(*host);
        const WebHost::Diagnostics d = host->diagnostics();
        check(first.submitted && !first.wantsFullRate && panel.ticks == 1 && sameBits(panel.lastDt, kDt)
                  && bufferIs(720.0, 480.0) && d.frames == 1 && d.physW == 720 && d.physH == 480 && d.scale == 2.0
                  && d.zoomPercent == 150 && d.lost == 0,
              "frame() ticks the Panel once with the pinned dt and submits a 720 x 480 buffer (" + bufferSize()
                  + "; zoom " + std::to_string(d.zoomPercent) + ", scale " + num(d.scale) + ")");
        check(sameBits(host->lastFrame().info.dpi, 3.0f) && host->lastFrame().info.logicalW == 240
                  && host->lastFrame().info.logicalH == 160 && host->lastFrame().info.frame == 1
                  && !host->lastFrame().info.displayLinked,
              "the frame's dpi is physical height / logical height = 480 / 160 = 3 exactly ("
                  + num(host->lastFrame().info.dpi) + "), its size the Panel's");
        pixelsMatch("zoom 150, scale 2", *host);
        check(canvasText(1) == "0" && canvasText(2) == "none" && canvasText(3) == "none",
              "the canvas has tabindex 0, touch-action none and user-select none");

        // ---- pointer: move, press, drag (also outside the canvas), release
        journal.clear();
        pointerAt("pointermove", 37.5f, 12.25f, 0, 0, 0);
        check(panel.moves == 1 && at(panel.move, 37.5f, 12.25f) && bits(panel.move.mods) == 0 && panel.move.clicks == 1
                  && !panel.move.popup,
              "a pointermove at client (canvas + 56.25, + 18.375) is pointerMove at logical (37.5, 12.25) exactly: "
                  + where(panel.move));
        check(canvasText(0) == "ew-resize", "the canvas's CSS cursor is the Panel's leftRight: " + canvasText(0));
        pointerAt("pointermove", 200.0f, 80.5f, 0, 0, 0);
        check(at(panel.move, 200.0f, 80.5f) && canvasText(0) == "pointer",
              "and follows it to pointingHand: " + canvasText(0));

        fg_host_blur();
        int captures = fg_host_captures();
        pointerAt("pointerdown", 37.5f, 12.25f, 0, 1, kShift);
        check(panel.downs == 1 && at(panel.down, 37.5f, 12.25f) && bits(panel.down.mods) == kModShift
                  && panel.down.clicks == 1 && !panel.down.popup,
              "a pointerdown with Shift is pointerDown there with shift alone, one click: " + where(panel.down));
        check(fg_host_focused() == 1 && canvasText(0) == "crosshair",
              "the press focused the canvas; the cursor is crosshair");
        check(fg_host_captures() == captures + 1 && fg_host_captured() == 1,
              "and asked the canvas to capture the event's pointer, once: setPointerCapture("
                  + std::to_string(fg_host_captured()) + "), the synthetic mouse's id");
        pointerAt("pointermove", 239.0f, 0.5f, 0, 1, kShift);
        check(panel.drags == 1 && panel.moves == 2 && at(panel.drag, 239.0f, 0.5f)
                  && bits(panel.drag.mods) == kModShift,
              "a pointermove during the press is pointerDrag: " + where(panel.drag));
        pointerAt("pointermove", -10.0f, 170.0f, 0, 1, 0);
        check(panel.drags == 2 && at(panel.drag, -10.0f, 170.0f),
              "a drag outside the canvas keeps its logical position: " + where(panel.drag));
        pointerAt("pointerup", 239.0f, 0.5f, 0, 0, 0);
        check(panel.ups == 1 && at(panel.up, 239.0f, 0.5f) && panel.up.clicks == 1 && panel.doubles == 0
                  && journalText() == "move move down drag drag up" && canvasText(0) == "ns-resize",
              "the release is pointerUp and no double click: " + journalText());

        // ---- click counts, in JUCE's order
        journal.clear();
        pointerAt("pointerdown", 60.0f, 40.0f, 0, 1, 0);
        const int firstClicks = panel.down.clicks;
        pointerAt("pointerup", 60.0f, 40.0f, 0, 0, 0);
        pointerAt("pointerdown", 60.0f, 40.0f, 0, 1, 0);
        const int secondClicks = panel.down.clicks;
        pointerAt("pointerup", 60.0f, 40.0f, 0, 0, 0);
        check(firstClicks == 1 && secondClicks == 2 && panel.up.clicks == 2 && panel.dbl.clicks == 2
                  && at(panel.dbl, 60.0f, 40.0f) && journalText() == "down up down up double",
              "two quick clicks: the second press counts 2, and its release is pointerUp, then doubleClick: "
                  + journalText());
        journal.clear();
        pointerAt("pointerdown", 60.0f, 40.0f, 0, 1, 0);
        pointerAt("pointerup", 60.0f, 40.0f, 0, 0, 0);
        check(panel.down.clicks == 3 && panel.up.clicks == 3 && panel.dbl.clicks == 3
                  && journalText() == "down up double",
              "a third counts 3: " + journalText());
        journal.clear();
        pointerAt("pointerdown", 60.0f, 40.0f, 0, 1, 0);
        pointerAt("pointermove", 66.0f, 40.0f, 0, 1, 0);          // 9 CSS px: a drag
        pointerAt("pointerup", 66.0f, 40.0f, 0, 0, 0);
        check(panel.down.clicks == 4 && panel.drag.clicks == 1 && panel.up.clicks == 1
                  && journalText() == "down drag up",
              "a fourth press counts 4; once it moves it is a drag, counts 1 and ends without a double click: "
                  + journalText());

        // ---- the popup rule and the modifiers on this platform
        journal.clear();
        pointerAt("pointerdown", 100.0f, 100.0f, 2, 2, 0);
        const bool rightDown = panel.down.popup && panel.down.clicks == 1;
        pointerAt("pointerup", 100.0f, 100.0f, 2, 0, 0);
        check(rightDown && panel.up.popup && journalText() == "down up",
              "the right button is a popup press and a popup release (another button: one click)");
        pointerAt("pointerdown", 140.0f, 100.0f, 0, 1, kCtrl);
        check(panel.down.popup == apple && bits(panel.down.mods) == (apple ? kModCtrl : (kModCmd | kModCtrl)),
              std::string("Ctrl-click on this platform (") + (apple ? "Apple" : "not Apple") + ") is "
                  + (apple ? "a popup click with ctrl alone" : "no popup, with cmd and ctrl") + ": "
                  + where(panel.down));
        pointerAt("pointerup", 140.0f, 100.0f, 0, 0, kCtrl);
        pointerAt("pointermove", 150.0f, 100.0f, 0, 0, kMeta | kAlt);
        check(bits(panel.move.mods) == (apple ? (kModCmd | kModAlt) : kModAlt),
              std::string("Meta is ") + (apple ? "cmd" : "nothing") + " here, Alt is alt: " + where(panel.move));

        // ---- another pointer's id is the one captured (the browser has no such pointer and throws: nothing stops)
        journal.clear();
        captures = fg_host_captures();
        pointerAt("pointerdown", 200.0f, 140.0f, 0, 1, kPen);
        const int penCaptured = fg_host_captured();
        pointerAt("pointerup", 200.0f, 140.0f, 0, 0, kPen);
        check(fg_host_captures() == captures + 1 && penCaptured == 7 && journalText() == "down up",
              "a press of another primary pointer (a pen, id 7) is taken and captured by its own id: "
              "setPointerCapture(" + std::to_string(penCaptured) + ")");

        // ---- what the host ignores
        int before = panel.events();
        captures = fg_host_captures();
        pointerAt("pointerdown", 100.0f, 100.0f, 1, 4, 0);
        pointerAt("pointerup", 100.0f, 100.0f, 1, 0, 0);
        check(panel.events() == before && fg_host_captures() == captures,
              "the middle button's press and release reach no Panel, and capture nothing");
        pointerAt("pointerdown", 100.0f, 100.0f, 0, 1, kSecondPointer);
        pointerAt("pointermove", 100.0f, 100.0f, 0, 1, kSecondPointer);
        pointerAt("pointerup", 100.0f, 100.0f, 0, 0, kSecondPointer);
        check(panel.events() == before && fg_host_captures() == captures, "nor does a second pointer (not primary)");
        pointerAt("pointerup", 100.0f, 100.0f, 0, 0, 0);
        check(panel.events() == before, "nor a release without a press");
        check(fg_host_mouse("contextmenu") == 1 && panel.events() == before,
              "contextmenu's default is prevented, and it is no event of the Panel's");

        // ---- leaving, and a press that cannot be captured
        journal.clear();
        pointerAt("pointerleave", 250.0f, 100.0f, 0, 0, 0);
        check(journalText() == "exit" && canvasText(0) == "default",
              "pointerleave is pointerExit, and the cursor is the default again");
        journal.clear();
        pointerAt("pointerdown", 20.0f, 20.0f, 0, 1, 0);
        pointerAt("pointermove", 30.0f, 30.0f, 0, 1, 0);
        const int upsBefore = panel.ups;
        pointerAt("pointerleave", 250.0f, 30.0f, 0, 1, 0);
        check(journalText() == "down drag up exit" && panel.ups == upsBefore + 1 && at(panel.up, 30.0f, 30.0f),
              "a press whose pointer leaves the canvas uncaptured ends where it last was, then exits: " + journalText()
                  + " at " + where(panel.up));
        before = panel.events();
        pointerAt("pointerup", 250.0f, 30.0f, 0, 0, 0);
        check(panel.events() == before, "and its late release is ignored");
        journal.clear();
        pointerAt("pointerdown", 20.0f, 20.0f, 0, 1, 0);
        pointerAt("pointercancel", 20.0f, 20.0f, 0, 0, 0);
        check(journalText() == "down up exit", "pointercancel ends a press the same way: " + journalText());

        // ---- a press whose release never reaches the page (each press away from the one before: one click)
        journal.clear();
        int doubles = panel.doubles;
        pointerAt("pointerdown", 60.0f, 60.0f, 0, 1, 0);
        pointerAt("pointermove", 70.0f, 60.0f, 0, 1, 0);
        pointerAt("pointermove", 84.0f, 60.0f, 0, 0, 0);          // no button down: it came up somewhere else
        check(journalText() == "down drag up move" && at(panel.up, 70.0f, 60.0f) && at(panel.move, 84.0f, 60.0f)
                  && panel.doubles == doubles && canvasText(0) == "ew-resize",
              "a press whose pointer moves with no button down ends where it last was, with no double click, and "
              "the move is a pointerMove: " + journalText() + ", up at " + where(panel.up));
        journal.clear();
        fg_host_blur();
        captures = fg_host_captures();
        pointerAt("pointerdown", 100.0f, 60.0f, 0, 1, 0);
        const bool takenAgain = journalText() == "down" && fg_host_focused() == 1 && fg_host_captures() == captures + 1;
        pointerAt("lostpointercapture", 100.0f, 60.0f, 0, 0, 0);
        const std::string atLoss = journalText();
        pointerAt("pointermove", 110.0f, 60.0f, 0, 0, 0);
        check(takenAgain && atLoss == "down up" && at(panel.up, 100.0f, 60.0f) && journalText() == "down up move",
              "the press after it is taken, focused and captured; the capture lost with the press open ends it the "
              "same way, with no exit, and the hover after it is a pointerMove: " + journalText());
        before = panel.events();
        pointerAt("pointerup", 110.0f, 60.0f, 0, 0, 0);
        pointerAt("pointerdown", 140.0f, 60.0f, 0, 1, 0);
        pointerAt("pointerup", 140.0f, 60.0f, 0, 0, 0);
        pointerAt("lostpointercapture", 140.0f, 60.0f, 0, 0, 0);  // as a browser sends it after every release
        check(panel.events() == before + 2 && panel.doubles == doubles,
              "the late release of such a press is ignored, and so is the capture lost after a release");
        journal.clear();
        pointerAt("pointerdown", 180.0f, 60.0f, 0, 1, 0);
        fg_host_blur();
        captures = fg_host_captures();
        before = panel.events();
        pointerAt("pointerdown", 20.0f, 100.0f, 0, 1, kPen);
        const bool otherIgnored = panel.events() == before && fg_host_captures() == captures && fg_host_focused() == 0;
        pointerAt("pointerdown", 220.0f, 60.0f, 0, 1, 0);         // its own release was lost, and nothing said so
        check(otherIgnored && journalText() == "down up down" && at(panel.up, 180.0f, 60.0f)
                  && at(panel.down, 220.0f, 60.0f) && panel.down.clicks == 1 && fg_host_focused() == 1
                  && fg_host_captures() == captures + 1 && fg_host_captured() == 1,
              "during a press another pointer's press is ignored, and the same pointer's ends the press it had "
              "(where it last was) and is taken: " + journalText());
        const int upsOpen = panel.ups;
        pointerAt("pointermove", 20.0f, 100.0f, 0, 0, kPen);      // a pen hovering: not the pointer that pressed
        const bool stillOpen = panel.ups == upsOpen;
        pointerAt("pointerup", 220.0f, 60.0f, 0, 0, 0);
        check(stillOpen && panel.ups == upsOpen + 1 && at(panel.up, 220.0f, 60.0f),
              "and another pointer's move with no button down does not end it: its own release does");
        journal.clear();
        pointerAt("pointerdown", 20.0f, 140.0f, 0, 1, 0);
        fg_host_hidden(1);
        fg_host_event(1, "visibilitychange");
        const std::string atHide = journalText();
        fg_host_hidden(0);
        fg_host_event(1, "visibilitychange");
        pointerAt("pointermove", 30.0f, 140.0f, 0, 0, 0);
        before = panel.events();
        pointerAt("pointerup", 30.0f, 140.0f, 0, 0, 0);
        check(atHide == "down up close" && at(panel.up, 20.0f, 140.0f) && journalText() == "down up close move"
                  && panel.events() == before && panel.doubles == doubles,
              "a press open when the document is hidden ends there, before the gestures are closed; the hover on "
              "return is a pointerMove and the late release is ignored: " + journalText());

        // ---- the wheel
        panel.takeWheel = true;
        int prevented = fg_host_wheel(60.0, 36.0, 0.0, 512.0, 0, kShift);
        check(panel.wheels == 1 && sameBits(panel.wheelEv.x, 40.0f) && sameBits(panel.wheelEv.y, 24.0f)
                  && sameBits(panel.wheelEv.dy, -1.0f) && sameBits(panel.wheelEv.dx, 0.0f) && panel.wheelEv.smooth
                  && !panel.wheelEv.reversed && !panel.wheelEv.inertial && bits(panel.wheelEv.mods) == kModShift
                  && prevented == 1,
              "a 512 px wheel towards the user at client (canvas + 60, + 36) is wheel at (40, 24) with dy -1, smooth, "
              "and its default is prevented because the Panel used it (dy " + num(panel.wheelEv.dy) + ")");
        panel.takeWheel = false;
        prevented = fg_host_wheel(60.0, 36.0, 0.0, -120.0, 0, 0);
        check(panel.wheels == 2 && sameBits(panel.wheelEv.dy, 120.0f * 0.5f / 256.0f) && prevented == 0,
              "a wheel the Panel does not use is left to the page (dy " + num(panel.wheelEv.dy) + ", away: positive)");
        fg_host_wheel(60.0, 36.0, 0.0, 3.0, 1, 0);
        check(panel.wheels == 3 && sameBits(panel.wheelEv.dy, -50.0f / 256.0f) && !panel.wheelEv.smooth,
              "a line-mode wheel is one notch, not smooth (dy " + num(panel.wheelEv.dy) + ")");

        // ---- keys
        panel.takeKey = true;
        journal.clear();
        prevented = fg_host_key("ArrowRight", kShift);
        check(panel.keys == 1 && panel.keyEv.key == Key::right && bits(panel.keyEv.mods) == kModShift && prevented == 1,
              "keydown ArrowRight with Shift is Key::right with shift, prevented because the Panel used it");
        fg_host_key(" ", 0);
        check(panel.keys == 2 && panel.keyEv.key == Key::space && panel.keyEv.ch == U' ',
              "the space bar is Key::space with its character");
        const int command = apple ? kMeta : kCtrl;
        fg_host_key("Z", command | kShift);
        check(panel.keys == 3 && panel.keyEv.key == Key::character && panel.keyEv.ch == U'z' && panel.keyEv.mods.cmd
                  && panel.keyEv.mods.shift && panel.keyEv.mods.ctrl == !apple,
              std::string("the redo chord (") + (apple ? "Meta" : "Ctrl")
                  + "+Shift+Z) is cmd+shift with a lower-case z");
        fg_host_key("\xC3\xA9", 0);
        check(panel.keys == 4 && panel.keyEv.ch == 0xe9u, "text past ASCII arrives as its code point (U+00E9)");
        before = panel.keys;
        const int modifier = fg_host_key("Shift", kShift), dead = fg_host_key("Dead", 0), fn = fg_host_key("F5", 0);
        check(panel.keys == before && modifier == 0 && dead == 0 && fn == 0,
              "Shift, a dead key and F5 reach no Panel and stay the browser's");
        panel.takeKey = false;
        prevented = fg_host_key("a", 0);
        const int tab = fg_host_key("Tab", 0);
        check(panel.keys == before + 2 && prevented == 0 && tab == 0 && panel.keyEv.key == Key::tab,
              "a key the Panel does not use is delivered and left to the browser (Tab moves the focus on)");
        panel.takeKey = true;

        // ---- HostServices
        check(hs.services() == (funkgui::hostservice::menus | funkgui::hostservice::clipboard),
              "services() is menus | clipboard, no file chooser (" + std::to_string(hs.services()) + ")");
        {
            auto token = std::make_shared<int>(0);
            bool ran = false;
            const bool taken = hs.chooseFiles(funkgui::FileRequest{}, [token, &ran](const std::vector<std::string>&) {
                ran = true;
            });
            check(!taken && !ran && token.use_count() == 1, "chooseFiles refuses and drops its callback unrun");
        }
        check(hs.commandKeyIsMeta() == apple,
              std::string("commandKeyIsMeta() is the browser's platform (") + (apple ? "Apple: Meta" : "Ctrl") + ")");
        check(hs.zoomPercent() == 150 && std::vector<int>(hs.zoomSteps().begin(), hs.zoomSteps().end()) == kSteps
                  && hs.zoomFits(175) && !hs.zoomFits(110),
              "zoomPercent() is the pin, zoomSteps() the config's, zoomFits() true for every step under a pin");
        hs.setZoomPercent(125);
        frame(*host);
        check(hs.zoomPercent() == 150 && canvasIs(360.0, 240.0), "a zoom chosen under the pin leaves the canvas alone");
        UiPreferences::get().setTheme(1);
        frame(*host);
        check(hs.themeIndex() == 0 && host->lastFrame().info.theme == 0,
              "themeIndex() and the frame keep the pinned theme when the preference changes");
        UiPreferences::get().setTheme(0);
        check(std::fabs(hs.nowSeconds() - 3.0 * static_cast<double>(kDt)) < 1.0e-12,
              "nowSeconds() is the Panel's simulated clock under a pinned dt (" + num(hs.nowSeconds()) + " s after 3 "
              "frames)");
        hs.beginBatch();
        const int depthInside = batchDepth;
        hs.endBatch();
        check(batches == 1 && depthInside == 1 && batchDepth == 0, "beginBatch and endBatch reach the config's");
        hs.setUnboundedDrag(true);
        hs.setUnboundedDrag(false);
        check(fg_host_pointer_locked() == 0, "setUnboundedDrag asks for no pointer lock");

        // ---- the services behind the seam
        check(seam.constructed == 1 && seam.selector == kCanvas && seam.letGoCalls == 0,
              "the host constructed one WebServices on its canvas's selector");
        MenuRequest menu;
        menu.items.push_back({ 1, "One" });
        menu.items.push_back({ 2, "Two \xE2\x80\x94 ticked", true, true });
        menu.items.push_back({ .separator = true });
        menu.items.push_back({ 3, "Three", false });
        menu.anchor = funkgui::Rect{ 20.5f, 30.0f, 64.0f, 16.0f };
        menu.theme = Theme::byIndex(1);
        int chosen = -1;
        bool taken = hs.showMenu(menu, [&chosen](int id) { chosen = id; });
        const MenuRequest& got = seam.lastMenu;
        check(taken && seam.menus == 1 && sameBits(seam.lastLogicalWidth, 240.0f),
              "showMenu reaches the services with the Panel's logical width, 240, at zoom 150 ("
                  + num(seam.lastLogicalWidth) + "), and their true comes back");
        check(got.items.size() == 4 && got.items[0].id == 1 && got.items[0].label == "One" && got.items[1].checked
                  && got.items[1].label == menu.items[1].label && got.items[2].separator && !got.items[3].enabled
                  && sameBits(got.anchor.x, 20.5f) && sameBits(got.anchor.y, 30.0f) && sameBits(got.anchor.w, 64.0f)
                  && sameBits(got.anchor.h, 16.0f) && got.theme.ground.r == Theme::byIndex(1).ground.r
                  && got.theme.ink100.r == Theme::byIndex(1).ink100.r,
              "with the request as the Panel made it: the items, the anchor in the Panel's px, the theme");
        check(chosen == -1 && static_cast<bool>(seam.pending), "and with the Panel's callback, not yet run");
        seam.pending(2);
        check(chosen == 2, "which is the one the services call: the Panel hears the chosen id");
        seam.answerMenu = false;
        auto token = std::make_shared<int>(0);
        taken = hs.showMenu(menu, [token](int) {});
        check(!taken && seam.menus == 2 && token.use_count() == 1,
              "a menu the services refuse is refused to the Panel, its callback dropped");
        seam.answerMenu = true;
        hs.dismissMenus();
        check(seam.dismissals == 1, "dismissMenus reaches the services");
        const char* clip = "FunkGui \xC2\xB5 \xE2\x80\x94 copy";
        bool copied = hs.copyText(clip);
        check(copied && seam.copies == 1 && seam.lastCopy == clip,
              "copyText reaches them with the text, and their true comes back");
        seam.answerCopy = false;
        copied = hs.copyText("again");
        check(!copied && seam.copies == 2 && seam.lastCopy == "again", "and so does their false");
        seam.answerCopy = true;
        seam.inEvent = false;
        panel.onDown = [&] { copied = hs.copyText("inside the press"); };
        pointerAt("pointerdown", 200.0f, 20.0f, 0, 1, 0);
        const bool insidePress = seam.inEvent && seam.lastCopy == "inside the press";
        pointerAt("pointerup", 200.0f, 20.0f, 0, 0, 0);
        panel.onDown = nullptr;
        seam.inEvent = false;
        panel.onKey = [&] { hs.showMenu(menu, [](int) {}); };
        fg_host_key("Enter", 0);
        panel.onKey = nullptr;
        check(insidePress && seam.inEvent && seam.menus == 3,
              "a Panel that copies in a press, or opens a menu on a key, reaches the services inside the DOM event's "
              "handler (the browser's gesture rule)");

        // ---- teardown, in the middle of a press. First the two listeners that are not on the canvas, while the host
        // lives: each event runs one, this host's (the hosts before it left none behind).
        int resizes = fg_host_heard(0), visibilities = fg_host_heard(1);
        fg_host_event(0, "resize");
        fg_host_event(1, "visibilitychange");
        check(fg_host_heard(0) == resizes + 1 && fg_host_heard(1) == visibilities + 1,
              "a resize of the window and a visibilitychange of the document each run one listener, this host's ("
                  + std::to_string(fg_host_heard(0) - resizes) + ", " + std::to_string(fg_host_heard(1) - visibilities)
                  + ")");
        pointerAt("pointerdown", 20.0f, 20.0f, 0, 1, 0);
        journal.clear();
        int eventsAtLetGo = -1;
        before = panel.events();
        seam.onLetGo = [&] {
            pointerAt("pointermove", 30.0f, 30.0f, 0, 1, 0);      // the listeners are gone already
            eventsAtLetGo = panel.events();
        };
        host.reset();
        check(journalText() == "let-go close detached services-destroyed",
              "~WebHost lets the services go, closes the Panel's gestures, reports setUiAttached(false) and destroys "
              "the services: " + journalText());
        check(eventsAtLetGo == before, "the listeners were removed before the services were let go");
        check(seam.sinkMarkAtDestroy == 1 && fg_host_sink_mark() == 0,
              "the sink was destroyed last: the canvas still carried it when the services went, and is free of it now");
        const int closes = panel.closes, reloads = store.reloads;
        resizes = fg_host_heard(0);
        visibilities = fg_host_heard(1);
        pointerAt("pointermove", 30.0f, 30.0f, 0, 1, 0);
        fg_host_key("a", 0);
        fg_host_wheel(60.0, 36.0, 0.0, 100.0, 0, 0);
        fg_host_event(0, "resize");
        fg_host_event(1, "visibilitychange");
        check(panel.events() == before && panel.closes == closes && seam.destroyed == 1,
              "after the host nothing reaches the Panel");
        check(fg_host_heard(0) == resizes && fg_host_heard(1) == visibilities && store.reloads == reloads,
              "and none of its listeners is left on the window or the document: a resize runs "
                  + std::to_string(fg_host_heard(0) - resizes) + " of them, a visibilitychange "
                  + std::to_string(fg_host_heard(1) - visibilities) + ", and the preferences are not reloaded");
        seam.onLetGo = nullptr;
    }

    // ---- 2. the zoom, unpinned --------------------------------------------------------------------------------------
    void zoom()
    {
        auto& prefs = UiPreferences::get();
        fg_host_inner(2000, 1500);                   // a window every step fits
        prefs.setInt(kZoomKey, 100);
        {
            Recorder panel(240, 160);
            WebHostConfig config = pinned(0);
            config.zoomSteps = { 150, 100, 100, 500, 20, 125, 175 };
            config.defaultZoomPercent = 125;
            config.zoomPrefKey = kZoomKey;
            WebHost host(panel, std::move(config));
            funkgui::HostServices& hs = *panel.host;
            check(std::vector<int>(hs.zoomSteps().begin(), hs.zoomSteps().end()) == kSteps && hs.zoomPercent() == 100
                      && canvasIs(240.0, 160.0),
                  "unpinned, the steps are cleaned and the host opens at the preference, 100: " + cssSize());
            frame(host);
            hs.setZoomPercent(150);
            check(hs.zoomPercent() == 150 && canvasIs(240.0, 160.0) && prefs.getInt(kZoomKey, 0, 0, 1000) == 150,
                  "setZoomPercent(150) answers at once, writes the preference and leaves the canvas until the frame");
            frame(host);
            check(canvasIs(360.0, 240.0) && bufferIs(720.0, 480.0) && sameBits(host.lastFrame().info.dpi, 3.0f)
                      && host.diagnostics().zoomPercent == 150,
                  "the next frame resizes the canvas (" + cssSize() + ") and its buffer (" + bufferSize()
                      + "), dpi 3");
            hs.setZoomPercent(130);
            frame(host);
            check(hs.zoomPercent() == 150 && prefs.getInt(kZoomKey, 0, 0, 1000) == 150 && canvasIs(360.0, 240.0),
                  "a value that is not a step is ignored");
            prefs.setInt(kZoomKey, 125);             // another host of the page, another tab's write reloaded
            frame(host);
            check(hs.zoomPercent() == 125 && canvasIs(300.0, 200.0) && bufferIs(600.0, 400.0)
                      && sameBits(host.lastFrame().info.dpi, 2.5f),
                  "another writer of the preference is followed on the next frame: " + cssSize() + ", buffer "
                      + bufferSize());

            // The fit follows the window: 330 px take 125 % (300) and not 150 % (360).
            fg_host_inner(330, 1500);
            fg_host_event(0, "resize");
            hs.setZoomPercent(175);
            frame(host);
            check(hs.zoomPercent() == 125 && prefs.getInt(kZoomKey, 0, 0, 1000) == 175 && canvasIs(300.0, 200.0)
                      && !hs.zoomFits(175) && !hs.zoomFits(150) && hs.zoomFits(125) && hs.zoomFits(100)
                      && !hs.zoomFits(110),
                  "in a 330 px window a chosen 175 % is kept as the preference and drawn at 125 %, and zoomFits() says "
                  "which steps fit");
            fg_host_inner(2000, 1500);
            fg_host_event(0, "resize");
            const bool refitted = hs.zoomPercent() == 175 && canvasIs(300.0, 200.0);
            frame(host);
            check(refitted && canvasIs(420.0, 280.0),
                  "a window resize refits (175 % at once) and the next frame resizes the canvas: " + cssSize());
            fg_host_inner(2000, 250);
            fg_host_event(0, "resize");
            frame(host);
            check(hs.zoomPercent() == 150 && canvasIs(360.0, 240.0), "the height binds too: 250 px take 150 %");
            fg_host_inner(100, 100);
            fg_host_event(0, "resize");
            frame(host);
            check(hs.zoomPercent() == 100 && canvasIs(240.0, 160.0), "when nothing fits it is the smallest step");
        }
        {
            // The config's margins are taken off the window: 2000 - 1640 = 360 px take 150 % exactly.
            fg_host_inner(2000, 1500);
            Recorder panel(240, 160);
            WebHostConfig config = pinned(0);
            config.zoomSteps = kSteps;
            config.zoomPrefKey = kZoomKey;           // holds 175
            config.fitMarginX = 1640;
            config.fitMarginY = 100;
            WebHost host(panel, std::move(config));
            check(panel.host->zoomPercent() == 150 && canvasIs(360.0, 240.0),
                  "a new host reads the preference and fits it to the window less the config's margins: 150 %");
        }
        {
            // A window no larger than the margins has room for no step: the smallest one, as wherever none fits, and
            // not the preference unfitted (which is for a window that is not known).
            fg_host_inner(2000, 90);
            Recorder panel(240, 160);
            WebHostConfig config = pinned(0);
            config.zoomSteps = kSteps;
            config.zoomPrefKey = kZoomKey;           // holds 175
            config.fitMarginX = 32;
            config.fitMarginY = 92;
            WebHost host(panel, std::move(config));
            funkgui::HostServices& hs = *panel.host;
            check(hs.zoomPercent() == 100 && canvasIs(240.0, 160.0) && !hs.zoomFits(175) && !hs.zoomFits(125),
                  "a window lower than the config's margin (90 px under a margin of 92) takes the smallest step, 100 "
                  "%, and no larger step fits: " + cssSize());
            const auto resized = [&](int w, int h) {
                fg_host_inner(w, h);
                fg_host_event(0, "resize");
                frame(host);
                return hs.zoomPercent();
            };
            const int roomy = resized(2000, 1500), exact = resized(2000, 92), onePx = resized(2000, 93);
            const int narrow = resized(20, 1500), narrowExact = resized(32, 1500);
            check(roomy == 175 && exact == 100 && onePx == 100 && narrow == 100 && narrowExact == 100
                      && canvasIs(240.0, 160.0) && !hs.zoomFits(175),
                  "and so after a resize: 175 % with room, 100 % in a window as high as the margin, 1 px higher, "
                  "narrower than the margin and as wide (" + std::to_string(roomy) + ", " + std::to_string(exact) + ", "
                      + std::to_string(onePx) + ", " + std::to_string(narrow) + ", " + std::to_string(narrowExact)
                      + ")");
            const int unknown = resized(-1, -1);
            check(unknown == 175 && hs.zoomFits(175),
                  "a window with no size at all is not known, and nothing is fitted to it: the preference, 175 %");
            fg_host_inner(2000, 1500);
        }
        {
            Recorder panel(240, 160);
            WebHostConfig config = pinned(0);
            config.zoomSteps = { 100, 125, 150 };
            config.defaultZoomPercent = 130;
            config.zoomPrefKey = "fgWebHostZoomNeverWritten";
            WebHost host(panel, std::move(config));
            check(panel.host->zoomPercent() == 125 && canvasIs(300.0, 200.0),
                  "a missing preference is the default, an unlisted default the nearest step: 125 %");
        }
        {
            Recorder panel(240, 160);
            WebHost host(panel, pinned(0));
            panel.host->setZoomPercent(150);
            frame(host);
            check(panel.host->zoomPercent() == 100 && panel.host->zoomSteps().empty() && canvasIs(240.0, 160.0),
                  "without steps there is no zoom: the canvas is the Panel's size and setZoomPercent does nothing");
        }
        fg_host_inner(0, 0);
        prefs.setInt(kZoomKey, 100);

        // ---- the scale: the browser's device pixel ratio, and a pinned one that changes
        {
            Recorder panel(240, 160);
            WebHostConfig config = pinned(125);
            config.capture.uiScale = 0.0f;
            WebHost host(panel, std::move(config));
            frame(host);
            const double ratio = fg_host_ratio();
            const WebHost::Diagnostics d = host.diagnostics();
            check(d.scale == ratio && std::fabs(d.physW - 300.0 * ratio) <= 1.0
                      && std::fabs(d.physH - 200.0 * ratio) <= 1.0 && bufferIs(d.physW, d.physH)
                      && sameBits(host.lastFrame().info.dpi, static_cast<float>(d.physH) / 160.0f),
                  "unpinned, the buffer is the CSS size times the device pixel ratio (" + num(ratio) + "): "
                      + bufferSize() + ", dpi " + num(host.lastFrame().info.dpi));
        }
        {
            Recorder panel(240, 160);
            WebHostConfig config = pinned(100);
            config.capture.uiScale = 1.5f;
            config.capture.uiScaleAfter = 2;
            WebHost host(panel, std::move(config));
            frame(host);
            frame(host);
            const double ratio = fg_host_ratio();
            const bool before = host.diagnostics().scale == ratio;
            frame(host);
            check(before && bufferIs(360.0, 240.0) && canvasIs(240.0, 160.0)
                      && sameBits(host.lastFrame().info.dpi, 1.5f) && host.diagnostics().scale == 1.5,
                  "a scale that changes (uiScale 1.5 from the third frame) resizes the buffer alone: " + cssSize()
                      + " CSS px, buffer " + bufferSize() + ", dpi 1.5");
        }

        // ---- the dt: a pin the frame clock would never give (it clamps at 100 ms)
        {
            Recorder panel(240, 160);
            WebHostConfig config = pinned(100);
            config.capture.fixedDt = 0.125f;
            WebHost host(panel, std::move(config));
            frame(host);
            frame(host);
            const funkgui::FrameInfo& info = host.lastFrame().info;
            check(sameBits(panel.firstDt, 0.125f) && sameBits(panel.lastDt, 0.125f) && sameBits(info.dt, 0.125f)
                      && info.fixedClock && sameBits(info.seconds, 0.25f) && info.frame == 2
                      && panel.host->nowSeconds() == 0.25,
                  "a pinned dt is the dt of every tick, whatever the timestamps: 0.125 s, the Panel's clock at 0.25 s "
                  "after two frames");
        }

        // ---- the theme, unpinned
        {
            Recorder panel(240, 160);
            WebHostConfig config = pinned(100);
            config.capture.uiTheme = -1;
            WebHost host(panel, std::move(config));
            frame(host);
            const bool opened = panel.host->themeIndex() == 0 && host.lastFrame().info.theme == 0;
            prefs.setTheme(1);
            const bool atOnce = panel.host->themeIndex() == 1;
            frame(host);
            const funkgui::FrameInfo& info = host.lastFrame().info;
            check(opened && atOnce && info.theme == 1 && info.clear.r == Theme::byIndex(1).ground.r
                      && info.clear.g == Theme::byIndex(1).ground.g,
                  "unpinned, themeIndex() answers a preference change at once and the next frame draws with it");
            prefs.setTheme(0);
        }

        // ---- no canvas
        {
            Recorder panel(240, 160);
            WebHostConfig config = pinned(100);
            config.canvasSelector = "#funkgui-no-such-canvas";
            WebHost host(panel, std::move(config));
            const WebHost::FrameResult r = frame(host);
            check(!host.ok() && host.error()[0] != '\0' && !r.submitted && panel.ticks == 1
                      && host.diagnostics().frames == 0 && host.lastFrame().prims.size() > 0,
                  std::string("a host whose selector names no canvas is not ok(), says why, and its frames tick and "
                              "record without drawing: ")
                      + host.error());
        }
    }

    // ---- 6. what a host asks of its clock, driven by frame(): no real time ------------------------------------------
    void requests()
    {
        auto& prefs = UiPreferences::get();
        fg_host_inner(2000, 1500);                   // a window every step fits
        prefs.setInt(kZoomKey, 100);
        Recorder panel(240, 160);
        WebHostConfig config = pinned(0);
        config.zoomSteps = kSteps;
        config.zoomPrefKey = kZoomKey;
        WebHost host(panel, std::move(config));
        funkgui::HostServices& hs = *panel.host;
        // One frame: the rate it says the clock was asked for before it (FrameInfo::fullRate). `wanted` is what the
        // Panel asked after it.
        bool wanted = false;
        const auto asked = [&] {
            wanted = frame(host).wantsFullRate;
            return host.lastFrame().info.fullRate;
        };

        // ---- the request before the frame, not the frame's own
        const bool first = asked(), idle = asked();
        panel.wantFull = true;
        const bool atStart = asked(), startWanted = wanted;      // asked before it: idle
        const bool during = asked();
        panel.wantFull = false;
        const bool atEnd = asked(), endWanted = wanted;          // asked before it: full
        const bool after = asked();
        check(!first && !idle && !atStart && startWanted && during && atEnd && !endWanted && !after,
              std::string("FrameInfo::fullRate is the request before the frame, not the frame's own: the first frame "
                          "of a Panel that starts to want full rate says idle, the first after it stopped says full (")
                  + (atStart ? "full" : "idle") + ", " + (atEnd ? "full" : "idle") + ")");

        // ---- input asks for full rate until the next frame's own request, whatever the Panel wants
        const auto nudged = [&](const char* what) {
            const bool full = asked() && !wanted;
            const bool then = asked();
            if (!full || then)
                say("         %s: the frame after it says %s, the next %s", what, full ? "full" : "idle",
                    then ? "full" : "idle");
            return full && !then;
        };
        fg_host_pointer("pointerdown", 200.0, 100.0, 0, 1, 0);
        const bool byPress = nudged("a press");
        fg_host_pointer("pointerup", 200.0, 100.0, 0, 0, 0);
        const bool notByRelease = !asked();
        fg_host_wheel(60.0, 36.0, 0.0, 100.0, 0, 0);
        const bool byWheel = nudged("a wheel");
        fg_host_key("ArrowRight", 0);
        const bool byKey = nudged("a key");
        fg_host_event(1, "visibilitychange");
        const bool byShown = nudged("the document shown again");
        check(byPress && notByRelease && byWheel && byKey,
              std::string("a press, a wheel and a key each ask for full rate until the next frame, and a release "
                          "does not (press ") + (byPress ? "full" : "idle") + ", release "
                  + (notByRelease ? "idle" : "full") + ", wheel " + (byWheel ? "full" : "idle") + ", key "
                  + (byKey ? "full" : "idle") + ")");
        check(byShown, "and so does the document when it is shown again");

        // ---- the preferences and the zoom come before the hidden gate
        const int ticks = panel.ticks;
        const uint32_t drawn = host.diagnostics().frames;
        fg_host_hidden(1);
        hs.setZoomPercent(150);
        const bool chosenHidden = !frame(host).submitted && canvasIs(360.0, 240.0);
        const std::string chosenSize = cssSize();
        prefs.setInt(kZoomKey, 125);                 // another writer of the preference
        const bool followedHidden = !frame(host).submitted && canvasIs(300.0, 200.0);
        check(chosenHidden && followedHidden && panel.ticks == ticks && host.diagnostics().frames == drawn,
              "a frame of a hidden document applies a zoom chosen since the last one (" + chosenSize
                  + ") and follows the preference (" + cssSize() + ") before it stops: nothing ticked or drawn");
        fg_host_hidden(0);
        fg_host_inner(0, 0);
        prefs.setInt(kZoomKey, 100);
    }

    // ---- 8. the product's hook, driven by frame(): no real time -----------------------------------------------------
    void hook()
    {
        Recorder panel(240, 160);
        auto token = std::make_shared<int>(0);       // held by the hook: who else holds it holds the hook
        WebHost* self = nullptr;
        bool reenter = false;                        // the next call asks its host for a frame, once
        WebHost::FrameResult inner{ true, true };    // what that frame() from inside the hook answered
        WebHostConfig config = pinned(100);
        config.beforeTick = [&, token] {
            ++panel.brought;
            if (std::exchange(reenter, false))
                inner = self->frame(stamp);
        };
        auto host = std::make_unique<WebHost>(panel, std::move(config));
        self = host.get();
        const auto counts = [&] {
            return std::to_string(panel.brought) + " calls, " + std::to_string(panel.ticks) + " ticks, "
                 + std::to_string(panel.fedTicks) + " of them after exactly their own call";
        };

        // ---- once per frame that ticks, before the tick
        check(host->ok() && panel.brought == 0 && panel.ticks == 0,
              "constructing a host with a beforeTick hook calls it no more than it ticks: not at all");
        const WebHost::FrameResult first = frame(*host);
        check(first.submitted && panel.ticks == 1 && panel.brought == 1 && panel.broughtSeen == 1,
              "frame() calls the hook once, before the tick: the Panel's tick saw what the hook brought in the same "
              "frame (" + std::to_string(panel.brought) + " call, " + std::to_string(panel.broughtSeen)
                  + " seen by the tick)");
        panel.wantFull = true;                       // whatever rate the Panel asks for, and after input
        frame(*host);
        frame(*host);
        panel.wantFull = false;
        fg_host_pointer("pointermove", 30.0, 30.0, 0, 0, 0);
        fg_host_key("ArrowRight", 0);
        const bool inputAlone = panel.moves == 1 && panel.keys == 1 && panel.brought == 3;
        frame(*host);
        frame(*host);
        check(panel.ticks == 5 && panel.brought == 5 && panel.fedTicks == 5 && panel.broughtSeen == 5 && inputAlone,
              "and so in every frame, exactly once: after 5 frames " + counts() + "; a pointer move and a key reach "
              "the Panel without it");

        // ---- inside the frame
        reenter = true;
        const WebHost::FrameResult outer = frame(*host);
        check(outer.submitted && !reenter && !inner.submitted && !inner.wantsFullRate && panel.ticks == 6
                  && panel.brought == 6,
              "the hook runs inside the frame: a frame() from it is refused, with no second call and no second tick ("
                  + counts() + ")");

        // ---- no tick, no hook
        const uint32_t drawn = host->diagnostics().frames;
        int calls = panel.brought, ticks = panel.ticks;
        fg_host_hidden(1);
        const bool drewOnce = frame(*host).submitted, drewTwice = frame(*host).submitted;
        const int hiddenCalls = panel.brought - calls, hiddenTicks = panel.ticks - ticks;
        fg_host_hidden(0);
        check(!drewOnce && !drewTwice && hiddenCalls == 0 && hiddenTicks == 0
                  && host->diagnostics().frames == drawn,
              "a frame of a hidden document, which ticks nothing, does not call the hook: "
                  + std::to_string(hiddenCalls) + " calls and " + std::to_string(hiddenTicks) + " ticks in 2 frames");
        frame(*host);
        check(panel.ticks == 7 && panel.brought == 7 && panel.fedTicks == 7,
              "shown again, the next frame calls it once, before its tick (" + counts() + ")");

        // ---- the teardown
        const long held = token.use_count();
        calls = panel.brought;
        ticks = panel.ticks;
        host.reset();
        check(held == 2 && token.use_count() == 1 && panel.brought == calls && panel.ticks == ticks,
              "~WebHost does not call the hook and takes it along: nothing holds it after the host (held by "
                  + std::to_string(held - 1) + " before, " + std::to_string(token.use_count() - 1) + " after; "
                  + std::to_string(panel.brought - calls) + " calls in the teardown)");

        // ---- a frame that ticks without drawing
        {
            Recorder undrawn(240, 160);
            WebHostConfig noCanvas = pinned(100);
            noCanvas.canvasSelector = "#funkgui-no-such-canvas";
            noCanvas.beforeTick = [&undrawn] { ++undrawn.brought; };
            WebHost blind(undrawn, std::move(noCanvas));
            const bool drew = frame(blind).submitted, drewAgain = frame(blind).submitted;
            check(!blind.ok() && !drew && !drewAgain && undrawn.ticks == 2 && undrawn.brought == 2
                      && undrawn.fedTicks == 2,
                  "a host with no canvas ticks without drawing, and calls the hook before each tick all the same ("
                      + std::to_string(undrawn.brought) + " calls, " + std::to_string(undrawn.ticks) + " ticks)");
        }

        // ---- an empty hook
        {
            Recorder plain(240, 160);
            WebHostConfig empty = pinned(100);
            empty.beforeTick = std::function<void()>{};          // what the config holds when nothing is set
            WebHost quiet(plain, std::move(empty));
            const WebHost::FrameResult r = frame(quiet);
            check(quiet.ok() && r.submitted && plain.ticks == 1 && plain.brought == 0
                      && quiet.diagnostics().frames == 1,
                  "an empty hook is nothing: frame() ticks the Panel once and draws");
        }
    }

    // ---- 6. the clock -----------------------------------------------------------------------------------------------
    struct Clock
    {
        std::unique_ptr<Recorder> panel;
        std::unique_ptr<WebHost>  host;
        int    stage = 0;
        double stageStart = 0.0;
        int    ticksAt = 0, idlesAt = 0;             // at the start of the stage
        int    broughtAt = 0, fedAt = 0;             // the hook's calls and the ticks after their own, likewise
        int    closesMark = 0, reloadsMark = 0;      // before the action a stage judges
        int    attempts = 0;
        int    looks = 0, looksUnrequested = 0;      // stage 0's polls, and those that found no animation frame asked

    };
    Clock clockRun;

    constexpr double kPollMs = 10.0, kStageTimeoutMs = 6000.0;
    // A nudged frame comes well before the idle wait alone could end (FrameCadence::kIdleWaitMs, 66.7 ms).
    constexpr double kNudgedWithinMs = 50.0;
    constexpr int    kWarmUp = 100;                  // the stage before stage 0

    void lossStart();

    void clockEnter(int stage)
    {
        Clock& c = clockRun;
        c.stage = stage;
        c.stageStart = emscripten_performance_now();
        c.ticksAt = c.panel->ticks;
        c.idlesAt = c.panel->idles;
        c.broughtAt = c.panel->brought;
        c.fedAt = c.panel->fedTicks;
    }

    void clockStep(void*)
    {
        Clock& c = clockRun;
        Recorder& panel = *c.panel;
        const double elapsed = emscripten_performance_now() - c.stageStart;
        const int ticked = panel.ticks - c.ticksAt;
        // The hook's calls in this stage, and the ticks that came after exactly their own call.
        const int called = panel.brought - c.broughtAt, fed = panel.fedTicks - c.fedAt;
        bool next = true;                            // poll again
        switch (c.stage)
        {
            case kWarmUp:                            // the first frames: main() held the page until now, and the
                if (ticked < 2 && elapsed < kStageTimeoutMs)   // first timestamp is older than the frame it starts
                    break;
                check(sameBits(panel.firstDt, kDt) && panel.smallestDt >= 0.001f && panel.largestDt <= 0.1f
                          && c.host->lastFrame().info.displayLinked && !c.host->lastFrame().info.fixedClock,
                      "started, the first dt is 1/60 s and the next is within 1 ms .. 100 ms (" + num(panel.lastDt)
                          + ")");
                panel.smallestDt = 1.0e9f;
                clockEnter(0);
                break;
            case 0:                                  // idle: 12 Hz frames from requestAnimationFrame, idle at 10 Hz
                ++c.looks;
                c.looksUnrequested += fg_host_frames_requested() == 0 ? 1 : 0;
                if (elapsed < 900.0)
                    break;
                check(ticked >= 3 && static_cast<double>(ticked) <= elapsed / web::FrameCadence::kIdleMinMs + 2.0
                          && static_cast<double>(panel.smallestDt) * 1000.0 >= web::FrameCadence::kIdleMinMs - 0.01,
                      "an idle Panel is drawn at the idle rate: " + std::to_string(ticked) + " frames in "
                          + num(elapsed) + " ms (12 Hz would be " + num(elapsed * 0.012) + "), none sooner than "
                          + num(static_cast<double>(panel.smallestDt) * 1000.0) + " ms after the one before");
                // The wait between two idle frames is a timer, 67 of every 83 ms, and no animation frame is asked for
                // until it ends (measured: none at 5 to 9 looks of 10, by how long a requested frame was in coming
                // on a busy machine). A wait measured on another clock than the frames' is never left, and a frame
                // is then requested at every look: the bound is far from both.
                check(c.looksUnrequested * 8 >= c.looks,
                      "between idle frames the host waits on a timer, with no animation frame requested: none at "
                          + std::to_string(c.looksUnrequested) + " of " + std::to_string(c.looks) + " looks");
                check(panel.idles - c.idlesAt >= 3
                          && std::fabs(panel.lastIdleNow * 1000.0 - emscripten_performance_now()) < 500.0,
                      "Panel::idle runs from the host's timer with the performance clock: "
                          + std::to_string(panel.idles - c.idlesAt) + " calls");
                check(called == ticked && fed == ticked,
                      "the hook follows the clock, once before each tick, and the idle timer does not call it: "
                          + std::to_string(called) + " calls for " + std::to_string(ticked) + " frames ("
                          + std::to_string(fed) + " ticks after exactly their own call)");
                clockEnter(1);
                break;
            case 1:                                  // a nudge: wait for an idle frame, then move the pointer
                if (ticked == 0 && elapsed < kStageTimeoutMs)
                    break;
                fg_host_pointer("pointermove", 30.0, 30.0, 0, 0, 0);
                ++c.attempts;
                clockEnter(2);
                break;
            case 2:                                  // the frame the nudge caused: sooner than idle could draw one
            {
                if (ticked == 0 && elapsed < kStageTimeoutMs)
                    break;
                const bool seen = ticked > 0 && static_cast<double>(panel.lastDt) * 1000.0 < kNudgedWithinMs;
                if (!seen && c.attempts < 8)
                {
                    clockEnter(1);                   // a busy machine was late with the pointer or the frame: again
                    break;
                }
                check(seen, "input during the idle wait is drawn at once: a frame " + num(panel.lastDt * 1000.0)
                                + " ms after the one before, where the idle wait alone lasts "
                                + num(web::FrameCadence::kIdleWaitMs) + " ms (attempt " + std::to_string(c.attempts)
                                + ")");
                panel.wantFull = true;
                fg_host_pointer("pointermove", 40.0, 30.0, 0, 0, 0);
                clockEnter(3);
                break;
            }
            case 3:                                  // full rate, capped
                if (elapsed < 700.0)
                    break;
                check(ticked >= 12 && static_cast<double>(ticked) <= elapsed / web::FrameCadence::kFullMinMs + 2.0,
                      "a Panel that wants full rate gets it, and no more than the cap: " + std::to_string(ticked)
                          + " frames in " + num(elapsed) + " ms (60 Hz would be " + num(elapsed * 0.06) + ")");
                check(c.host->lastFrame().info.fullRate, "and its frames say that full rate was asked for");
                check(called == ticked && fed == ticked,
                      "the hook is called at that rate, once before each tick: " + std::to_string(called)
                          + " calls for " + std::to_string(ticked) + " frames (" + std::to_string(fed)
                          + " ticks after exactly their own call)");
                panel.wantFull = false;
                c.closesMark = panel.closes;
                fg_host_hidden(1);
                fg_host_event(1, "visibilitychange");
                clockEnter(4);
                break;
            case 4:                                  // hidden: nothing drawn, the gestures closed
            {
                if (elapsed < 400.0)
                    break;
                // frame() stamps the clock even while hidden, at the idle rate: the frame after the document is shown
                // is measured from here.
                const WebHost::FrameResult direct = c.host->frame(emscripten_performance_now());
                check(ticked == 0 && panel.ticks == c.ticksAt && !direct.submitted && panel.closes == c.closesMark + 1,
                      "a hidden document closes the Panel's gestures and draws nothing: " + std::to_string(ticked)
                          + " frames in " + num(elapsed) + " ms, and frame() ticks nothing");
                check(panel.brought == c.broughtAt,
                      "nor is the hook called while the document is hidden, by the clock or by frame(): "
                          + std::to_string(panel.brought - c.broughtAt) + " calls");
                c.reloadsMark = store.reloads;
                c.attempts = 1;
                panel.markDt();
                fg_host_hidden(0);
                fg_host_event(1, "visibilitychange");
                clockEnter(5);
                break;
            }
            case 5:                                  // shown again: the preferences reloaded, a frame at once
            {
                if (ticked == 0 && elapsed < kStageTimeoutMs)
                    break;
                if (c.attempts == 1)
                    check(ticked > 0 && store.reloads == c.reloadsMark + 1 && panel.closes == c.closesMark + 1,
                          "shown again, the host reloads the preferences and draws (" + num(elapsed) + " ms)");
                const double shownDtMs = static_cast<double>(panel.markedDt) * 1000.0;
                const bool soon = ticked > 0 && shownDtMs < kNudgedWithinMs;
                if (!soon && c.attempts < 8)
                {
                    ++c.attempts;                    // a busy machine was late with the frame: hide, stamp, show again
                    fg_host_hidden(1);
                    fg_host_event(1, "visibilitychange");
                    c.host->frame(emscripten_performance_now());
                    panel.markDt();
                    fg_host_hidden(0);
                    fg_host_event(1, "visibilitychange");
                    clockEnter(5);
                    break;
                }
                check(soon, "and at once, as after input: its first frame " + num(shownDtMs) + " ms after the frame() "
                                "before, where the idle wait alone lasts " + num(web::FrameCadence::kIdleWaitMs)
                                + " ms (attempt " + std::to_string(c.attempts) + ")");
                panel.wantFull = true;               // frames at every vsync: one is always requested when stop() comes
                fg_host_pointer("pointermove", 50.0, 30.0, 0, 0, 0);
                clockEnter(6);
                break;
            }
            case 6:                                  // stop() at full rate
                if (ticked < 4 && elapsed < kStageTimeoutMs)
                    break;
                c.closesMark = panel.closes;
                c.host->stop();
                clockEnter(7);
                break;
            case 7:                                  // stopped
                if (elapsed < 400.0)
                    break;
                check(!c.host->running() && ticked == 0 && panel.idles == c.idlesAt
                          && panel.closes == c.closesMark + 1,
                      "stop() cancels the frame it had requested, stops the idle calls and closes the gestures: "
                          + std::to_string(ticked) + " frames and " + std::to_string(panel.idles - c.idlesAt)
                          + " idle calls in " + num(elapsed) + " ms");
                check(called == 0, "and the hook is not called after stop(): " + std::to_string(called) + " calls");
                c.host->stop();                      // idempotent
                check(panel.closes == c.closesMark + 1, "a second stop() does nothing");
                panel.wantFull = false;
                panel.firstDt = 0.0f;
                panel.ticks = 0;
                c.host->start();
                c.host->start();
                clockEnter(8);
                break;
            case 8:                                  // started again
                if (ticked == 0 && elapsed < kStageTimeoutMs)
                    break;
                check(c.host->running() && panel.ticks > 0 && sameBits(panel.firstDt, kDt),
                      "start() starts them again, the first frame's dt 1/60 s once more");
                check(called == ticked && fed == ticked && called > 0,
                      "and the hook with them, before each tick: " + std::to_string(called) + " calls for "
                          + std::to_string(ticked) + " frames (" + std::to_string(fed)
                          + " ticks after exactly their own call)");
                panel.wantFull = true;               // destroyed with a frame requested and the idle timer running
                fg_host_pointer("pointermove", 60.0, 30.0, 0, 0, 0);
                clockEnter(9);
                break;
            case 9:
                if (ticked < 3 && elapsed < kStageTimeoutMs)
                    break;
                c.host.reset();                      // the destructor stops the clock
                clockEnter(10);
                break;
            case 10:
                if (elapsed < 300.0)
                    break;
                check(ticked == 0 && panel.idles == c.idlesAt,
                      "a host destroyed while its clock runs leaves no frame and no idle call behind");
                check(called == 0, "and no call of its hook: " + std::to_string(called) + " calls in " + num(elapsed)
                                       + " ms");
                next = false;
                c.panel.reset();
                lossStart();
                break;
            default: next = false; break;
        }
        if (next)
            emscripten_set_timeout(clockStep, kPollMs, nullptr);
    }

    void clockStart()
    {
        Clock& c = clockRun;
        c.panel = std::make_unique<Recorder>(240, 160);
        WebHostConfig config = pinned(100);
        config.capture.fixedDt = 0.0f;               // the browser's clock
        config.beforeTick = [] { ++clockRun.panel->brought; };
        c.host = std::make_unique<WebHost>(*c.panel, std::move(config));
        c.host->start();
        clockEnter(kWarmUp);
        emscripten_set_timeout(clockStep, kPollMs, nullptr);
    }

    // ---- 7. a context loss through the host -------------------------------------------------------------------------
    struct Loss
    {
        std::unique_ptr<Recorder> panel;
        std::unique_ptr<WebHost>  host;
        Image  before;
        double waitedMs = 0.0;
    };
    Loss loss;

    void lossFinish()
    {
        loss.host.reset();
        loss.panel.reset();
        verdict();
    }

    void afterRestore(void*)
    {
        WebHost& host = *loss.host;
        if (host.sink().lost())
        {
            loss.waitedMs += kPollMs;
            if (loss.waitedMs >= kStageTimeoutMs)
            {
                fail("the context was not restored within 6 s of restoreContext()");
                lossFinish();
                return;
            }
            emscripten_set_timeout(afterRestore, kPollMs, nullptr);
            return;
        }
        const WebHost::FrameResult r = frame(host);
        const Image after = host.sink().readPixels();
        const WebHost::Diagnostics d = host.diagnostics();
        check(r.submitted && host.ok() && d.restores == 1 && d.lost == 1 && d.frames == 2,
              "after the restore the host draws again (restores " + std::to_string(d.restores) + ", frames "
                  + std::to_string(d.frames) + ")");
        check(after.w == loss.before.w && after.h == loss.before.h && after.w > 0 && after.rgba == loss.before.rgba,
              "the same pixels as before the loss, byte for byte");
        lossFinish();
    }

    void afterLoss(void*)
    {
        WebHost& host = *loss.host;
        if (!host.sink().lost())
        {
            loss.waitedMs += kPollMs;
            if (loss.waitedMs >= kStageTimeoutMs)
            {
                fail("the context was not lost within 6 s of loseContext()");
                lossFinish();
                return;
            }
            emscripten_set_timeout(afterLoss, kPollMs, nullptr);
            return;
        }
        const int ticks = loss.panel->ticks;
        const WebHost::FrameResult r = frame(host);
        const WebHost::Diagnostics d = host.diagnostics();
        check(!r.submitted && !host.ok() && d.lost == 1 && d.frames == 1 && loss.panel->ticks == ticks + 1
                  && host.lastFrame().prims.size() > 0,
              "while the context is lost frame() ticks and records, submits nothing and counts the frame as lost");
        loss.waitedMs = 0.0;
        fg_host_restore();
        emscripten_set_timeout(afterRestore, kPollMs, nullptr);
    }

    void lossStart()
    {
        loss.panel = std::make_unique<Recorder>(240, 160);
        loss.host = std::make_unique<WebHost>(*loss.panel, pinned(100));
        const WebHost::FrameResult r = frame(*loss.host);
        loss.before = loss.host->sink().readPixels();
        if (!check(r.submitted && loss.before.w == 480 && loss.before.h == 320, "a host draws before the loss")
            || fg_host_lose() == 0)
        {
            if (firstFailure.empty())
                fail("the browser has no WEBGL_lose_context: the loss and restore could not be run");
            lossFinish();
            return;
        }
        loss.waitedMs = 0.0;
        emscripten_set_timeout(afterLoss, kPollMs, nullptr);
    }

    void run()
    {
        fg_host_setup(kCanvas);
        apple = fg_host_apple() != 0;
        say("FunkGui::web: WebHost against HeadlessHost and SoftRaster (the canvas at client %s, %s; device pixel "
            "ratio %s; platform %s)",
            num(fg_host_canvas(6)).c_str(), num(fg_host_canvas(7)).c_str(), num(fg_host_ratio()).c_str(),
            apple ? "Apple" : "not Apple");

        funkgui::FontService& fonts = funkgui::FontService::get();
        if (!check(fonts.atlas().baked() && fonts.ok(), "the atlas is the committed bake of the embedded face"))
        {
            verdict();
            return;
        }
        UiPreferences::get().setBackend(std::make_unique<StoreBackend>());

        parity();
        sizingAndInput();
        zoom();
        requests();
        hook();
        clockStart();                                // continues in clockStep, then lossStart, then the verdict
    }
}

int main()
{
    run();
    return 0;
}

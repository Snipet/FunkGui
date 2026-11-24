#include <funkgui/desktop/DesktopHost.h>

#include "DesktopSurface.h"

#include "../gpu/BgfxReset.h"
#include "../gpu/FrameGate.h"
#include "../gpu/ShaderLibraryDetail.h"

#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/FrameCompare.h>
#include <funkgui/canvas/Images.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/core/Config.h>
#include <funkgui/core/Theme.h>
#include <funkgui/desktop/DesktopInput.h>
#include <funkgui/desktop/MenuOverlay.h>
#include <funkgui/gpu/BgfxContext.h>
#include <funkgui/gpu/BgfxSink.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/text/FontService.h>
#include <funkgui/web/WebClock.h>

#include <SDL3/SDL.h>
#define SDL_MAIN_HANDLED                             // SDL_main.h for SDL_SetMainReady() only: main() is the caller's
#include <SDL3/SDL_main.h>
#if defined(__APPLE__)
  #include <SDL3/SDL_metal.h>
#endif

#include <bgfx/bgfx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

// DesktopHost (v0.28.0): EditorHost.cpp's frame and WebHost.cpp's clock, sizing and input around a window of SDL3's.
// The rules that need no window are in desktop/DesktopInput.h, web/WebInput.h and web/WebClock.h (fg.desktop.input,
// fg.web.input, fg.web.clock); this file is SDL and bgfx around them. Choices where the header is silent:
//
// - SDL. open() initialises SDL's video subsystem and close() lets it go (SDL counts the two), never SDL_Quit: an
//   application may hold other subsystems (audio, gamepads). On Linux the video driver is asked to be x11 before the
//   first initialisation, since bgfx is given an X11 window; a video subsystem that is already up on another driver is
//   refused in words.
// - The window is made hidden at the Panel's size, sized for its display's scale, registered with bgfx and only then
//   shown, so no frame of the system's own background is seen first.
// - The native handle bgfx gets: macOS, the CAMetalLayer of a view SDL makes for Metal (SDL_Metal_CreateView), so
//   nothing here is Objective-C; Windows, the HWND; Linux, the X11 window with the Display (src/desktop/
//   NativeSurface.cpp nativeDisplay()).
// - Vsync. BgfxContext starts and resets bgfx without vsync (src/gpu/BgfxReset.h: a plug-in's frames are paced by the
//   FramePump). Here the display paces them: after every reset the context makes, the host resets again with the
//   same size and flags and BGFX_RESET_VSYNC (bgfx applies the last reset of a frame).
// - One press at a time: the first button down takes it, and other buttons are ignored until it comes up. SDL keeps
//   the pointer captured while a button is down, so a drag keeps arriving outside the window. A release counts as a
//   double click by SDL's click count (the system's double-click time) unless the pointer moved 4 px from the press
//   (JUCE's rule that a drag is no multiple click).
// - Menus (desktop/MenuOverlay.h) are drawn into the frame after the Panel. While one shows, every pointer, wheel
//   and key event is the menu's and the Panel gets none: it is told the pointer left (pointerExit) when the menu
//   first takes a pointer event, and its next move after the menu closed is a move again. A press the Panel took
//   before the menu opened (the click on a dropdown that opened it) stays the Panel's until its release.
// - A window that is hidden or minimised ticks and draws nothing, and a frame drawn while SDL reports it occluded is
//   drawn again once it is not (macOS presents nothing for such a window).
// - The event watch draws only on the main thread, never inside a frame, and not more often than every 4 ms.

namespace funkgui::desktop
{
    namespace
    {
#if defined(__APPLE__)
        constexpr bool kApple = true;
#else
        constexpr bool kApple = false;
#endif
#if defined(__linux__)
        constexpr bool kKeepsFrame = false;          // an X11 window comes back from being unmapped without its picture
#else
        constexpr bool kKeepsFrame = true;
#endif

        constexpr double kIdlePeriodMs = 100.0;      // Panel::idle at 10 Hz, as the other hosts
        constexpr double kWatchMinMs = 4.0;          // the event watch's frames are at least this far apart
        constexpr float  kDragPx = 4.0f;             // a press that moved this far is no multiple click
        constexpr int    kCaptureFrames = 8;         // bgfx::frame() calls a read-back may take

        // DesktopInput.h's numbers are SDL's.
        static_assert(kModShift == SDL_KMOD_SHIFT && kModCtrl == SDL_KMOD_CTRL && kModAlt == SDL_KMOD_ALT
                      && kModGui == SDL_KMOD_GUI);
        static_assert(kButtonLeft == SDL_BUTTON_LEFT && kButtonMiddle == SDL_BUTTON_MIDDLE
                      && kButtonRight == SDL_BUTTON_RIGHT);
        static_assert(kKeyBackspace == SDLK_BACKSPACE && kKeyTab == SDLK_TAB && kKeyReturn == SDLK_RETURN
                      && kKeyEscape == SDLK_ESCAPE && kKeySpace == SDLK_SPACE && kKeyDelete == SDLK_DELETE);
        static_assert(kKeyRight == SDLK_RIGHT && kKeyLeft == SDLK_LEFT && kKeyDown == SDLK_DOWN && kKeyUp == SDLK_UP
                      && kKeyHome == SDLK_HOME && kKeyPageUp == SDLK_PAGEUP && kKeyEnd == SDLK_END
                      && kKeyPageDown == SDLK_PAGEDOWN && kKeyKeypadEnter == SDLK_KP_ENTER);

        double nowMs()
        {
            return static_cast<double>(SDL_GetTicksNS()) * 1.0e-6;
        }

        std::string sdlError(const char* what)
        {
            const char* e = SDL_GetError();
            std::string why = what;
            if (e != nullptr && *e != 0)
            {
                why += ": ";
                why += e;
            }
            return why;
        }

        SDL_SystemCursor systemCursor(Cursor c) noexcept
        {
            switch (c)
            {
                case Cursor::normal:       return SDL_SYSTEM_CURSOR_DEFAULT;
                case Cursor::leftRight:    return SDL_SYSTEM_CURSOR_EW_RESIZE;
                case Cursor::upDown:       return SDL_SYSTEM_CURSOR_NS_RESIZE;
                case Cursor::pointingHand: return SDL_SYSTEM_CURSOR_POINTER;
                case Cursor::crosshair:    return SDL_SYSTEM_CURSOR_CROSSHAIR;
                // SDL has no open hand, closed hand or copy cursor (DesktopHost.h says what shows instead).
                case Cursor::grab:         return SDL_SYSTEM_CURSOR_POINTER;
                case Cursor::grabbing:     return SDL_SYSTEM_CURSOR_MOVE;
                case Cursor::copy:         return SDL_SYSTEM_CURSOR_DEFAULT;
                case Cursor::notAllowed:   return SDL_SYSTEM_CURSOR_NOT_ALLOWED;
            }
            return SDL_SYSTEM_CURSOR_DEFAULT;
        }
    }

    //==================================================================================================================
    // Impl
    //==================================================================================================================

    struct DesktopHost::Impl
    {
        Impl(DesktopHost& h, Panel& p, Options o)
            : host(h), panel(p), options(std::move(o)), canvas(FontService::get().atlas())
        {
        }

        DesktopHost& host;
        Panel&       panel;
        Options      options;
        Canvas       canvas;
        BgfxSink     sink;
        funkgui::detail::FrameGate gate;
        PrimList     drawnList;                      // the last recording submitted (capacity kept)
        bool         haveDrawn = false;

        Theme    theme = Theme::graphite();
        int      themeIdx = 0;
        uint32_t prefsRevision = 0;

        SDL_Window*  window = nullptr;
        SDL_WindowID windowId = 0;
        void*        metalView = nullptr;            // macOS: SDL's view for Metal
        void*        nwh = nullptr;                  // what BgfxContext holds the window by
        bool         videoUp = false;                // this host holds SDL's video subsystem
        bool         watching = false;               // the event watch is installed
        bool         open = false;
        bool         quit = false;
        SDL_ThreadID mainThread = 0;
        uint32_t     wakeEvent = 0;                  // requestFrame()'s event type
        std::atomic<bool> wakePending{ false };

        web::FrameCadence cadence;
        double   lastFrameMs = 0.0;                  // the last frame's time (the cadence's stamp)
        bool     framed = false;
        double   lastIdleMs = 0.0;
        bool     inFrame = false;
        bool     firstFrameDone = false;
        double   seconds = 0.0;                      // the Panel's clock: the sum of the dts it was ticked with
        Diagnostics diag{};

        int    physW = 0, physH = 0;                 // the drawable bgfx was last sized for
        int    logicalW = 0, logicalH = 0;           // the Panel's size the window is sized for
        double sizedFor = 0.0;                       // display scale x zoom the window was last sized for
        bool   resizable = false;                    // the Panel's resizable() and Options::resizable, at open()

        bool   pressed = false;
        int    pressedButton = 0;
        int    pressClicks = 1;
        bool   pressMoved = false;                   // 4 px from the press: no multiple click
        float  pressX = 0.0f, pressY = 0.0f;         // window coordinates of the press
        float  virtX = 0.0f, virtY = 0.0f;           // the drag's position (virtual in relative mode)
        bool   unbounded = false;

        Cursor      cursor = Cursor::normal;
        bool        cursorSet = false;
        SDL_Cursor* cursors[SDL_SYSTEM_CURSOR_COUNT] = {};

        std::vector<int> zoomSteps;
        std::string      zoomPrefKey;
        int zoomDefault = 100, zoomChosen = 100, zoomTarget = 100, zoomApplied = 100;

        MenuOverlay menu;
        uint32_t menuDrawn = 0;                      // menu.revision() of the last frame drawn
        bool     menuHasPointer = false;             // the Panel was told the pointer left: the menu has it
        bool     pressIsMenus = false;               // the press under way began while a menu showed

        // ---- sizes --------------------------------------------------------------------------------------------------

        double displayScale() const
        {
            const float s = window != nullptr ? SDL_GetWindowDisplayScale(window) : 0.0f;
            return s > 0.05f ? static_cast<double>(s) : 1.0;
        }

        double pixelDensity() const
        {
            const float d = window != nullptr ? SDL_GetWindowPixelDensity(window) : 0.0f;
            return d > 0.05f ? static_cast<double>(d) : 1.0;
        }

        // Physical px per logical px the window is to be sized for: the display's content scale times the zoom.
        double wantedScale() const
        {
            return displayScale() * static_cast<double>(zoomApplied) / 100.0;
        }

        // Logical px -> window coordinates at scale `s`.
        int windowUnits(int logical, double s) const
        {
            return std::max(1, static_cast<int>(std::lround(static_cast<double>(logical) * s / pixelDensity())));
        }

        // The window takes the size of logicalW x logicalH at the wanted scale; a resizable one also its limits.
        void sizeWindow()
        {
            const double s = wantedScale();
            sizedFor = s;
            if (resizable)
            {
                SDL_SetWindowMinimumSize(window, windowUnits(panel.minWidth(), s), windowUnits(panel.minHeight(), s));
                SDL_SetWindowMaximumSize(window, options.maxWidth > 0 ? windowUnits(options.maxWidth, s) : 0,
                                         options.maxHeight > 0 ? windowUnits(options.maxHeight, s) : 0);
            }
            SDL_SetWindowSize(window, windowUnits(logicalW, s), windowUnits(logicalH, s));
        }

        void applyReset() const
        {
            if (physW > 0 && physH > 0)
                bgfx::reset(static_cast<uint32_t>(physW), static_cast<uint32_t>(physH),
                            funkgui::detail::kBgfxResetFlags | (options.vsync ? BGFX_RESET_VSYNC : BGFX_RESET_NONE));
        }

        // Step 3 of the frame: the window's size for the Panel, the Panel's size for the window, bgfx's drawable.
        // False when the window has no pixels to draw into.
        bool refreshDrawable()
        {
            const double s = wantedScale();
            if (!resizable)
            {
                // A fixed Panel: the window follows its size (which a host application may change between frames),
                // the display's scale and the zoom.
                if (panel.width() != logicalW || panel.height() != logicalH || s != sizedFor)
                {
                    logicalW = panel.width();
                    logicalH = panel.height();
                    sizeWindow();
                }
            }
            else if (s != sizedFor)
                sizeWindow();                        // the same logical size on the new display or at the new zoom

            int pw = 0, ph = 0;
            if (!SDL_GetWindowSizeInPixels(window, &pw, &ph) || pw < 1 || ph < 1)
                return false;
            if (resizable)
            {
                int lw = static_cast<int>(std::lround(static_cast<double>(pw) / s));
                int lh = static_cast<int>(std::lround(static_cast<double>(ph) / s));
                lw = std::max(lw, panel.minWidth());
                lh = std::max(lh, panel.minHeight());
                if (options.maxWidth > 0)
                    lw = std::min(lw, options.maxWidth);
                if (options.maxHeight > 0)
                    lh = std::min(lh, options.maxHeight);
                if (lw != panel.width() || lh != panel.height())
                    panel.resize(lw, lh);
                logicalW = panel.width();
                logicalH = panel.height();
            }
            if (pw != physW || ph != physH)
            {
                physW = pw;
                physH = ph;
                BgfxContext::get().resizeWindow(nwh, physW, physH);
                applyReset();
            }
            return true;
        }

        // ---- theme and zoom -----------------------------------------------------------------------------------------

        void applyTheme(int idx)
        {
            themeIdx = std::clamp(idx, 0, Theme::kCount - 1);
            theme = Theme::byIndex(themeIdx);
        }

        bool isZoomStep(int percent) const noexcept
        {
            return std::binary_search(zoomSteps.begin(), zoomSteps.end(), percent);
        }

        int readZoomPreference() const
        {
            if (zoomSteps.empty())
                return 100;
            if (zoomPrefKey.empty())
                return zoomDefault;
            const int v = UiPreferences::get().getInt(zoomPrefKey.c_str(), zoomDefault,
                                                      std::numeric_limits<int>::min(),
                                                      std::numeric_limits<int>::max());
            return isZoomStep(v) ? v : zoomDefault;
        }

        // The largest step at or below `chosen` whose window fits the usable area of the window's display, in
        // logical px at 100 % (web::fitZoom's rule; an unknown area fits everything).
        int fitZoom(int chosen) const
        {
            int areaW = 0, areaH = 0;
            SDL_Rect usable{};
            const SDL_DisplayID display = window != nullptr ? SDL_GetDisplayForWindow(window) : 0;
            if (display != 0 && SDL_GetDisplayUsableBounds(display, &usable) && usable.w > 0 && usable.h > 0)
            {
                int top = 0, left = 0, bottom = 0, right = 0;
                if (!SDL_GetWindowBordersSize(window, &top, &left, &bottom, &right))
                    top = left = bottom = right = 0;
                const double perLogical = displayScale() / pixelDensity();    // window units per logical px at 100 %
                areaW = std::max(1, static_cast<int>(static_cast<double>(usable.w - left - right) / perLogical));
                areaH = std::max(1, static_cast<int>(static_cast<double>(usable.h - top - bottom) / perLogical));
            }
            return web::fitZoom(zoomSteps, chosen, logicalW, logicalH, areaW, areaH);
        }

        void updateZoomTarget()
        {
            zoomTarget = zoomSteps.empty() ? 100 : fitZoom(zoomChosen);
        }

        void initZoom()
        {
            zoomSteps = web::cleanZoomSteps(options.zoomSteps);
            zoomPrefKey = options.zoomPrefKey != nullptr ? options.zoomPrefKey : "";
            zoomDefault = web::nearestZoomStep(zoomSteps, options.defaultZoomPercent);
            zoomChosen = readZoomPreference();
            zoomTarget = zoomChosen;                 // fitted once the window exists (open())
            zoomApplied = zoomTarget;
        }

        void followZoomPreference()
        {
            if (zoomSteps.empty() || zoomPrefKey.empty())
                return;
            const int chosen = readZoomPreference();
            if (chosen == zoomChosen)
                return;
            zoomChosen = chosen;
            updateZoomTarget();
        }

        // ---- the loop -----------------------------------------------------------------------------------------------

        void nudge()
        {
            cadence.nudge();
        }

        bool onMainThread() const
        {
            return SDL_GetCurrentThreadID() == mainThread;
        }

        static bool SDLCALL watch(void* userdata, SDL_Event* event)
        {
            auto& impl = *static_cast<Impl*>(userdata);
            if (!impl.open || impl.inFrame || event == nullptr || !impl.onMainThread())
                return true;
            const bool redraw = event->type == SDL_EVENT_WINDOW_EXPOSED
                             || event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED
                             || event->type == SDL_EVENT_WINDOW_RESIZED;
            if (!redraw || event->window.windowID != impl.windowId)
                return true;
            const double now = nowMs();
            if (impl.framed && now - impl.lastFrameMs < kWatchMinMs)
                return true;
            impl.gate.lost();                        // whatever was on screen, this event says to draw again
            impl.frame(now);
            return true;
        }

        void frame(double now);
        void handle(const SDL_Event&);

        // ---- input --------------------------------------------------------------------------------------------------

        // Window coordinates -> the Panel's logical px, through the window's size.
        PointerEvent pointerAt(float wx, float wy, int sdlButton, int clicks) const
        {
            int ww = 0, wh = 0;
            SDL_GetWindowSize(window, &ww, &wh);
            const uint16_t mods = static_cast<uint16_t>(SDL_GetModState());
            PointerEvent p;
            p.x = web::toLogical(static_cast<double>(wx), 0.0, static_cast<double>(ww), logicalW);
            p.y = web::toLogical(static_cast<double>(wy), 0.0, static_cast<double>(wh), logicalH);
            p.mods = modsFromSdl(mods, kApple);
            p.clicks = clicks;
            p.popup = popupFromSdl(sdlButton, mods, kApple);
            p.button = buttonFromSdl(sdlButton);
            return p;
        }

        void updateCursor()
        {
            const Cursor c = menu.showing() ? Cursor::normal : panel.cursor();
            if (cursorSet && c == cursor)
                return;
            cursor = c;
            cursorSet = true;
            const SDL_SystemCursor id = systemCursor(c);
            if (cursors[id] == nullptr)
                cursors[id] = SDL_CreateSystemCursor(id);
            if (cursors[id] != nullptr)
                SDL_SetCursor(cursors[id]);
        }

        // The menu takes a pointer event: the Panel is told once that the pointer left it.
        void menuTakesPointer()
        {
            if (menuHasPointer)
                return;
            menuHasPointer = true;
            panel.pointerExit();
        }

        void setUnbounded(bool on)
        {
            if (on == unbounded || window == nullptr)
                return;
            unbounded = on;
            SDL_SetWindowRelativeMouseMode(window, on);
        }

        // The press ends at `at`: pointerUp, then (JUCE's order) doubleClick for a release that counts two or more.
        void endPress(const PointerEvent& at, bool doubleClick)
        {
            pressed = false;
            panel.pointerUp(at);
            setUnbounded(false);                     // the widget ended its drag (or never started one)
            updateCursor();
            if (doubleClick)
            {
                panel.doubleClick(at);
                updateCursor();
            }
        }
    };

    //==================================================================================================================
    // The frame (EditorHost::submitFrame's order)
    //==================================================================================================================

    void DesktopHost::Impl::frame(double now)
    {
        if (inFrame || !open)
            return;
        struct Guard
        {
            bool& flag;
            explicit Guard(bool& f) : flag(f) { flag = true; }
            ~Guard() { flag = false; }
        } guard(inFrame);

        const float dt = cadence.advance(now);
        lastFrameMs = now;
        framed = true;
        diag.fps = cadence.fps();

        // 1. The preferences: the theme unless one is pinned, the zoom. Then a zoom chosen since the last frame.
        {
            auto& prefs = UiPreferences::get();
            if (prefs.revision() != prefsRevision)
            {
                prefsRevision = prefs.revision();
                if (options.themeIndex < 0)
                    applyTheme(prefs.theme());
                followZoomPreference();
            }
            zoomApplied = zoomTarget;                // refreshDrawable() sizes the window for it
        }

        // 2. A hidden or minimised window draws nothing and its Panel is not ticked (a window the caller asked to
        //    keep hidden is a test's: it runs).
        const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
        if ((flags & SDL_WINDOW_MINIMIZED) != 0 || ((flags & SDL_WINDOW_HIDDEN) != 0 && !options.hidden))
        {
            gate.lost();
            cadence.requestRate(false);
            return;
        }

        // 3. The drawable.
        auto& ctx = BgfxContext::get();
        if (!ctx.valid() || !ctx.owns(nwh) || !refreshDrawable())
        {
            gate.lost();
            cadence.requestRate(false);
            return;
        }

        // 4. Advance the Panel: seconds, never frames; the pinned dt when one is set. The application's hook first.
        const bool firstFrame = !firstFrameDone;
        firstFrameDone = true;
        const bool fixedClock = options.fixedDt > 0.0f;
        const float tickDt = fixedClock ? options.fixedDt : dt;
        if (options.beforeTick)
            options.beforeTick();
        panel.tick(tickDt);
        seconds += static_cast<double>(tickDt);
        ++diag.ticks;

        // 5. Whether to draw (gpu/EditorHost.h "Unchanged frames").
        const bool panelWants = panel.needsDraw();
        const bool reachesScreen = (flags & SDL_WINDOW_OCCLUDED) == 0;
        funkgui::detail::SurfaceState state;
        state.surface = ctx.surfaceRevision(nwh);
        state.physW = physW;
        state.physH = physH;
        state.logicalW = logicalW;
        state.logicalH = logicalH;
        state.theme = themeIdx;
        state.images = images::generation();
        state.shaders = shaders::detail::generation();
        menu.setBounds(static_cast<float>(logicalW), static_cast<float>(logicalH));
        const bool always = firstFrame || fixedClock || !kKeepsFrame;
        const bool forced = always || menu.revision() != menuDrawn || gate.mustDraw(state, reachesScreen);
        const auto finish = [this]
        {
            cadence.requestRate(panel.wantsFullRate());
        };
        if (!forced && !panelWants)
        {
            ++diag.skipped;
            finish();
            return;
        }

        // Record.
        FrameInfo info;
        info.logicalW = logicalW;
        info.logicalH = logicalH;
        float dpi = logicalH > 0 ? static_cast<float>(physH) / static_cast<float>(logicalH) : 1.0f;
        if (!(dpi > 0.05f))
            dpi = 1.0f;
        info.dpi = dpi;
        info.clear = theme.ground;
        info.textGamma = theme.textGamma;
        info.theme = themeIdx;
        info.seconds = static_cast<float>(seconds);
        info.frame = diag.ticks;
        info.dt = tickDt;
        info.fixedClock = fixedClock;
        info.displayLinked = options.vsync && !fixedClock;
        info.fps = fixedClock ? 0.0f : cadence.fps();
        info.fullRate = cadence.wantedFullRate();
        info.overflows = diag.overflows;
        canvas.begin(info);
        panel.draw(canvas, theme);
        menu.draw(canvas);                           // over the Panel's frame; nothing while no menu shows
        menuDrawn = menu.revision();
        const PrimList& list = canvas.end();

        diag.scale = static_cast<double>(dpi);
        diag.displayScale = displayScale();
        diag.zoomPercent = zoomApplied;
        diag.physW = physW;
        diag.physH = physH;
        diag.logicalW = logicalW;
        diag.logicalH = logicalH;

        // 6. The safety net: a recording equal to the last one drawn is not submitted.
        if (!forced && haveDrawn && sameDrawing(list, drawnList))
        {
            ++diag.unchanged;
            finish();
            return;
        }

        // Submit and present. View 0 is the window's (the default backbuffer: this host's window is bgfx's primary).
        bool shown = false;
        if (sink.submit(list, ctx.viewIdFor(nwh), ctx.framebufferFor(nwh), physW, physH)
            == BgfxSink::Result::droppedOverflow)
            ++diag.overflows;
        else
            shown = reachesScreen;
        gate.drawn(state, shown);
        drawnList = list;                            // capacity kept
        haveDrawn = true;
        ++diag.frames;
        bgfx::frame();
        ctx.afterFrame();

        // 7. Full rate only while something moves.
        finish();
    }

    //==================================================================================================================
    // Events
    //==================================================================================================================

    void DesktopHost::Impl::handle(const SDL_Event& e)
    {
        if (e.type == wakeEvent && wakeEvent != 0)
        {
            wakePending.store(false);
            nudge();
            return;
        }
        switch (e.type)
        {
            case SDL_EVENT_QUIT:
                quit = true;
                break;

            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                if (e.window.windowID == windowId)
                    quit = true;
                break;

            case SDL_EVENT_WINDOW_EXPOSED:
            case SDL_EVENT_WINDOW_SHOWN:
            case SDL_EVENT_WINDOW_RESTORED:
            case SDL_EVENT_WINDOW_RESIZED:
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
            case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
                if (e.window.windowID == windowId)
                {
                    if (e.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED && !zoomSteps.empty())
                        updateZoomTarget();          // another display may fit another step
                    gate.lost();
                    nudge();
                }
                break;

            case SDL_EVENT_WINDOW_HIDDEN:
            case SDL_EVENT_WINDOW_MINIMIZED:
            case SDL_EVENT_WINDOW_OCCLUDED:
                if (e.window.windowID == windowId)
                    gate.lost();
                break;

            case SDL_EVENT_WINDOW_MOUSE_LEAVE:
                if (e.window.windowID == windowId && !pressed)
                {
                    nudge();
                    if (!menu.pointerExit())
                        panel.pointerExit();
                    updateCursor();
                }
                break;

            case SDL_EVENT_MOUSE_MOTION:
                if (e.motion.windowID != windowId)
                    break;
                nudge();
                if (pressed)
                {
                    if (unbounded)
                    {
                        virtX += e.motion.xrel;      // the pointer is held: the drag's position is the sum of its moves
                        virtY += e.motion.yrel;
                    }
                    else
                    {
                        virtX = e.motion.x;
                        virtY = e.motion.y;
                    }
                    const float dx = virtX - pressX, dy = virtY - pressY;
                    pressMoved = pressMoved || dx * dx + dy * dy >= kDragPx * kDragPx;
                    const PointerEvent at = pointerAt(virtX, virtY, pressedButton, pressMoved ? 1 : pressClicks);
                    if (pressIsMenus)
                        static_cast<void>(menu.pointerMove(at.x, at.y));
                    else
                        panel.pointerDrag(at);
                }
                else
                {
                    const PointerEvent at = pointerAt(e.motion.x, e.motion.y, 0, 1);
                    if (menu.pointerMove(at.x, at.y))
                        menuTakesPointer();
                    else
                    {
                        menuHasPointer = false;
                        panel.pointerMove(at);
                    }
                }
                updateCursor();
                break;

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (e.button.windowID != windowId || !takesButton(e.button.button) || pressed)
                    break;
                nudge();
                pressed = true;
                pressedButton = e.button.button;
                pressClicks = std::max(1, static_cast<int>(e.button.clicks));
                pressMoved = false;
                pressX = virtX = e.button.x;
                pressY = virtY = e.button.y;
                {
                    const PointerEvent at = pointerAt(pressX, pressY, pressedButton, pressClicks);
                    pressIsMenus = menu.showing();
                    if (pressIsMenus)
                    {
                        menuTakesPointer();
                        static_cast<void>(menu.pointerDown(at.x, at.y));
                    }
                    else
                        panel.pointerDown(at);
                }
                updateCursor();
                break;

            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (!pressed || e.button.button != pressedButton)
                    break;
                nudge();
                {
                    if (!unbounded)
                    {
                        virtX = e.button.x;
                        virtY = e.button.y;
                    }
                    const int clicks = pressMoved ? 1 : std::max(1, static_cast<int>(e.button.clicks));
                    const PointerEvent at = pointerAt(virtX, virtY, pressedButton, clicks);
                    if (pressIsMenus)
                    {
                        pressed = false;
                        pressIsMenus = false;
                        static_cast<void>(menu.pointerUp(at.x, at.y));
                        updateCursor();
                    }
                    else
                    {
                        endPress(at, clicks >= 2);
                        // The release of the press that opened a menu (a dropdown's click) may be on one of its
                        // items: a press dragged onto an item and let go chooses it.
                        if (menu.showing() && pressMoved)
                            static_cast<void>(menu.pointerUp(at.x, at.y));
                    }
                }
                break;

            case SDL_EVENT_MOUSE_WHEEL:
                if (e.wheel.windowID != windowId)
                    break;
                {
                    WheelEvent w = wheelFromSdl(e.wheel.x, e.wheel.y, e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED,
                                                kApple);
                    const PointerEvent at = pointerAt(e.wheel.mouse_x, e.wheel.mouse_y, 0, 1);
                    w.x = at.x;
                    w.y = at.y;
                    w.mods = at.mods;
                    nudge();
                    if (!menu.wheel(w.x, w.y, w.dy))
                        static_cast<void>(panel.wheel(w));
                    updateCursor();
                }
                break;

            case SDL_EVENT_PINCH_UPDATE:
                if (e.pinch.windowID != windowId && e.pinch.windowID != 0)
                    break;
                {
                    float mx = 0.0f, my = 0.0f;
                    SDL_GetMouseState(&mx, &my);     // a pinch has no position of its own: the pointer's
                    const PointerEvent at = pointerAt(mx, my, 0, 1);
                    MagnifyEvent g;
                    g.x = at.x;
                    g.y = at.y;
                    g.scale = e.pinch.scale;
                    g.mods = at.mods;
                    nudge();
                    if (!menu.showing())
                        static_cast<void>(panel.magnify(g));
                    updateCursor();
                }
                break;

            case SDL_EVENT_KEY_DOWN:
                if (e.key.windowID != windowId)
                    break;
                {
                    KeyEvent ev;
                    if (!keyFromSdl(static_cast<uint32_t>(e.key.key), static_cast<uint16_t>(e.key.mod), kApple, ev))
                        break;
                    nudge();
                    if (!menu.key(ev, host.nowSeconds()))
                        static_cast<void>(panel.key(ev));
                    updateCursor();
                }
                break;

            case SDL_EVENT_TEXT_INPUT:
                if (e.text.windowID != windowId || e.text.text == nullptr)
                    break;
                {
                    const std::string_view text(e.text.text);
                    const uint16_t mods = static_cast<uint16_t>(SDL_GetModState());
                    std::size_t at = 0;
                    for (char32_t cp = nextCodePoint(text, at); cp != 0; cp = nextCodePoint(text, at))
                    {
                        if (!textIsCharacter(cp))
                            continue;
                        KeyEvent ev;
                        ev.key = Key::character;
                        ev.ch = cp;
                        ev.mods.shift = (mods & kModShift) != 0;     // typed text is no chord: the others stay off
                        nudge();
                        if (!menu.key(ev, host.nowSeconds()))
                            static_cast<void>(panel.key(ev));
                    }
                    updateCursor();
                }
                break;

            default: break;
        }
    }

    //==================================================================================================================
    // Lifetime
    //==================================================================================================================

    DesktopHost::DesktopHost(Panel& panel, Options options)
        : impl_(std::make_unique<Impl>(*this, panel, std::move(options)))
    {
    }

    DesktopHost::~DesktopHost()
    {
        close();
    }

    bool DesktopHost::isOpen() const noexcept
    {
        return impl_->open;
    }

    SDL_Window* DesktopHost::window() const noexcept
    {
        return impl_->window;
    }

    bool DesktopHost::open(std::string& why)
    {
        Impl& m = *impl_;
        why.clear();
        if (m.open)
            return true;
        m.quit = false;

        // SDL's video subsystem (the top of this file).
        SDL_SetMainReady();                          // this library never takes over main()
        if (SDL_WasInit(SDL_INIT_VIDEO) == 0)
        {
#if defined(__linux__)
            SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
#endif
        }
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
        {
            why = sdlError("SDL could not start its video subsystem (is there a display?)");
            return false;
        }
        m.videoUp = true;
        m.mainThread = SDL_GetCurrentThreadID();
        const char* driver = SDL_GetCurrentVideoDriver();
        m.diag.videoDriver = driver != nullptr ? driver : "";
#if defined(__linux__)
        if (m.diag.videoDriver != "x11")
        {
            why = "SDL's video driver is '" + m.diag.videoDriver + "': FunkGui::desktop draws into an X11 window "
                  "(on Wayland through XWayland)";
            close();
            return false;
        }
#endif
        if (m.wakeEvent == 0)
        {
            const uint32_t type = SDL_RegisterEvents(1);
            m.wakeEvent = type != 0 ? type : 0;
        }

        // The preferences and the theme.
        auto& prefs = UiPreferences::get();
        prefs.reload();
        m.prefsRevision = prefs.revision();
        m.applyTheme(m.options.themeIndex >= 0 ? m.options.themeIndex : prefs.theme());
        m.initZoom();

        // The window: hidden, at the Panel's size, then sized for its display.
        m.resizable = m.options.resizable && m.panel.resizable();
        m.logicalW = std::max(1, m.panel.width());
        m.logicalH = std::max(1, m.panel.height());
        SDL_WindowFlags flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
        if (m.resizable)
            flags |= SDL_WINDOW_RESIZABLE;
#if defined(__APPLE__)
        flags |= SDL_WINDOW_METAL;
#endif
        const std::string title = !m.options.title.empty() ? m.options.title : std::string(FUNKGUI_PRODUCT_NAME);
        m.window = SDL_CreateWindow(title.c_str(), m.logicalW, m.logicalH, flags);
        if (m.window == nullptr)
        {
            why = sdlError("SDL could not make a window");
            close();
            return false;
        }
        m.windowId = SDL_GetWindowID(m.window);
        if (!m.zoomSteps.empty())
        {
            m.updateZoomTarget();
            m.zoomApplied = m.zoomTarget;
        }
        m.sizeWindow();
        SDL_SyncWindow(m.window);                    // the size is the window's before bgfx reads it

        // The native handle (the top of this file).
        const SDL_PropertiesID props = SDL_GetWindowProperties(m.window);
#if defined(__APPLE__)
        m.metalView = SDL_Metal_CreateView(m.window);
        m.nwh = m.metalView != nullptr ? SDL_Metal_GetLayer(m.metalView) : nullptr;
        static_cast<void>(props);
#elif defined(_WIN32)
        m.nwh = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#else
        detail::setNativeDisplay(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
        m.nwh = reinterpret_cast<void*>(static_cast<uintptr_t>(
            SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0)));
#endif
        if (m.nwh == nullptr)
        {
            why = sdlError("SDL gave no native window for bgfx to draw into");
            close();
            return false;
        }

        int pw = 0, ph = 0;
        if (!SDL_GetWindowSizeInPixels(m.window, &pw, &ph) || pw < 1 || ph < 1)
        {
            pw = m.logicalW;
            ph = m.logicalH;
        }
        auto& ctx = BgfxContext::get();
        if (!ctx.acquire(m.nwh, pw, ph))
        {
#if defined(__APPLE__)
            why = "bgfx could not start on Metal";
#elif defined(_WIN32)
            why = "bgfx could not start on Direct3D 11";
#else
            why = "bgfx could not start on Vulkan (is a Vulkan driver installed? mesa-vulkan-drivers has a software one)";
#endif
            m.nwh = nullptr;                         // never registered: nothing to release
            close();
            return false;
        }
        m.physW = pw;
        m.physH = ph;
        m.applyReset();
        m.diag.renderer = bgfx::getRendererName(bgfx::getRendererType());

        m.watching = SDL_AddEventWatch(&Impl::watch, &m);
        SDL_StartTextInput(m.window);                // typed text arrives as SDL_EVENT_TEXT_INPUT
        if (!m.options.hidden)
            SDL_ShowWindow(m.window);

        m.cadence.reset();
        m.framed = false;
        m.firstFrameDone = false;
        m.haveDrawn = false;
        m.gate = {};
        m.lastIdleMs = nowMs();
        m.open = true;
        m.panel.attach(*this);
        if (m.options.setUiAttached)
            m.options.setUiAttached(true);
        return true;
    }

    void DesktopHost::close()
    {
        Impl& m = *impl_;
        const bool wasOpen = m.open;
        m.open = false;                              // no frame and no watch from here on
        if (wasOpen)
        {
            // A window can close with a button still down, in which case the release never arrives; leaving the
            // gesture open would strand the product's parameter write (EditorHost's rule).
            m.menu.dismiss();                        // unanswered: its callback does not run
            m.setUnbounded(false);
            m.pressed = false;
            m.pressIsMenus = false;
            m.menuHasPointer = false;
            m.panel.closeGestures();
            if (m.options.setUiAttached)
                m.options.setUiAttached(false);
        }
        if (m.watching)
        {
            SDL_RemoveEventWatch(&Impl::watch, &m);
            m.watching = false;
        }
        if (m.nwh != nullptr)
        {
            BgfxContext::get().release(m.nwh);       // the last window: bgfx shuts down before this returns
            m.nwh = nullptr;
        }
        detail::setNativeDisplay(nullptr);
        for (SDL_Cursor*& c : m.cursors)
        {
            if (c != nullptr)
                SDL_DestroyCursor(c);
            c = nullptr;
        }
        m.cursorSet = false;
#if defined(__APPLE__)
        if (m.metalView != nullptr)
            SDL_Metal_DestroyView(m.metalView);
#endif
        m.metalView = nullptr;
        if (m.window != nullptr)
        {
            SDL_DestroyWindow(m.window);
            m.window = nullptr;
            m.windowId = 0;
        }
        if (m.videoUp)
        {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            m.videoUp = false;
        }
        m.physW = m.physH = 0;
    }

    //==================================================================================================================
    // The loop
    //==================================================================================================================

    int DesktopHost::run()
    {
        while (pump())
        {
        }
        return 0;
    }

    bool DesktopHost::pump()
    {
        Impl& m = *impl_;
        if (!m.open)
            return false;

        // How long the loop may sleep: until the next frame is due by the cadence, and no longer than the next idle
        // call. At full rate a frame is due at once after a vsync-paced one (the wait was inside bgfx::frame()).
        // Under a pinned clock (a test, a capture) nothing waits: every turn draws a frame.
        const bool pinned = m.options.fixedDt > 0.0f;
        double now = nowMs();
        double wait = 0.0;
        if (m.framed && !pinned)
        {
            const double period = m.cadence.wantedFullRate() ? web::FrameCadence::kFullMinMs
                                                             : web::FrameCadence::kIdleMinMs;
            wait = std::min(m.lastFrameMs + period - now, m.lastIdleMs + kIdlePeriodMs - now);
        }

        SDL_Event e;
        bool got = wait >= 1.0 ? SDL_WaitEventTimeout(&e, static_cast<Sint32>(wait)) : SDL_PollEvent(&e);
        while (got && m.open)
        {
            if (!m.options.rawEvent || !m.options.rawEvent(e))
                m.handle(e);
            got = m.open && SDL_PollEvent(&e);
        }
        if (m.quit)
            close();
        if (!m.open)
            return false;

        now = nowMs();
        if (now - m.lastIdleMs >= kIdlePeriodMs)
        {
            m.lastIdleMs = now;
            m.panel.idle(nowSeconds());
        }
        if (pinned || m.cadence.due(now, false))
            m.frame(now);
        return m.open;
    }

    void DesktopHost::requestFrame()
    {
        Impl& m = *impl_;
        if (m.wakeEvent == 0 || m.wakePending.exchange(true))
            return;                                  // one wake event in the queue is enough
        SDL_Event e{};
        e.type = m.wakeEvent;
        if (!SDL_PushEvent(&e))
            m.wakePending.store(false);
    }

    DesktopHost::Diagnostics DesktopHost::diagnostics() const
    {
        return impl_->diag;
    }

    //==================================================================================================================
    // capture(): the last frame drawn again into an offscreen target and read back (gpu/GpuPicture.h's recipe)
    //==================================================================================================================

    bool DesktopHost::capture(std::vector<std::uint8_t>& rgba, int& w, int& h, std::string& why)
    {
        Impl& m = *impl_;
        rgba.clear();
        w = h = 0;
        auto& ctx = BgfxContext::get();
        if (!m.open || !ctx.valid())
        {
            why = "the host is not open";
            return false;
        }
        if (!m.haveDrawn || m.physW < 1 || m.physH < 1)
        {
            why = "no frame has been drawn yet";
            return false;
        }
        const bgfx::Caps* caps = bgfx::getCaps();
        if (caps == nullptr || (caps->supported & BGFX_CAPS_TEXTURE_READ_BACK) == 0
            || (caps->supported & BGFX_CAPS_TEXTURE_BLIT) == 0 || caps->limits.maxViews < 4u)
        {
            why = "this renderer cannot read a texture back";
            return false;
        }
        const int pw = m.physW, ph = m.physH;
        if (static_cast<uint32_t>(std::max(pw, ph)) > caps->limits.maxTextureSize)
        {
            why = "the frame is larger than the GPU's largest texture";
            return false;
        }
        // bgfx's last two views: the window's is view 0 (BgfxContext hands out 0..kMaxWindows).
        const auto drawView = static_cast<bgfx::ViewId>(caps->limits.maxViews - 2u);
        const auto blitView = static_cast<bgfx::ViewId>(caps->limits.maxViews - 1u);
        const auto tw = static_cast<uint16_t>(pw), th = static_cast<uint16_t>(ph);

        bgfx::TextureHandle colour = bgfx::createTexture2D(tw, th, false, 1, bgfx::TextureFormat::BGRA8,
                                                           BGFX_TEXTURE_RT);
        bgfx::TextureHandle readback = bgfx::createTexture2D(tw, th, false, 1, bgfx::TextureFormat::BGRA8,
                                                             BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
        bgfx::FrameBufferHandle fb = BGFX_INVALID_HANDLE;
        if (bgfx::isValid(colour))
            fb = bgfx::createFrameBuffer(1, &colour, false);
        const auto destroyTargets = [&]
        {
            if (bgfx::isValid(fb))
                bgfx::destroy(fb);
            if (bgfx::isValid(readback))
                bgfx::destroy(readback);
            if (bgfx::isValid(colour))
                bgfx::destroy(colour);
            bgfx::frame();                           // runs the destroys
            ctx.afterFrame();
        };
        if (!bgfx::isValid(colour) || !bgfx::isValid(readback) || !bgfx::isValid(fb))
        {
            destroyTargets();
            why = "bgfx refused the offscreen target";
            return false;
        }

        // The frame as the window got it (its clear, its draw calls), then the copy and the read. The window's own
        // view is not touched in these frames, so bgfx presents nothing into it and its picture stays.
        BgfxSink sink;
        const BgfxSink::Result submitted = sink.submit(m.drawnList, drawView, fb, pw, ph);
        bgfx::setViewFrameBuffer(blitView, fb);      // never the default backbuffer, the window's
        bgfx::touch(blitView);
        bgfx::blit(blitView, readback, 0, 0, colour);
        std::vector<uint8_t> bgra(static_cast<size_t>(pw) * static_cast<size_t>(ph) * 4u);
        const uint32_t ready = bgfx::readTexture(readback, bgra.data());
        uint32_t at = 0;
        for (int i = 0; i < kCaptureFrames && at < ready; ++i)
        {
            at = bgfx::frame();
            ctx.afterFrame();
        }
        bgfx::setViewFrameBuffer(drawView, BGFX_INVALID_HANDLE);
        bgfx::setViewFrameBuffer(blitView, BGFX_INVALID_HANDLE);
        bgfx::setViewClear(drawView, BGFX_CLEAR_NONE);
        destroyTargets();
        m.gate.lost();                               // the window is drawn again at its next frame, to be safe

        if (at < ready)
        {
            // Never seen: bgfx still owes the read and would write into a freed buffer. The buffer is left to it.
            static_cast<void>(new std::vector<uint8_t>(std::move(bgra)));
            why = "the read-back never completed";
            return false;
        }
        if (submitted == BgfxSink::Result::droppedOverflow)
        {
            why = "bgfx's transient buffer was full";
            return false;
        }
        w = pw;
        h = ph;
        rgba.resize(bgra.size());
        for (size_t i = 0; i < bgra.size(); i += 4u)
        {
            rgba[i] = bgra[i + 2u];
            rgba[i + 1u] = bgra[i + 1u];
            rgba[i + 2u] = bgra[i];
            rgba[i + 3u] = 255u;                     // opaque: the frame over its clear, as the window shows it
        }
        return true;
    }

    //==================================================================================================================
    // HostServices
    //==================================================================================================================

    void DesktopHost::setUnboundedDrag(bool on)
    {
        impl_->setUnbounded(on && impl_->pressed);   // only a press can be dragged without bounds
    }

    void DesktopHost::showParamMenu(ParamPort&, float, float) {}

    void DesktopHost::nudgeFullRate()
    {
        impl_->nudge();
    }

    double DesktopHost::nowSeconds() const
    {
        return impl_->options.fixedDt > 0.0f ? impl_->seconds : static_cast<double>(SDL_GetTicksNS()) * 1.0e-9;
    }

    void DesktopHost::beginBatch()
    {
        if (impl_->options.beginBatch)
            impl_->options.beginBatch();
    }

    void DesktopHost::endBatch()
    {
        if (impl_->options.endBatch)
            impl_->options.endBatch();
    }

    // What the next frame draws with: the frame's step 1 sets the theme to the preference before the Panel is ticked.
    int DesktopHost::themeIndex() const
    {
        return impl_->options.themeIndex >= 0 ? impl_->themeIdx : UiPreferences::get().theme();
    }

    int DesktopHost::zoomPercent() const
    {
        return impl_->zoomTarget;
    }

    std::span<const int> DesktopHost::zoomSteps() const
    {
        return impl_->zoomSteps;
    }

    void DesktopHost::setZoomPercent(int percent)
    {
        Impl& m = *impl_;
        if (!m.isZoomStep(percent))
            return;                                  // not a step this host offers: nothing written, nothing moves
        if (!m.zoomPrefKey.empty())
            UiPreferences::get().setInt(m.zoomPrefKey.c_str(), percent);
        m.zoomChosen = percent;
        m.updateZoomTarget();
        m.nudge();                                   // the next frame, soon, applies it
    }

    bool DesktopHost::zoomFits(int percent) const
    {
        return impl_->isZoomStep(percent) && impl_->fitZoom(percent) == percent;
    }

    unsigned DesktopHost::services() const
    {
        return hostservice::menus | hostservice::clipboard;
    }

    bool DesktopHost::showMenu(const MenuRequest& request, MenuCallback done)
    {
        Impl& m = *impl_;
        if (!m.open)
            return false;                            // `done` is dropped unrun
        m.menuHasPointer = false;
        m.nudge();                                   // drawn, or drawn away, at the next frame
        return m.menu.show(request, std::move(done), static_cast<float>(m.logicalW), static_cast<float>(m.logicalH));
    }

    void DesktopHost::dismissMenus()
    {
        impl_->menu.dismiss();
        impl_->nudge();
    }

    bool DesktopHost::chooseFiles(const FileRequest&, FilesCallback)
    {
        return false;
    }

    bool DesktopHost::copyText(std::string_view utf8)
    {
        if (!impl_->videoUp)
            return false;
        const std::string text(utf8);                // SDL wants a terminated string
        return SDL_SetClipboardText(text.c_str());
    }

    std::string DesktopHost::clipboardText() const
    {
        if (!impl_->videoUp)
            return {};
        char* text = SDL_GetClipboardText();
        std::string out = text != nullptr ? text : "";
        SDL_free(text);
        return out;
    }

    bool DesktopHost::commandKeyIsMeta() const
    {
        return kApple;
    }

    void DesktopHost::announce(std::string_view, Announce) {}

    void DesktopHost::a11yFocus(uint32_t) {}
}

#pragma once

// The live host of a Panel in a desktop window with no JUCE under it (v0.28.0; FunkGui::desktop, built only with
// FUNKGUI_WITH_DESKTOP): EditorHost's and WebHost's sibling (gpu/EditorHost.h, web/WebHost.h) over a window of SDL3's.
// It owns the window, registers it with BgfxContext (Metal on macOS, Direct3D 11 on Windows, Vulkan on an X11 window
// on Linux), runs the loop, records each frame with a member Canvas and submits it through BgfxSink, converts SDL's
// pointer, wheel, pinch, key and text events into the Panel's plain input structs (desktop/DesktopInput.h has the
// rules), and implements HostServices for the Panel. The Panel is the caller's and outlives the host. This header
// includes no SDL and no bgfx header.
//
//   funkgui::desktop::DesktopHost host(panel, { .title = "My application" });
//   std::string why;
//   if (!host.open(why)) { ...say why...; return 1; }
//   return host.run();
//
// The frame (EditorHost::submitFrame's order): (1) a change of UiPreferences::revision() is followed (the theme,
// unless Options::themeIndex pins one; the zoom preference), then a zoom chosen since the last frame is applied: the
// window takes its new size before this frame ticks; (2) nothing more happens while the window is hidden or
// minimised; (3) the drawable follows the window's size in pixels, and a resizable Panel gets resize() for a new
// logical size; (4) Options::beforeTick, then Panel::tick(dt), or the pinned dt; (5) unless Panel::needsDraw() is false
// and the host has no reason of its own to draw (gpu/EditorHost.h "Unchanged frames": the first frame, a new size,
// scale, zoom or theme, a surface made again, a picture or custom shader replaced, a menu that opened, moved or
// closed), the frame is recorded: FrameInfo as EditorHost fills it, dpi = physical height / logical height; (6) a
// recording equal to the last one drawn is not submitted (canvas/FrameCompare.h); else BgfxSink::submit, bgfx::frame()
// and BgfxContext::afterFrame(); (7) Panel::wantsFullRate() is the loop's next rate. On Linux every frame is drawn
// (an X11 window keeps no picture: gpu/NativeSurface.h renderViewKeepsFrame says why).
//
// The loop (web/WebClock.h FrameCadence, the other hosts' cadence): while the Panel wants full rate, or input arrived,
// frames are paced by the swap chain's vsync (bgfx::frame() waits for the display; never more than about 75 a second
// on a faster display); otherwise 12 frames a second, the loop asleep in SDL_WaitEventTimeout between them and woken
// by any event. Panel::idle(nowSeconds()) runs at 10 Hz or more whatever the frame rate. During a live resize of the
// window, when the system's own loop holds the main thread, frames are drawn from SDL's event watch (exposed and
// resized events), so the picture follows the edge as far as SDL and bgfx allow.
//
// Size. The window holds the Panel at its logical size times the zoom times the display's content scale (SDL's
// display scale: 2 on a Retina display, 1.5 at Windows' 150 %), and the frame's dpi is physical px per logical px, so
// the Panel draws, hit-tests and lists accessibility in its own px on every display. A Panel whose resizable() is true
// gets a resizable window (unless Options::resizable is false), limited below by minWidth() x minHeight(). A fixed
// Panel whose width() or height() changes by itself (a host application that swaps its content) has the window follow
// at the next frame.
//
// Zoom is EditorHost's (gpu/EditorHost.h "Zoom"): Options::zoomSteps turns it on, the preference under
// Options::zoomPrefKey is followed, a step that does not fit the display's usable area is drawn at the largest that
// does, and the window resizes.
//
// HostServices: themeIndex(), the zoom calls, nowSeconds() (the loop's clock, or the Panel's simulated clock under a
// pinned dt), nudgeFullRate(); setUnboundedDrag() through SDL's relative mouse mode (the pointer hidden and held, the
// drag's positions virtual); the cursor polled from Panel::cursor() onto SDL's system cursors (grab and grabbing show
// the pointing hand and the move cursor, copy the default arrow: SDL has no open hand, closed hand or copy cursor);
// services() = menus | fileChooser | clipboard: menus drawn by the Canvas in the same window over the Panel's frame
// (desktop/MenuOverlay.h), file choosers through SDL's dialogs (the system's own; the callback always runs on the
// main thread, from the loop), the clipboard's text through SDL. showParamMenu() does nothing (no plug-in host);
// announce() and a11yFocus() do nothing: this release has no accessibility mirror (README, "Desktop").
//
// Preferences. The first open() of a process whose Options::prefsFile is true gives UiPreferences a file store
// (UiPreferences::setBackend; desktop/PrefsFile.h has the format): "preferences.txt" in <ENV_PREFIX>PREFS_DIR when
// that variable is set (a test's sandbox), else in SDL's preferences folder for the application FUNKGUI_PREFS_FOLDER
// (and the organisation Options::prefsOrganisation, a folder above it when it is not empty): on macOS
// ~/Library/Application Support/<folder>/, on Windows %APPDATA%\<folder>\, on Linux ~/.local/share/<folder>/
// (or $XDG_DATA_HOME). An application with a store of its own sets prefsFile false.
//
// Threads: everything here is the main thread's (the thread that called open(); on macOS and Windows that must be the
// process's main thread, SDL's rule), except requestFrame().
//
// Teardown: close() (and the destructor) lets the menus and the pending chooser go unanswered, closes the Panel's
// gestures, releases the window from BgfxContext (which shuts bgfx down when it was the last), then destroys the
// window. The Panel is not touched after that.
//
// Not here in this release (README, "Desktop", lists each): a second window, popup windows, the system menu bar,
// accessibility, input methods, drag and drop, Wayland.

#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Panel.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct SDL_Window;
union SDL_Event;

namespace funkgui::desktop
{
    struct Options
    {
        std::string title;                           // the window's title; empty: FUNKGUI_PRODUCT_NAME

        // The theme: an index pins it (Theme::byIndex; the preference is then neither read nor followed), -1 follows
        // UiPreferences::theme(), as EditorHost does without its capture pin.
        int themeIndex = -1;

        bool resizable = true;                       // a resizable window, only if the Panel says resizable()
        int  maxWidth = 0, maxHeight = 0;            // a resizable Panel's largest logical size; 0: no limit

        // Called on the main thread with every SDL event before the host reads it; true: the host ignores it. How an
        // application takes key-up, gamepad and joystick events, which a Panel's structs do not carry. Not called
        // from the event watch (a live resize's frames), and never during a frame.
        std::function<bool(const SDL_Event&)> rawEvent;

        // Called once in every frame that ticks the Panel, on the main thread, just before Panel::tick (WebHost's
        // beforeTick): where an application brings in what the frame is about to show. It must not close the host.
        std::function<void()> beforeTick;

        // UI zoom, as EditorConfig's: no steps = no zoom, the window is the Panel's size.
        std::vector<int> zoomSteps;                  // percent, ascending; values outside 25..400 are dropped
        int         defaultZoomPercent = 100;        // the step for a missing or unlisted preference
        const char* zoomPrefKey = nullptr;           // UiPreferences int key; nullptr = not persisted

        // The frames wait for the display (the swap chain's vsync). false: bgfx::frame() never waits and the loop's
        // own clock paces them (a test, a benchmark).
        bool vsync = true;

        // A pinned clock for tests and captures (EditorHost's UI_FIXED_DT): > 0, every tick gets this dt, every frame
        // is drawn, nowSeconds() is the sum of the dts, and pump() draws a frame at every turn without waiting for
        // the cadence (with vsync on, bgfx::frame() still waits for the display).
        float fixedDt = 0.0f;

        bool hidden = false;                         // the window is made hidden and stays so (tests)

        // The preferences' file store (above), and the organisation's folder above the application's ("": none).
        bool        prefsFile = true;
        std::string prefsOrganisation;

        std::function<void(bool)> setUiAttached;     // true from open(), false from close(): a telemetry gate
        std::function<void()> beginBatch, endBatch;  // HostServices::beginBatch/endBatch; empty = no-op
    };

    class DesktopHost final : public HostServices
    {
    public:
        // Attaches nothing and opens nothing: open() does.
        DesktopHost(Panel&, Options);
        ~DesktopHost() override;

        DesktopHost(const DesktopHost&) = delete;
        DesktopHost& operator=(const DesktopHost&) = delete;

        // SDL's video subsystem, the window and bgfx on it; then the Panel is attached. False, with `why` saying what
        // failed in words (no display, no Vulkan driver, ...), on any failure: nothing is left open then.
        bool open(std::string& why);
        bool isOpen() const noexcept;

        // The loop, until the window closes or close() is called: pump() over and over. The process's exit code: 0.
        int run();

        // One turn of the loop, for an application that owns its own: waits for events as long as the cadence allows
        // (not at all at full rate), delivers them, runs Panel::idle when due and draws a frame when one is due. False
        // once the window is closed (the user closed it, SDL's quit event arrived, or close() was called).
        bool pump();

        // Any thread: wakes the loop and has it draw a frame at full rate (HostServices::nudgeFullRate()'s effect).
        void requestFrame();

        // Closes the window and lets bgfx go (the header comment, "Teardown"). pump() answers false afterwards.
        void close();

        SDL_Window* window() const noexcept;

        // The last frame recorded, drawn again by the GPU into an offscreen target of the window's size in pixels and
        // read back as straight RGBA8, row-major from the top-left, opaque (tests, screenshots): the same draw calls,
        // programs and textures as the window's frame. A menu showing is in it. False with `why` before the first
        // frame, where bgfx cannot read a texture back, or when the read fails.
        bool capture(std::vector<std::uint8_t>& rgba, int& w, int& h, std::string& why);

        struct Diagnostics
        {
            uint32_t frames = 0;                     // frames drawn and submitted
            uint32_t skipped = 0;                    // nothing recorded: needsDraw() false and no reason to draw
            uint32_t unchanged = 0;                  // recorded, equal to the last frame drawn: not submitted
            uint32_t overflows = 0;                  // frames BgfxSink dropped (the transient buffer was full)
            uint32_t ticks = 0;                      // Panel::tick calls
            float    fps = 0.0f;                     // the loop's measured frame rate
            double   scale = 0.0;                    // physical px per logical px the last frame was drawn at
            double   displayScale = 0.0;             // SDL's display scale of the window (the content scale)
            int      zoomPercent = 100;              // the zoom the window is sized and drawn at
            int      physW = 0, physH = 0;           // the drawable of the last frame, px
            int      logicalW = 0, logicalH = 0;     // the Panel's size at the last frame
            std::string renderer;                    // bgfx's name for it ("Metal", "Vulkan", "Direct3D 11")
            std::string videoDriver;                 // SDL's ("cocoa", "windows", "x11")
        };
        Diagnostics diagnostics() const;

        // HostServices (the header comment says what each does here).
        void   setUnboundedDrag(bool on) override;
        void   showParamMenu(ParamPort&, float x, float y) override;
        void   nudgeFullRate() override;
        double nowSeconds() const override;
        void   beginBatch() override;
        void   endBatch() override;
        int    themeIndex() const override;
        int    zoomPercent() const override;
        void   setZoomPercent(int percent) override;
        std::span<const int> zoomSteps() const override;
        bool   zoomFits(int percent) const override;
        unsigned services() const override;
        bool   showMenu(const MenuRequest&, MenuCallback) override;
        void   dismissMenus() override;
        bool   chooseFiles(const FileRequest&, FilesCallback) override;
        bool   copyText(std::string_view utf8) override;
        std::string clipboardText() const override;
        bool   commandKeyIsMeta() const override;
        void   announce(std::string_view text, Announce priority) override;   // nothing: no accessibility mirror
        void   a11yFocus(uint32_t itemId) override;                           // nothing, likewise

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}

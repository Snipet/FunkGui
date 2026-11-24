// FUNKGUI_TEST name=fg.desktop.host timeout=300 gpu=1 links=desktop
//
// fg.desktop.host (v0.28.0): DesktopHost on a real window and the real GPU path (Metal, Direct3D 11, or Vulkan on
// X11), the one fg.desktop test that needs a display. On Linux it runs only when DISPLAY is set (an X server: a
// desktop's, or Xvfb with Mesa's software Vulkan, as FunkGui's CI runs it) and otherwise skips every row with a note,
// so the suite also passes on a machine with no display; on macOS and Windows it always runs. The window is made
// hidden, the clock is pinned (1/60 s a frame, every frame drawn) and vsync is off.
//   open.*      open() succeeds, attaches the Panel, reports the platform's renderer and SDL's video driver
//   frame.*     pump() ticks and draws: the Panel's tick and draw counts, the frame's size and dpi, the diagnostics
//   capture.*   the frame read back from the GPU: its size, opaque, and the Panel's rectangles at their places in
//               their colours (within 2 of 255 a channel), the theme's ground between them
//   raster.*    the same frame through SoftRaster: the share of pixels that differ by more than 16 in a channel is
//               under 2 % (the GPU's edges are one sample a pixel, the CPU's two)
//   input.*     SDL events pushed to the window reach the Panel as its structs: a move, a press, a drag and a release
//               in logical px with the button and the click count, a right press as a popup, a wheel notch in JUCE's
//               units, a named key, a chord, typed text
//   services.*  the clipboard's text written and read back; themeIndex(); nowSeconds() is the pinned clock
//   resize.*    a fixed Panel that changes its own size has the window follow, and the next frame is the new size
//   close.*     close() closes the Panel's gestures; pump() answers false after it; a second open() works
// Spec rows only.

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/SoftRaster.h>
#include <funkgui/core/Theme.h>
#include <funkgui/desktop/DesktopHost.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Input.h>
#include <funkgui/panel/Panel.h>
#include <funkgui/test/Harness.h>
#include <funkgui/text/FontService.h>

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace T = funkgui::test;
using funkgui::Col;

namespace
{
    constexpr Col kRed{ 255, 0, 0, 255 }, kGreen{ 0, 255, 0, 255 }, kBlue{ 0, 0, 255, 255 };

    // A fixed Panel that draws three opaque rectangles and remembers what it was given.
    class TestPanel final : public funkgui::Panel
    {
    public:
        int w = 320, h = 200;
        int ticks = 0, draws = 0, closes = 0, exits = 0;
        float lastDt = 0.0f;
        funkgui::HostServices* host = nullptr;
        std::vector<std::string> events;             // one line an input event

        void attach(funkgui::HostServices& s) override { host = &s; }
        int  width() const override { return w; }
        int  height() const override { return h; }
        void tick(float dt) override
        {
            ++ticks;
            lastDt = dt;
        }
        void draw(funkgui::Canvas& c, const funkgui::Theme&) override
        {
            ++draws;
            c.rrect(20.0f, 20.0f, 80.0f, 60.0f, 0.0f, kRed);
            c.rrect(120.0f, 20.0f, 80.0f, 60.0f, 0.0f, kGreen);
            c.rrect(20.0f, 100.0f, 180.0f, 60.0f, 0.0f, kBlue);
        }
        bool wantsFullRate() const override { return false; }
        void accessibility(std::vector<funkgui::A11yItem>&) const override {}
        uint32_t a11yRevision() const override { return 0; }
        void a11yAction(uint32_t, funkgui::A11yAction, double) override {}
        void closeGestures() override { ++closes; }

        void pointerMove(const funkgui::PointerEvent& e) override { log("move", e); }
        void pointerExit() override { ++exits; }
        void pointerDown(const funkgui::PointerEvent& e) override { log("down", e); }
        void pointerDrag(const funkgui::PointerEvent& e) override { log("drag", e); }
        void pointerUp(const funkgui::PointerEvent& e) override { log("up", e); }
        void doubleClick(const funkgui::PointerEvent& e) override { log("double", e); }
        bool wheel(const funkgui::WheelEvent& e) override
        {
            char line[128];
            std::snprintf(line, sizeof line, "wheel %.1f %.1f dy=%.6f smooth=%d", static_cast<double>(e.x),
                          static_cast<double>(e.y), static_cast<double>(e.dy), e.smooth ? 1 : 0);
            events.emplace_back(line);
            return true;
        }
        bool key(const funkgui::KeyEvent& e) override
        {
            char line[128];
            std::snprintf(line, sizeof line, "key %d ch=%u cmd=%d shift=%d", static_cast<int>(e.key),
                          static_cast<unsigned>(e.ch), e.mods.cmd ? 1 : 0, e.mods.shift ? 1 : 0);
            events.emplace_back(line);
            return true;
        }

    private:
        void log(const char* what, const funkgui::PointerEvent& e)
        {
            char line[128];
            std::snprintf(line, sizeof line, "%s %.1f %.1f button=%d clicks=%d popup=%d", what,
                          static_cast<double>(e.x), static_cast<double>(e.y), static_cast<int>(e.button), e.clicks,
                          e.popup ? 1 : 0);
            events.emplace_back(line);
        }
    };

    const uint8_t* px(const std::vector<uint8_t>& rgba, int w, int x, int y)
    {
        return &rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 4u];
    }

    bool isCol(const uint8_t* p, Col c, int tol = 2)
    {
        return std::abs(p[0] - c.r) <= tol && std::abs(p[1] - c.g) <= tol && std::abs(p[2] - c.b) <= tol && p[3] == 255;
    }

    bool has(const TestPanel& panel, const std::string& line)
    {
        for (const std::string& e : panel.events)
            if (e == line)
                return true;
        return false;
    }

    void pump(funkgui::desktop::DesktopHost& host, int turns)
    {
        for (int i = 0; i < turns && host.pump(); ++i)
            host.nudgeFullRate();
    }

    // A window coordinate for a logical one (the window's units per logical px, from its size).
    float toWindow(SDL_Window* window, float logical, int logicalSize, bool vertical)
    {
        int ww = 0, wh = 0;
        SDL_GetWindowSize(window, &ww, &wh);
        return logical * static_cast<float>(vertical ? wh : ww) / static_cast<float>(logicalSize);
    }
}

int main(int argc, char** argv)
{
    T::Probe P("fg.desktop.host", "", argc, argv);

#if defined(__linux__)
    const char* display = std::getenv("DISPLAY");    // a test's own check: is there an X server to open a window on
    if (display == nullptr || *display == 0)
    {
        std::printf("NOTE     no X display (DISPLAY is not set): every row is skipped\n");
        P.note("skipped", "\"no display\"");
        return P.finish();
    }
#endif

    TestPanel panel;
    funkgui::desktop::Options options;
    options.title = "fg.desktop.host";
    options.themeIndex = 0;
    options.hidden = true;
    options.vsync = false;
    options.fixedDt = 1.0f / 60.0f;
    options.prefsFile = false;
    int attached = 0;
    options.setUiAttached = [&attached](bool on) { attached += on ? 1 : -1; };
    funkgui::desktop::DesktopHost host(panel, options);

    // ---- open -------------------------------------------------------------------------------------------------------
    std::string why;
    const bool opened = host.open(why);
    if (!opened)
        std::printf("NOTE     open() failed: %s\n", why.c_str());
    P.eq("open.ok", opened, 1);
    if (!opened)
        return P.finish();
    P.eq("open.attached_panel", panel.host == static_cast<funkgui::HostServices*>(&host), 1);
    P.eq("open.ui_attached", attached, 1);
    P.eq("open.window", host.window() != nullptr && host.isOpen(), 1);
    P.eq("open.again_is_open", host.open(why), 1);
    {
        const std::string renderer = host.diagnostics().renderer, video = host.diagnostics().videoDriver;
        std::printf("NOTE     renderer %s, video driver %s\n", renderer.c_str(), video.c_str());
#if defined(__APPLE__)
        P.eq("open.renderer", renderer == "Metal", 1);
        P.eq("open.video_driver", video == "cocoa", 1);
#elif defined(_WIN32)
        P.eq("open.renderer", renderer == "Direct3D 11", 1);
        P.eq("open.video_driver", video == "windows", 1);
#else
        P.eq("open.renderer", renderer == "Vulkan", 1);
        P.eq("open.video_driver", video == "x11", 1);
#endif
    }

    // ---- frames -----------------------------------------------------------------------------------------------------
    pump(host, 4);
    funkgui::desktop::DesktopHost::Diagnostics d = host.diagnostics();
    P.ge("frame.ticks", panel.ticks, 3);
    P.eq("frame.every_tick_drawn", panel.draws == panel.ticks && static_cast<int>(d.frames) == panel.draws, 1);
    P.near("frame.dt_is_pinned", static_cast<double>(panel.lastDt), 1.0 / 60.0, 1.0e-7);
    P.eq("frame.logical_size", d.logicalW == 320 && d.logicalH == 200, 1);
    P.eq("frame.has_pixels", d.physW >= 1 && d.physH >= 1, 1);
    P.near("frame.dpi_is_pixels_per_logical", d.scale, static_cast<double>(d.physH) / 200.0, 1.0e-6);
    P.eq("frame.no_overflow", static_cast<int64_t>(d.overflows), 0);
    std::printf("NOTE     %d x %d px for 320 x 200 logical, display scale %.3f\n", d.physW, d.physH, d.displayScale);

    // ---- capture ----------------------------------------------------------------------------------------------------
    std::vector<uint8_t> rgba;
    int cw = 0, ch = 0;
    const bool captured = host.capture(rgba, cw, ch, why);
    if (!captured)
        std::printf("NOTE     capture() failed: %s\n", why.c_str());
    P.eq("capture.ok", captured, 1);
    if (captured)
    {
        P.eq("capture.size", cw == d.physW && ch == d.physH && rgba.size() == static_cast<std::size_t>(cw) * ch * 4u,
             1);
        const double sx = static_cast<double>(cw) / 320.0, sy = static_cast<double>(ch) / 200.0;
        const auto at = [&](double lx, double ly)
        {
            return px(rgba, cw, static_cast<int>(lx * sx), static_cast<int>(ly * sy));
        };
        const Col ground = funkgui::Theme::byIndex(0).ground;
        P.eq("capture.red", isCol(at(60, 50), kRed), 1);
        P.eq("capture.green", isCol(at(160, 50), kGreen), 1);
        P.eq("capture.blue", isCol(at(110, 130), kBlue), 1);
        P.eq("capture.ground_between", isCol(at(110, 50), Col{ ground.r, ground.g, ground.b, 255 }), 1);
        P.eq("capture.ground_corner", isCol(at(310, 190), Col{ ground.r, ground.g, ground.b, 255 }), 1);
        bool opaque = true;
        for (std::size_t i = 3; opaque && i < rgba.size(); i += 4u)
            opaque = rgba[i] == 255;
        P.eq("capture.opaque", opaque, 1);

        // The same frame on the CPU.
        funkgui::Canvas canvas(funkgui::FontService::get().atlas());
        funkgui::FrameInfo info;
        info.logicalW = 320;
        info.logicalH = 200;
        info.dpi = static_cast<float>(d.scale);
        info.clear = ground;
        canvas.begin(info);
        panel.draw(canvas, funkgui::Theme::byIndex(0));
        --panel.draws;                               // the test's own draw
        const funkgui::Image cpu = funkgui::rasterise(canvas.end(), funkgui::FontService::get().atlas(), 2);
        if (cpu.w == cw && cpu.h == ch)
        {
            std::size_t off = 0;
            for (std::size_t i = 0; i < rgba.size(); i += 4u)
                for (std::size_t k = 0; k < 3u; ++k)
                    if (std::abs(static_cast<int>(rgba[i + k]) - static_cast<int>(cpu.rgba[i + k])) > 16)
                    {
                        ++off;
                        break;
                    }
            const double share = static_cast<double>(off) / static_cast<double>(rgba.size() / 4u);
            std::printf("NOTE     %.4f %% of the pixels differ from SoftRaster's by more than 16\n", share * 100.0);
            P.le("raster.share_off", share, 0.02);
        }
        else
            std::printf("NOTE     SoftRaster's frame is %d x %d, the GPU's %d x %d: not compared\n", cpu.w, cpu.h, cw, ch);
    }

    // ---- input ------------------------------------------------------------------------------------------------------
    {
        SDL_Window* window = host.window();
        const SDL_WindowID id = SDL_GetWindowID(window);
        const auto wx = [&](float lx) { return toWindow(window, lx, 320, false); };
        const auto wy = [&](float ly) { return toWindow(window, ly, 200, true); };
        const auto push = [](SDL_Event e) { SDL_PushEvent(&e); };
        const auto motion = [&](float lx, float ly)
        {
            SDL_Event e{};
            e.type = SDL_EVENT_MOUSE_MOTION;
            e.motion.windowID = id;
            e.motion.x = wx(lx);
            e.motion.y = wy(ly);
            push(e);
        };
        const auto button = [&](bool down, int which, float lx, float ly, int clicks)
        {
            SDL_Event e{};
            e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
            e.button.windowID = id;
            e.button.button = static_cast<Uint8>(which);
            e.button.down = down;
            e.button.clicks = static_cast<Uint8>(clicks);
            e.button.x = wx(lx);
            e.button.y = wy(ly);
            push(e);
        };
        const auto key = [&](SDL_Keycode code, SDL_Keymod mod)
        {
            SDL_Event e{};
            e.type = SDL_EVENT_KEY_DOWN;
            e.key.windowID = id;
            e.key.key = code;
            e.key.mod = mod;
            e.key.down = true;
            push(e);
        };

        panel.events.clear();
        motion(40.0f, 30.0f);
        button(true, SDL_BUTTON_LEFT, 40.0f, 30.0f, 1);
        motion(90.0f, 70.0f);
        button(false, SDL_BUTTON_LEFT, 90.0f, 70.0f, 1);
        pump(host, 2);
        P.eq("input.move", has(panel, "move 40.0 30.0 button=0 clicks=1 popup=0"), 1);
        P.eq("input.down", has(panel, "down 40.0 30.0 button=0 clicks=1 popup=0"), 1);
        P.eq("input.drag", has(panel, "drag 90.0 70.0 button=0 clicks=1 popup=0"), 1);
        P.eq("input.up", has(panel, "up 90.0 70.0 button=0 clicks=1 popup=0"), 1);

        panel.events.clear();
        button(true, SDL_BUTTON_LEFT, 50.0f, 50.0f, 2);
        button(false, SDL_BUTTON_LEFT, 50.0f, 50.0f, 2);
        pump(host, 2);
        P.eq("input.double_click_after_up", panel.events.size() == 3 && panel.events[1] == "up 50.0 50.0 button=0 clicks=2 popup=0"
                                                && panel.events[2] == "double 50.0 50.0 button=0 clicks=2 popup=0", 1);

        panel.events.clear();
        button(true, SDL_BUTTON_RIGHT, 60.0f, 40.0f, 1);
        button(true, SDL_BUTTON_LEFT, 60.0f, 40.0f, 1);      // a second button during a press is ignored
        button(false, SDL_BUTTON_LEFT, 60.0f, 40.0f, 1);
        button(false, SDL_BUTTON_RIGHT, 60.0f, 40.0f, 1);
        button(true, SDL_BUTTON_MIDDLE, 10.0f, 10.0f, 1);
        button(false, SDL_BUTTON_MIDDLE, 10.0f, 10.0f, 1);
        button(true, SDL_BUTTON_X1, 10.0f, 10.0f, 1);        // the back button is no press
        button(false, SDL_BUTTON_X1, 10.0f, 10.0f, 1);
        pump(host, 2);
        P.eq("input.right_is_popup", has(panel, "down 60.0 40.0 button=2 clicks=1 popup=1"), 1);
        P.eq("input.middle", has(panel, "down 10.0 10.0 button=1 clicks=1 popup=0"), 1);
        P.eq("input.one_press_at_a_time", static_cast<int64_t>(panel.events.size()), 4);

        panel.events.clear();
        {
            SDL_Event e{};
            e.type = SDL_EVENT_MOUSE_WHEEL;
            e.wheel.windowID = id;
            e.wheel.y = 1.0f;
            e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
            e.wheel.mouse_x = wx(100.0f);
            e.wheel.mouse_y = wy(80.0f);
            push(e);
        }
        key(SDLK_ESCAPE, SDL_KMOD_NONE);
        key(SDLK_Z, host.commandKeyIsMeta() ? SDL_KMOD_LGUI : SDL_KMOD_LCTRL);
        key(SDLK_A, SDL_KMOD_NONE);                  // a plain letter is text's, not a key's
        {
            static const char text[] = "a\xC3\xA9 ";         // a, e acute, and a space (the key's, not text's)
            SDL_Event e{};
            e.type = SDL_EVENT_TEXT_INPUT;
            e.text.windowID = id;
            e.text.text = text;
            push(e);
        }
        pump(host, 2);
#if defined(__APPLE__)
        P.eq("input.wheel", has(panel, "wheel 100.0 80.0 dy=0.019531 smooth=1"), 1);
#else
        P.eq("input.wheel", has(panel, "wheel 100.0 80.0 dy=0.195312 smooth=0"), 1);
#endif
        P.eq("input.named_key", has(panel, "key 10 ch=0 cmd=0 shift=0"), 1);              // Key::escape
        P.eq("input.chord", has(panel, "key 0 ch=122 cmd=1 shift=0"), 1);                 // z with the command key
        P.eq("input.text", has(panel, "key 0 ch=97 cmd=0 shift=0") && has(panel, "key 0 ch=233 cmd=0 shift=0"), 1);
        P.eq("input.count", static_cast<int64_t>(panel.events.size()), 5);
    }

    // ---- services ---------------------------------------------------------------------------------------------------
    P.eq("services.clipboard_bit", (host.services() & funkgui::hostservice::clipboard) != 0, 1);
    {
        const std::string before = host.clipboardText();
        const bool wrote = host.copyText("fg.desktop.host \xE2\x88\x92 1");
        const std::string read = host.clipboardText();
        if (!wrote || read != "fg.desktop.host \xE2\x88\x92 1")
            std::printf("NOTE     the clipboard: wrote %d, read '%s'\n", wrote ? 1 : 0, read.c_str());
        P.eq("services.clipboard_round_trip", wrote && read == "fg.desktop.host \xE2\x88\x92 1", 1);
        host.copyText(before);                       // what the machine's clipboard held
    }
    P.eq("services.theme_index", host.themeIndex(), 0);
    P.near("services.now_is_the_pinned_clock", host.nowSeconds(), static_cast<double>(panel.ticks) / 60.0, 1.0e-4);
    P.eq("services.zoom_100", host.zoomPercent(), 100);

    // ---- a fixed Panel that changes its size ------------------------------------------------------------------------
    panel.w = 400;
    panel.h = 240;
    pump(host, 6);
    d = host.diagnostics();
    P.eq("resize.logical_follows", d.logicalW == 400 && d.logicalH == 240, 1);
    std::printf("NOTE     after the Panel grew: %d x %d px, scale %.3f\n", d.physW, d.physH, d.scale);
    if (host.capture(rgba, cw, ch, why))
        P.eq("resize.captured_at_the_new_size", cw == d.physW && ch == d.physH, 1);
    else
        P.eq("resize.captured_at_the_new_size", 0, 1);

    // ---- close ------------------------------------------------------------------------------------------------------
    const int ticksBefore = panel.ticks;
    host.close();
    P.eq("close.gestures_closed", panel.closes, 1);
    P.eq("close.ui_detached", attached, 0);
    P.eq("close.not_open", !host.isOpen() && host.window() == nullptr, 1);
    P.eq("close.pump_is_false", host.pump(), 0);
    P.eq("close.no_tick_after", panel.ticks, ticksBefore);
    const bool reopened = host.open(why);
    P.eq("close.opens_again", reopened, 1);
    if (reopened)
    {
        pump(host, 3);
        P.ge("close.ticks_again", panel.ticks, ticksBefore + 2);
        host.close();
    }
    return P.finish();
}

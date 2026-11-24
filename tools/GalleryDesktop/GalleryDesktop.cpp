// funkgui_gallery_desktop: FunkGui's gallery in a desktop window with no JUCE (v0.28.0). One GalleryPanel section
// (test/gallery: the sections GalleryProbe runs headless, FunkGuiGalleryApp on the GPU under JUCE and
// funkgui_gallery_web in a browser) live in a DesktopHost: SDL3's window, bgfx on Metal, Direct3D 11 or Vulkan. It is
// what a person runs to see and try FunkGui::desktop, and what a machine runs to prove a platform draws.
//
//   funkgui_gallery_desktop [--section <name>] [--theme <0|1>] [--frames <n>] [--shot <file.png>] [--info] [--list]
//                           [--resize]
//
// - --section: a gallery section, "primitives" when it is not given (else the first registered one). In the window,
//   F1 and F2 show the section before and after it and F3 the other theme; the window takes each section's size.
// - --theme: pins the theme; without it the theme is the preference (F3 writes it).
// - --frames n: for a machine. Runs n frames at full rate on a pinned clock (1/60 s a frame, every frame drawn), then
//   exits 0.
// - --shot file.png: when the frames are done (120 without --frames), writes the last frame, read back from the GPU
//   (DesktopHost::capture), as a PNG, and exits. Exit 1 when the frame cannot be read back or the file not written.
// - --info: opens the window hidden, prints funkgui::version(), funkgui::gpuInfo(), the renderer, the window's size
//   and scale, one line each, and exits 0.
// - --list: the sections' names, one a line.
// - --resize: in place of a section (which has one fixed size), a resizable Panel made for looking at a live resize:
//   a frame along the window's edges, a block in each corner, a grid from the top left, and the Panel's size in
//   logical px as text in the middle. While the window's edge is dragged the frame should stay on the edge, the
//   blocks in the corners, and the text should say the size without lagging. F3 still changes the theme.
// A clean exit prints one line, "fg.desktop: ok renderer=<name> frames=<n>". A failure to open prints why and exits 1.

#include "../../test/gallery/GalleryPanel.h"

#include <funkgui/canvas/SoftRaster.h>
#include <funkgui/core/Theme.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/core/Version.h>
#include <funkgui/desktop/DesktopHost.h>
#include <funkgui/gpu/GpuInfo.h>
#include <funkgui/prefs/UiPreferences.h>

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace G = funkgui::gallery;

namespace
{
    constexpr const char* kDefaultSection = "primitives";

    struct Args
    {
        std::string section;
        std::string shot;
        int  theme = -1;
        long frames = 0;
        bool info = false, list = false, resize = false, bad = false;
    };

    // --resize: a Panel of any size from 240 x 160 up, which draws its own bounds (the top of this file).
    class ResizePanel final : public funkgui::Panel
    {
    public:
        void attach(funkgui::HostServices&) override {}
        int  width() const override { return w_; }
        int  height() const override { return h_; }
        void tick(float) override {}
        bool wantsFullRate() const override { return false; }
        void accessibility(std::vector<funkgui::A11yItem>&) const override {}
        uint32_t a11yRevision() const override { return 0; }
        void a11yAction(uint32_t, funkgui::A11yAction, double) override {}
        void closeGestures() override {}
        bool resizable() const override { return true; }
        int  minWidth() const override { return 240; }
        int  minHeight() const override { return 160; }
        void resize(int w, int h) override
        {
            w_ = w;
            h_ = h;
            ++resizes_;
        }

        void draw(funkgui::Canvas& c, const funkgui::Theme& th) override
        {
            const float w = static_cast<float>(w_), h = static_cast<float>(h_);
            for (float x = 40.0f; x < w; x += 40.0f)             // the grid: fixed from the top left
                c.hairlineV(x, 0.0f, h, th.ink16);
            for (float y = 40.0f; y < h; y += 40.0f)
                c.hairlineH(0.0f, y, w, th.ink16);
            c.rrect(4.0f, 4.0f, w - 8.0f, h - 8.0f, 6.0f, funkgui::Col{ 0, 0, 0, 0 }, 2.0f, th.accent);   // the frame
            constexpr float k = 18.0f;                           // a block in each corner
            c.rrect(0.0f, 0.0f, k, k, 0.0f, th.ink100);
            c.rrect(w - k, 0.0f, k, k, 0.0f, th.ink100);
            c.rrect(0.0f, h - k, k, k, 0.0f, th.ink100);
            c.rrect(w - k, h - k, k, k, 0.0f, th.ink100);
            char size[64];
            std::snprintf(size, sizeof size, "%d x %d", w_, h_);
            c.text(size, w * 0.5f, c.capCentreTop(h * 0.5f - 10.0f, funkgui::type::kValueP), funkgui::type::kValueP,
                   th.ink100, funkgui::Align::centre);
            std::snprintf(size, sizeof size, "RESIZED %d TIMES", resizes_);
            c.text(size, w * 0.5f, c.capCentreTop(h * 0.5f + 22.0f, funkgui::type::kLabel), funkgui::type::kLabel,
                   th.ink52, funkgui::Align::centre);
        }

    private:
        int w_ = 480, h_ = 300, resizes_ = 0;
    };

    Args parse(int argc, char** argv)
    {
        Args a;
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            const bool more = i + 1 < argc;
            if (arg == "--section" && more)
                a.section = argv[++i];
            else if (arg == "--shot" && more)
                a.shot = argv[++i];
            else if (arg == "--theme" && more)
                a.theme = std::atoi(argv[++i]);
            else if (arg == "--frames" && more)
                a.frames = std::atol(argv[++i]);
            else if (arg == "--info")
                a.info = true;
            else if (arg == "--list")
                a.list = true;
            else if (arg == "--resize")
                a.resize = true;
            else
                a.bad = true;
        }
        if (a.frames < 0 || a.theme >= funkgui::Theme::kCount)
            a.bad = true;
        return a;
    }

    std::string titleOf(const G::SectionInfo& s)
    {
        return "FunkGui gallery: " + s.name + "   (F1/F2: section, F3: theme)";
    }
}

int main(int argc, char** argv)
{
    const Args args = parse(argc, argv);
    if (args.bad)
    {
        std::fprintf(stderr, "usage: funkgui_gallery_desktop [--section <name>] [--theme <0|1>] [--frames <n>] "
                             "[--shot <file.png>] [--info] [--list] [--resize]\n");
        return 2;
    }
    const std::vector<G::SectionInfo>& sections = G::sections();
    if (args.list)
    {
        for (const G::SectionInfo& s : sections)
            std::printf("%s\n", s.name.c_str());
        return 0;
    }
    const std::string wanted = !args.section.empty() ? args.section
                             : G::findSection(kDefaultSection) != nullptr || sections.empty()
                                 ? std::string(kDefaultSection) : sections.front().name;
    const G::SectionInfo* section = G::findSection(wanted);
    if (section == nullptr)
    {
        std::fprintf(stderr, "funkgui_gallery_desktop: no gallery section '%s' (--list names them)\n", wanted.c_str());
        return 2;
    }

    G::GalleryPanel panel(*section);
    ResizePanel resizePanel;
    funkgui::Panel& shown = args.resize ? static_cast<funkgui::Panel&>(resizePanel) : panel;
    const bool machine = args.frames > 0 || !args.shot.empty();
    const long frames = args.frames > 0 ? args.frames : 120;

    funkgui::desktop::Options options;
    options.title = args.resize ? std::string("FunkGui gallery: resize   (drag the window's edges; F3: theme)")
                                : titleOf(*section);
    options.themeIndex = args.theme;
    options.hidden = args.info;
    options.fixedDt = machine ? 1.0f / 60.0f : 0.0f;
    options.zoomSteps = { 100, 125, 150, 175 };      // the other gallery apps' steps and key
    options.zoomPrefKey = "uiZoom";

    funkgui::desktop::DesktopHost* hostPtr = nullptr;
    // F1, F2 and F3 are the gallery's own: every other event is the host's.
    options.rawEvent = [&](const SDL_Event& e)
    {
        if (e.type != SDL_EVENT_KEY_DOWN || e.key.repeat)
            return false;
        if ((e.key.key == SDLK_F1 || e.key.key == SDLK_F2) && !args.resize)
        {
            std::size_t at = 0;
            for (std::size_t i = 0; i < sections.size(); ++i)
                if (&sections[i] == &panel.info())
                    at = i;
            const std::size_t n = sections.size();
            const G::SectionInfo& next = sections[(at + (e.key.key == SDLK_F2 ? 1u : n - 1u)) % n];
            panel.show(next);
            if (hostPtr != nullptr)
            {
                SDL_SetWindowTitle(hostPtr->window(), titleOf(next).c_str());
                hostPtr->nudgeFullRate();
            }
            return true;
        }
        if (e.key.key == SDLK_F3)
        {
            auto& prefs = funkgui::UiPreferences::get();
            prefs.setTheme((prefs.theme() + 1) % funkgui::Theme::kCount);
            if (hostPtr != nullptr)
                hostPtr->nudgeFullRate();
            return true;
        }
        return false;
    };

    funkgui::desktop::DesktopHost host(shown, std::move(options));
    hostPtr = &host;
    std::string why;
    if (!host.open(why))
    {
        std::fprintf(stderr, "funkgui_gallery_desktop: %s\n", why.c_str());
        return 1;
    }

    if (args.info)
    {
        for (int i = 0; i < 3 && host.pump(); ++i)   // a frame or two, so the sizes are the drawn ones
            host.nudgeFullRate();
        const funkgui::GpuInfo gpu = funkgui::gpuInfo();
        const funkgui::desktop::DesktopHost::Diagnostics d = host.diagnostics();
        std::printf("funkgui %s\n", funkgui::version());
        std::printf("renderer %s\n", d.renderer.c_str());
        std::printf("gpu running=%d vendor=0x%04x device=0x%04x maxTexture=%u instancing=%d transientVb=%u "
                    "imageTextures=%u customShaders=%u gpuPictures=%d\n",
                    gpu.running ? 1 : 0, static_cast<unsigned>(gpu.vendorId), static_cast<unsigned>(gpu.deviceId),
                    static_cast<unsigned>(gpu.maxTextureSize), gpu.instancing ? 1 : 0,
                    static_cast<unsigned>(gpu.transientVbBytes), static_cast<unsigned>(gpu.imageTextures),
                    static_cast<unsigned>(gpu.customShaders), gpu.gpuPictures ? 1 : 0);
        std::printf("video %s\n", d.videoDriver.c_str());
        std::printf("window logical=%dx%d pixels=%dx%d scale=%.3f displayScale=%.3f zoom=%d\n", d.logicalW,
                    d.logicalH, d.physW, d.physH, d.scale, d.displayScale, d.zoomPercent);
        host.close();
        std::printf("fg.desktop: ok renderer=%s frames=%u\n", d.renderer.c_str(), static_cast<unsigned>(d.frames));
        return 0;
    }

    int code = 0;
    if (machine)
    {
        while (static_cast<long>(host.diagnostics().ticks) < frames && host.pump())
            host.nudgeFullRate();                    // full rate: a machine does not wait for the idle cadence
        if (!args.shot.empty() && host.isOpen())
        {
            funkgui::Image img;
            if (!host.capture(img.rgba, img.w, img.h, why))
            {
                std::fprintf(stderr, "funkgui_gallery_desktop: no picture: %s\n", why.c_str());
                code = 1;
            }
            else if (!funkgui::writePng(img, args.shot.c_str()))
            {
                std::fprintf(stderr, "funkgui_gallery_desktop: could not write %s\n", args.shot.c_str());
                code = 1;
            }
            else
                std::printf("fg.desktop: wrote %s (%d x %d)\n", args.shot.c_str(), img.w, img.h);
        }
    }
    else
        code = host.run();

    const funkgui::desktop::DesktopHost::Diagnostics d = host.diagnostics();
    host.close();
    if (code == 0)
        std::printf("fg.desktop: ok renderer=%s frames=%u\n", d.renderer.c_str(), static_cast<unsigned>(d.frames));
    return code;
}

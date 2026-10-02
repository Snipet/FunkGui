// funkgui_gallery_web: FunkGui's gallery as a web page (v0.13.0; FCompressor ADR-93, docs/sprints/web-c.md G-D). One
// GalleryPanel section (test/gallery: the sections GalleryProbe runs headless and FunkGuiGalleryApp runs on the GPU)
// live in a WebHost on the page's canvas: WebGlSink draws it, the browser's pointer, wheel and keys drive it. It is
// what a person opens to see and try FunkGui::web in a browser; the automated check of WebHost is test/web/host.cpp.
//
//   index.html?section=<name>&theme=<0|1>&zoom=<percent>&dt=<seconds>&scale=<factor>
//
// - section: a gallery section, "primitives" when the parameter is missing (else the first registered one). An unknown
//   name says so on the page and lists the sections. The page's links, one per section, reload the page on another
//   section and keep the other parameters: a host is never rebuilt, and a section's size is fixed for its host's life.
// - theme, zoom, dt and scale fill the host's capture pins (WebHostConfig::capture: uiTheme, uiZoom 25..400, fixedDt
//   in 0..1 s, uiScale in 0.25..8), which a browser cannot take from an environment. Without them the theme and the
//   zoom are UiPreferences' (the zoom steps and their key are the native gallery app's: 100, 125, 150 and 175 %,
//   "uiZoom"), the clock is the browser's and the scale its device pixel ratio.
// - The zoom is fitted to the window less the page around the canvas (what lies left of and above it, and as much
//   again right of and below it).
// - The line under the canvas is the host's diagnostics, twice a second. An uncaught error, an unhandled rejection or
//   an abort is shown under it (index.html), in an element this file never writes: the first one stays.

#include "../../test/gallery/GalleryPanel.h"

#include <funkgui/core/Theme.h>
#include <funkgui/web/WebHost.h>

#include <emscripten/em_js.h>
#include <emscripten/em_macros.h>
#include <emscripten/eventloop.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace G = funkgui::gallery;

EM_JS_DEPS(funkgui_gallery_web_deps, "$UTF8ToString,$stringToUTF8");

// A parameter of the page's query string into `out`; 1 when it is there, 0 (and an empty text) when it is not.
EM_JS(int, fg_gallery_param, (const char* name, char* out, int size), {
    const value = new URLSearchParams(window.location.search).get(UTF8ToString(name));
    stringToUTF8(value === null ? "" : value, out, size);
    return value === null ? 0 : 1;
});

// One link to a section: the page as it is with `section` replaced. The current one is marked.
EM_JS(void, fg_gallery_link, (const char* section, int current), {
    const nav = document.getElementById('funkgui-sections');
    if (!nav) return;
    const name = UTF8ToString(section);
    const params = new URLSearchParams(window.location.search);
    params.set('section', name);
    const link = document.createElement('a');
    link.href = '?' + params.toString();
    link.textContent = name;
    if (current) link.className = 'current';
    nav.appendChild(link);
});

EM_JS(void, fg_gallery_text, (const char* id, const char* text), {
    const element = document.getElementById(UTF8ToString(id));
    if (element) element.textContent = UTF8ToString(text);
});

EM_JS(void, fg_gallery_title, (const char* text), { document.title = UTF8ToString(text); });

// The page around the canvas, in CSS px: what lies left of and above it, twice (the same again on the far sides).
EM_JS(int, fg_gallery_margin, (const char* selector, int vertical), {
    const canvas = document.querySelector(UTF8ToString(selector));
    if (!canvas) return 0;
    const box = canvas.getBoundingClientRect();
    return Math.ceil(2 * (vertical ? box.top + window.scrollY : box.left + window.scrollX));
});

namespace
{
    constexpr const char* kCanvas = "#funkgui-canvas";
    constexpr const char* kDefaultSection = "primitives";

    // The runtime outlives main(): the Panel and its host live for the page's life.
    std::unique_ptr<G::GalleryPanel> panel;
    std::unique_ptr<funkgui::WebHost> host;
    const G::SectionInfo* section = nullptr;

    // A numeric query parameter in [lo, hi], or `otherwise` when it is missing, not a number or out of range.
    double numberParam(const char* name, double lo, double hi, double otherwise)
    {
        char text[64];
        if (fg_gallery_param(name, text, static_cast<int>(sizeof text)) == 0 || text[0] == '\0')
            return otherwise;
        char* end = nullptr;
        const double v = std::strtod(text, &end);
        return end != text && *end == '\0' && v >= lo && v <= hi ? v : otherwise;
    }

    void showStatus(void*)
    {
        const funkgui::WebHost::Diagnostics d = host->diagnostics();
        char text[256];
        if (!host->ok() && !host->sink().lost())
            std::snprintf(text, sizeof text, "NO WEBGL2: %s", host->error());
        else
            std::snprintf(text, sizeof text,
                          "%s   %d x %d   zoom %d %%   scale %.3g   buffer %d x %d   %.1f fps   frames %u%s",
                          section->name.c_str(), panel->width(), panel->height(), d.zoomPercent, d.scale, d.physW,
                          d.physH, static_cast<double>(d.fps), d.frames,
                          host->sink().lost() ? "   CONTEXT LOST" : "");
        fg_gallery_text("funkgui-status", text);
    }
}

int main()
{
    char wanted[64];
    if (fg_gallery_param("section", wanted, static_cast<int>(sizeof wanted)) == 0 || wanted[0] == '\0')
        std::snprintf(wanted, sizeof wanted, "%s",
                      G::findSection(kDefaultSection) != nullptr || G::sections().empty()
                          ? kDefaultSection
                          : G::sections().front().name.c_str());
    section = G::findSection(wanted);
    for (const G::SectionInfo& s : G::sections())
        fg_gallery_link(s.name.c_str(), &s == section ? 1 : 0);
    if (section == nullptr)
    {
        const std::string why = std::string("NO GALLERY SECTION '") + wanted + "': CHOOSE ONE ABOVE";
        fg_gallery_text("funkgui-status", why.c_str());
        return 0;
    }
    fg_gallery_title(("FunkGui gallery: " + section->name).c_str());

    funkgui::WebHostConfig config;
    config.canvasSelector = kCanvas;
    config.zoomSteps = { 100, 125, 150, 175 };       // the native gallery app's (tools/GalleryApp/GalleryApp.cpp)
    config.zoomPrefKey = "uiZoom";
    config.fitMarginX = fg_gallery_margin(kCanvas, 0);
    config.fitMarginY = fg_gallery_margin(kCanvas, 1);
    config.capture.uiTheme = static_cast<int>(numberParam("theme", 0.0, funkgui::Theme::kCount - 1, -1.0));
    config.capture.uiZoom = static_cast<int>(numberParam("zoom", 25.0, 400.0, 0.0));
    config.capture.fixedDt = static_cast<float>(numberParam("dt", 1.0e-6, 1.0, 0.0));
    config.capture.uiScale = static_cast<float>(numberParam("scale", 0.25, 8.0, 0.0));

    panel = std::make_unique<G::GalleryPanel>(*section);
    host = std::make_unique<funkgui::WebHost>(*panel, std::move(config));
    host->start();
    showStatus(nullptr);
    emscripten_set_interval(showStatus, 500.0, nullptr);
    return 0;
}

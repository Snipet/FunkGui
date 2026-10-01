// The browser check of FunkGui::web (v0.12.0; FCompressor ADR-93, docs/sprints/web-b.md G-C). Not a self-registered
// test (this file has no test line): node has no WebGL, so nothing under `verify` can draw with WebGlSink.
// test/web/CMakeLists.txt builds this, in the `web` preset only, into <build>/test/web/{index.html,
// funkgui_web_page.js, funkgui_web_page.wasm}; tools/web/check-page.mjs (CTest fg.web.page, label live) serves that
// directory on 127.0.0.1 and runs it in headless Chrome. Or serve it and open it:
//
//   python3 -m http.server 8137 --bind 127.0.0.1 --directory <build>/test/web      http://127.0.0.1:8137/index.html
//
// The verdict is machine-readable and exact about what ran: document.title is "RUNNING" until the end, then "PASS" or
// "FAIL: <the first thing that failed>"; every line goes to console.log and to the page's <pre>. PASS means all of:
//
// 1. The atlas is the committed bake (FontService, baked from the embedded face).
// 2. A sink on a selector that names no canvas, on a malformed selector and on a canvas that cannot give a WebGL2
//    context (it already holds a 2D one: the nearest a page gets to a browser without WebGL2) reports !ok() with a
//    reason, and submit() refuses; nothing throws or aborts.
// 3. The sink on the page's canvas is ok(): the context was created with the attributes asked for (no alpha, no
//    antialias, no depth, no stencil), the program compiled and linked, the buffer and the atlas texture were built.
// 4. Per case (a theme and a dpi: 1, 1.25, 2), one frame holding primitives of all four kinds (rrect, text, segment,
//    area) and real text from the atlas is drawn by WebGlSink, read back, and compared with SoftRaster's image of the
//    same PrimList at one sample per pixel: the largest channel difference, where it is, the count of samples over
//    kTolerance and the whole distribution are printed, and the frame passes when no sample differs by more than
//    kWorst and at most kOverPerMille per mille of them by more than kTolerance. Each kind is then drawn alone and
//    compared the same way, and must have changed pixels on the canvas, so no kind can be missing behind another.
// 5. WEBGL_lose_context takes the context: the sink sees the loss (lost(), !ok(), submit() == Result::lost, no frame
//    counted, nothing to read back).
// 6. The context is restored: the sink rebuilt its program, buffer and texture (restoreCount() == 1, ok()), and every
//    case draws again, within the same bounds of SoftRaster and byte for byte what it drew before the loss.
// 7. The sink is destroyed and a new one on the same canvas draws the first case byte for byte again.
//
// The bounds are measured, not derived. SoftRaster evaluates in float on the CPU and rounds a frame once at the end; a
// GPU rounds to 8 bits after every primitive, interpolates the varyings itself, filters the atlas with fixed-point
// weights and has its own sqrt, inversesqrt and pow. Measured with Chrome 154 on an Apple M5 (2026-10-01), all frames:
//   ANGLE on Metal (the GPU)             largest difference 1; none over 2; at most 0.5 % of the samples differ by 1
//   ANGLE on SwiftShader (software)      largest difference 8 (text), 6 (segment, area), 1 (rrect); at most 5.1 per
//                                        mille of a frame's samples over 2 (text alone at dpi 1)
// kTolerance 2 is the small tolerance the count is taken over; kWorst 16 is twice the largest difference seen and
// kOverPerMille 10 twice the largest share, so both renderers pass with room, and anything that is wrong rather than
// rounded fails: a missing glyph, another blend or filter, a frame one pixel off (the tie below moved 225 pixels by
// 26 levels when the scene's clip lay on pixel centres).
//
// One difference is outside those bounds and outside the frames, and the page prints it as a note: the fill rule where
// a hard quad edge passes exactly through pixel centres. A quad has such an edge in y only when a clip is not on a
// device pixel: Canvas::pushClip asks for one that is, and no primitive has a hard edge of its own in y (an AREA
// column has them in x, where the rules agree). Metal, and SoftRaster with it, give the tied row to the quad whose top
// edge it is; WebGL's window is y-up, and gives it to the quad whose bottom edge it is. The scene's clip is on a
// device pixel at every dpi of kCases.

#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/Prim.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/canvas/SoftRaster.h>
#include <funkgui/core/Col.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/core/Theme.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/text/FontService.h>
#include <funkgui/web/WebGlSink.h>

#include <emscripten/em_js.h>
#include <emscripten/em_macros.h>
#include <emscripten/eventloop.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using funkgui::Canvas;
using funkgui::Col;
using funkgui::FrameInfo;
using funkgui::Image;
using funkgui::Prim;
using funkgui::PrimList;
using funkgui::Theme;
using funkgui::WebGlSink;

// ---- the page's side of the check, in JavaScript -------------------------------------------------------------------
EM_JS_DEPS(funkgui_web_page_deps, "$UTF8ToString,$stringToUTF8");

EM_JS(void, fg_page_line, (const char* text), {
    const line = UTF8ToString(text);
    console.log(line);
    const pre = document.getElementById('funkgui-log');
    if (pre) pre.textContent += line + '\n';
});

EM_JS(void, fg_page_title, (const char* text), { document.title = UTF8ToString(text); });

// Counts the context events of the canvas (after the sink's own listeners: it registered first).
EM_JS(void, fg_page_watch, (const char* selector), {
    const canvas = document.querySelector(UTF8ToString(selector));
    const page = globalThis.funkguiPage = { canvas: canvas, lost: 0, restored: 0, ext: null };
    canvas.addEventListener('webglcontextlost', () => { page.lost++; });
    canvas.addEventListener('webglcontextrestored', () => { page.restored++; });
});

EM_JS(int, fg_page_lost_events, (), { return globalThis.funkguiPage.lost; });
EM_JS(int, fg_page_restored_events, (), { return globalThis.funkguiPage.restored; });

// The context the sink made (a canvas has one), described into `out`; 0 when there is none.
EM_JS(int, fg_page_context_text, (char* out, int size), {
    const gl = globalThis.funkguiPage.canvas.getContext('webgl2');
    if (!gl) return 0;
    const info = gl.getExtension('WEBGL_debug_renderer_info');
    const renderer = info ? gl.getParameter(info.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER);
    stringToUTF8(gl.getParameter(gl.VERSION) + '; ' + gl.getParameter(gl.SHADING_LANGUAGE_VERSION) + '; ' + renderer,
                 out, size);
    return 1;
});

// Its attributes as the browser reports them: bit 0 alpha, 1 antialias, 2 depth, 3 stencil; -1 with no context.
EM_JS(int, fg_page_context_attributes, (), {
    const gl = globalThis.funkguiPage.canvas.getContext('webgl2');
    const a = gl && gl.getContextAttributes();
    if (!a) return -1;
    return (a.alpha ? 1 : 0) | (a.antialias ? 2 : 0) | (a.depth ? 4 : 0) | (a.stencil ? 8 : 0);
});

// WEBGL_lose_context on that context: 1 when the loss was forced, 0 when the extension is missing.
EM_JS(int, fg_page_lose, (), {
    const page = globalThis.funkguiPage;
    const gl = page.canvas.getContext('webgl2');
    page.ext = gl && gl.getExtension('WEBGL_lose_context');
    if (!page.ext) return 0;
    page.ext.loseContext();
    return 1;
});

EM_JS(void, fg_page_restore, (), { globalThis.funkguiPage.ext.restoreContext(); });

namespace
{
    constexpr const char* kCanvas = "#funkgui-canvas";
    constexpr const char* kTakenCanvas = "#funkgui-taken";      // index.html gave it a 2D context
    constexpr int kLogicalW = 480, kLogicalH = 300;

    // A frame against SoftRaster (see the header comment for the measurements these come from).
    constexpr int    kTolerance = 2;             // "over tolerance" counts the samples that differ by more than this
    constexpr int    kWorst = 16;                // no sample may differ by more
    constexpr double kOverPerMille = 10.0;       // at most this share of a frame's samples may be over kTolerance

    constexpr double kPollMs = 10.0, kEventTimeoutMs = 5000.0;

    struct Case
    {
        const char* name;
        int         theme;
        float       dpi;
    };
    constexpr Case kCases[] = { { "graphite dpi 1", 0, 1.0f }, { "paper dpi 1.25", 1, 1.25f },
                                { "graphite dpi 2", 0, 2.0f } };
    constexpr std::size_t kCaseCount = sizeof(kCases) / sizeof(kCases[0]);

    constexpr const char* kKindNames[] = { "rrect", "text", "segment", "area" };
    constexpr int kKinds = 4;

    // ---- output -----------------------------------------------------------------------------------------------------
    std::string firstFailure;                    // "" while everything passed

    __attribute__((format(printf, 1, 2))) void say(const char* format, ...)
    {
        char text[1024];
        va_list args;
        va_start(args, format);
        std::vsnprintf(text, sizeof text, format, args);
        va_end(args);
        fg_page_line(text);
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
        if (firstFailure.empty())
        {
            say("VERDICT  PASS");
            fg_page_title("PASS");
        }
        else
        {
            say("VERDICT  FAIL: %s", firstFailure.c_str());
            fg_page_title(("FAIL: " + firstFailure).c_str());
        }
    }

    // ---- the scene: every kind, real text, overlaps and a clip ------------------------------------------------------
    void scene(Canvas& c, const Theme& th)
    {
        // rrect (kind 0): square, bordered, per-corner radii with a soft edge, a ring, a pill, hairlines, dots, a disc
        c.rrect(12.0f, 12.0f, 56.0f, 32.0f, 0.0f, th.ink16);
        c.rrect(80.25f, 12.5f, 56.0f, 32.0f, 6.0f, th.ink32, 1.5f, th.accent);
        c.rrect4(148.1f, 12.2f, 60.3f, 32.4f, 0.0f, 10.0f, 3.0f, 14.0f, th.accentDim, 0.0f, {}, 2.5f);
        c.rrect(220.0f, 12.0f, 32.0f, 32.0f, 16.0f, th.ink70.withAlpha(0.0f), 1.5f, th.ink70);
        c.rrect(264.0f, 16.0f, 80.0f, 24.0f, 9999.0f, th.signal.withAlpha(0.6f));
        c.hairlineH(12.0f, 52.3f, 332.0f, th.ink16);
        c.hairlineV(352.7f, 12.0f, 40.0f, th.ink32);
        c.dotted(12.0f, 58.0f, 332.0f, 4.0f, th.ink52);
        c.disc(376.0f, 28.0f, 10.0f, th.accent, 2.0f, th.ink100);
        c.disc(404.5f, 28.5f, 5.0f, th.signal);

        // segment (kind 2): a diagonal, a soft vertical, a curve of joined capsules
        c.segment(12.0f, 110.0f, 110.0f, 70.0f, 1.5f, th.ink100);
        c.segment(124.0f, 70.0f, 124.0f, 110.0f, 2.0f, th.ink52, 0.75f);
        {
            float xs[49], ys[49];
            for (int i = 0; i < 49; ++i)
            {
                xs[i] = 140.0f + 4.0f * static_cast<float>(i);
                const float t = static_cast<float>(i);
                ys[i] = 90.0f - 18.0f * std::sin(t * 0.31f) * (1.0f - t / 64.0f);
            }
            c.polyline(xs, ys, 49, 1.25f, funkgui::premix(th.ground, th.accent, 0.9f));
        }
        c.segment(350.0f, 72.0f, 462.0f, 108.0f, 3.5f, th.ink32.withAlpha(0.5f), 1.5f);

        // area (kind 3): single columns with each stroke edge, a strip with fractional edges, a band between two curves
        c.area(12.0f, 44.0f, 132.0f, 126.0f, 168.0f, 160.0f, th.accentDim, 1.5f, th.accent);
        c.area(52.0f, 84.0f, 126.0f, 138.0f, 160.0f, 168.0f, th.ink16, 1.5f, th.ink70, funkgui::AreaEdge::bottom);
        c.area(92.0f, 124.0f, 130.0f, 130.0f, 166.0f, 158.0f, th.ink32.withAlpha(0.5f), 2.0f, th.signal,
               funkgui::AreaEdge::both);
        {
            float xs[25], top[25], bottom[25];
            for (int i = 0; i < 25; ++i)
            {
                const float t = static_cast<float>(i) / 24.0f;
                xs[i] = 140.0f + 7.3f * static_cast<float>(i);
                top[i] = 150.0f - 22.0f * std::sin(t * 5.1f) * std::sin(t * 5.1f);
                bottom[i] = 158.0f + 8.0f * std::cos(t * 7.0f);
            }
            c.areaStrip(xs, 25, top, nullptr, 170.0f, th.accentDim.withAlpha(0.55f), 1.5f, th.accent);
            c.areaStrip(xs, 25, top, bottom, 0.0f, th.signal.withAlpha(0.25f), 1.0f, th.signal,
                        funkgui::AreaEdge::both);
        }

        // text (kind 1): the type scale, the three alignments, glyphs past ASCII
        c.text("FUNKGUI WEBGL2 SINK", 12.0f, 182.0f, funkgui::type::kWordmark, th.ink100);
        c.text("THRESHOLD \xE2\x88\x92" "12.5 DB", 12.0f, 204.0f, funkgui::type::kLabel, th.ink52);
        c.text("\xE2\x88\x92" "12.5", 12.0f, 222.0f, funkgui::type::kDisplay, th.ink100);
        c.text("DB", 132.0f, 248.0f, funkgui::type::kUnit, th.ink52);
        c.text("4:1", 240.0f, 196.0f, funkgui::type::kValueP, th.ink70, funkgui::Align::centre);
        c.text("0123456789", 240.0f, 226.0f, funkgui::type::kValueS, th.ink100, funkgui::Align::centre);
        c.text("\xC2\xB0\xC2\xB1\xC3\x97\xE2\x80\x93\xE2\x86\x92\xE2\x88\x9E\xC2\xB5\xE2\x89\xA4\xE2\x89\xA5\xCE\x94",
               468.0f, 196.0f, funkgui::type::kNumeral, th.accent, funkgui::Align::right);
        c.text("the quick brown fox jumps over the lazy dog", 468.0f, 226.0f, funkgui::type::kCaption, th.ink52,
               funkgui::Align::right);
        c.text("ATTACK  RELEASE  RATIO  KNEE  MIX", 468.0f, 244.0f, funkgui::type::kMicro, th.ink32,
               funkgui::Align::right);

        // order and clipping: text under a translucent panel, text over it, a row cut by a clip (on a device px at
        // dpi 1, 1.25 and 2: multiples of 4 logical px)
        c.text("UNDER THE PANEL", 20.0f, 272.0f, funkgui::type::kLatch, th.ink100);
        c.rrect(12.0f, 264.0f, 200.0f, 28.0f, 4.0f, th.accentDim.withAlpha(0.6f), 1.0f, th.accent);
        c.text("OVER", 160.0f, 272.0f, funkgui::type::kLatch, th.ink100);
        {
            const Canvas::ClipScope clip(c, funkgui::Rect{ 232.0f, 264.0f, 180.0f, 24.0f });
            c.rrect(224.0f, 260.0f, 250.0f, 36.0f, 8.0f, th.ink16, 1.5f, th.ink52);
            c.text("CLIPPED AT BOTH ENDS OF THIS ROW", 226.0f, 270.0f, funkgui::type::kLabel, th.ink100);
        }
    }

    PrimList record(Canvas& canvas, const Case& k)
    {
        const Theme th = Theme::byIndex(k.theme);
        FrameInfo info;
        info.logicalW = kLogicalW;
        info.logicalH = kLogicalH;
        info.dpi = k.dpi;
        info.clear = th.ground;
        info.textGamma = th.textGamma;
        info.theme = k.theme;
        info.fixedClock = true;
        canvas.begin(info);
        scene(canvas, th);
        return canvas.end();
    }

    int kindOf(const Prim& p) noexcept
    {
        return p.d2[2] < 0.5f ? 0 : (p.d2[2] < 1.5f ? 1 : (p.d2[2] < 2.5f ? 2 : 3));
    }

    PrimList onlyKind(const PrimList& list, int kind)
    {
        PrimList out;
        out.info = list.info;
        for (const Prim& p : list.prims)
            if (kindOf(p) == kind)
                out.prims.push_back(p);
        return out;
    }

    // ---- the comparison ---------------------------------------------------------------------------------------------
    constexpr int kBins = 10;                    // |difference| 0..8, then 9 or more

    struct Diff
    {
        bool        sameSize = false;
        int         worst = 0, worstX = 0, worstY = 0, worstChannel = 0, worstGl = 0, worstSoft = 0;
        std::size_t samples = 0, over = 0;
        std::size_t histogram[kBins] = {};
    };

    Diff compare(const Image& gl, const Image& soft)
    {
        Diff d;
        d.sameSize = gl.w == soft.w && gl.h == soft.h && gl.w > 0 && gl.rgba.size() == soft.rgba.size()
                  && gl.rgba.size() == static_cast<std::size_t>(gl.w) * static_cast<std::size_t>(gl.h) * 4u;
        if (!d.sameSize)
            return d;
        d.samples = gl.rgba.size();
        for (std::size_t i = 0; i < gl.rgba.size(); ++i)
        {
            const int delta = std::abs(static_cast<int>(gl.rgba[i]) - static_cast<int>(soft.rgba[i]));
            ++d.histogram[std::min(delta, kBins - 1)];
            if (delta > kTolerance)
                ++d.over;
            if (delta > d.worst)
            {
                d.worst = delta;
                d.worstX = static_cast<int>((i / 4u) % static_cast<std::size_t>(gl.w));
                d.worstY = static_cast<int>((i / 4u) / static_cast<std::size_t>(gl.w));
                d.worstChannel = static_cast<int>(i % 4u);
                d.worstGl = gl.rgba[i];
                d.worstSoft = soft.rgba[i];
            }
        }
        return d;
    }

    double overPerMille(const Diff& d) noexcept
    {
        return d.samples > 0 ? static_cast<double>(d.over) * 1000.0 / static_cast<double>(d.samples) : 0.0;
    }

    bool within(const Diff& d) noexcept
    {
        return d.sameSize && d.worst <= kWorst && overPerMille(d) <= kOverPerMille;
    }

    std::string describe(const Diff& d)
    {
        if (!d.sameSize)
            return "the images differ in size";
        char text[512];
        int n = std::snprintf(text, sizeof text,
                              "largest channel difference %d (WebGL %d, SoftRaster %d, channel %c at %d,%d); %zu of "
                              "%zu samples over %d (%.2f per mille); samples by |difference|",
                              d.worst, d.worstGl, d.worstSoft, "rgba"[d.worstChannel], d.worstX, d.worstY, d.over,
                              d.samples, kTolerance, overPerMille(d));
        for (int b = 0; b < kBins && n > 0 && static_cast<std::size_t>(n) < sizeof text; ++b)
            if (d.histogram[b] != 0 || b <= kTolerance)
                n += std::snprintf(text + n, sizeof text - static_cast<std::size_t>(n),
                                   b + 1 < kBins ? " %d: %zu" : " %d or more: %zu", b, d.histogram[b]);
        return text;
    }

    // Pixels that are not the clear colour.
    std::size_t drawnPixels(const Image& image, Col clear)
    {
        std::size_t n = 0;
        for (std::size_t i = 0; i + 3 < image.rgba.size(); i += 4)
            if (image.rgba[i] != clear.r || image.rgba[i + 1] != clear.g || image.rgba[i + 2] != clear.b)
                ++n;
        return n;
    }

    // ---- the run ----------------------------------------------------------------------------------------------------
    struct Page
    {
        const funkgui::FontAtlasSdf* atlas = nullptr;
        std::unique_ptr<Canvas>      canvas;
        std::unique_ptr<WebGlSink>   sink;
        std::vector<Image>           before;     // each case as WebGL drew it before the loss
        double                       waitedMs = 0.0;
    };
    Page page;

    const char* resultName(WebGlSink::Result r) noexcept
    {
        switch (r)
        {
            case WebGlSink::Result::submitted:   return "submitted";
            case WebGlSink::Result::empty:       return "empty";
            case WebGlSink::Result::lost:        return "lost";
            case WebGlSink::Result::unavailable: return "unavailable";
        }
        return "?";
    }

    // One list through the sink and through SoftRaster. `label` names it in the log; the WebGL image is returned.
    Image drawAndCompare(const std::string& label, const PrimList& list, bool mustDraw)
    {
        const Image soft = funkgui::rasterise(list, *page.atlas, 1);
        const uint32_t framesBefore = page.sink->frameCount();
        const WebGlSink::Result result = page.sink->submit(list, soft.w, soft.h);
        const Image gl = page.sink->readPixels();
        const bool drew = result == WebGlSink::Result::submitted && page.sink->frameCount() == framesBefore + 1
                       && page.sink->lastVertexCount() == list.prims.size() * 6u;
        if (!drew)
        {
            fail(label + ": submit() returned " + resultName(result) + " (frame count "
                 + std::to_string(page.sink->frameCount()) + ", vertices "
                 + std::to_string(page.sink->lastVertexCount()) + ", error '" + page.sink->error() + "')");
            return gl;
        }
        const Diff d = compare(gl, soft);
        const std::size_t glDrawn = drawnPixels(gl, list.info.clear), softDrawn = drawnPixels(soft, list.info.clear);
        say("         %s: %d x %d px, %zu prims, %u vertices, frame %u; pixels drawn WebGL %zu, SoftRaster %zu",
            label.c_str(), gl.w, gl.h, list.prims.size(), page.sink->lastVertexCount(), page.sink->frameCount(),
            glDrawn, softDrawn);
        say("         %s: %s", label.c_str(), describe(d).c_str());
        if (!within(d))
            fail(label + ": WebGL is outside the bounds against SoftRaster (largest difference "
                 + std::to_string(d.worst) + ", " + std::to_string(d.over) + " of " + std::to_string(d.samples)
                 + " samples over " + std::to_string(kTolerance) + ")");
        if (mustDraw && (glDrawn == 0 || softDrawn == 0))
            fail(label + ": nothing was drawn (WebGL " + std::to_string(glDrawn) + " px, SoftRaster "
                 + std::to_string(softDrawn) + " px)");
        return gl;
    }

    // Every case: the whole frame (kept in `out`) and, when asked, each kind alone.
    void drawCases(const char* phase, bool kindsAlone, std::vector<Image>& out)
    {
        out.clear();
        for (const Case& k : kCases)
        {
            const PrimList list = record(*page.canvas, k);
            int counts[kKinds] = {};
            for (const Prim& p : list.prims)
                ++counts[kindOf(p)];
            const std::string label = std::string(phase) + ", " + k.name;
            say("frame    %s: %zu prims (rrect %d, text %d, segment %d, area %d), missing glyphs %u", label.c_str(),
                list.prims.size(), counts[0], counts[1], counts[2], counts[3], list.missingGlyphs);
            for (int kind = 0; kind < kKinds; ++kind)
                if (counts[kind] == 0)
                    fail(label + ": the scene recorded no " + kKindNames[kind] + " primitive");
            if (list.missingGlyphs != 0)
                fail(label + ": the atlas lacks glyphs the scene draws");
            out.push_back(drawAndCompare(label, list, true));
            if (kindsAlone)
                for (int kind = 0; kind < kKinds; ++kind)
                    drawAndCompare(label + ", " + kKindNames[kind] + " alone", onlyKind(list, kind), true);
        }
    }

    // Where a hard edge passes through pixel centres (a note, not a check: see the header comment). A white square
    // clipped to 10.5 .. 20.5 in x and y at dpi 1: every pixel centre on its border is a tie.
    void edgeRule()
    {
        FrameInfo info;
        info.logicalW = 32;
        info.logicalH = 32;
        info.clear = Col{ 0, 0, 0, 255 };
        info.fixedClock = true;
        page.canvas->begin(info);
        {
            const Canvas::ClipScope clip(*page.canvas, funkgui::Rect{ 10.5f, 10.5f, 10.0f, 10.0f });
            page.canvas->rrect(0.0f, 0.0f, 32.0f, 32.0f, 0.0f, Col{ 255, 255, 255, 255 });
        }
        const PrimList list = page.canvas->end();
        const Image soft = funkgui::rasterise(list, *page.atlas, 1);
        const WebGlSink::Result result = page.sink->submit(list, soft.w, soft.h);
        const Image gl = page.sink->readPixels();
        if (result != WebGlSink::Result::submitted || gl.w != 32 || gl.h != 32 || soft.w != 32 || soft.h != 32)
        {
            fail("the edge rule frame was not drawn");
            return;
        }
        // The lit span of row 15 (columns) and of column 15 (rows), as "first..last".
        const auto span = [](const Image& image, bool alongX) {
            int first = -1, last = -1;
            for (int i = 0; i < 32; ++i)
            {
                const int x = alongX ? i : 15, y = alongX ? 15 : i;
                if (image.rgba[(static_cast<std::size_t>(y) * 32u + static_cast<std::size_t>(x)) * 4u] > 127)
                {
                    first = first < 0 ? i : first;
                    last = i;
                }
            }
            return std::to_string(first) + ".." + std::to_string(last);
        };
        const std::string softX = span(soft, true), softY = span(soft, false);
        const std::string glX = span(gl, true), glY = span(gl, false);
        say("note     a hard edge through pixel centres (a clip at 10.5 .. 20.5, dpi 1): SoftRaster fills columns %s "
            "and rows %s, WebGL fills columns %s and rows %s",
            softX.c_str(), softY.c_str(), glX.c_str(), glY.c_str());
        if (softX != glX || softY != glY)
            say("note     WebGL gives a tied row or column to the other quad than Metal and SoftRaster do: a clip edge "
                "that lies on pixel centres cuts one pixel further on");
    }

    bool sameImage(const Image& a, const Image& b) noexcept
    {
        return a.w == b.w && a.h == b.h && a.w > 0 && a.rgba == b.rgba;
    }

    // A sink that cannot have a context: it must say so and refuse, never throw.
    void refuses(const char* selector, const char* what)
    {
        WebGlSink sink(selector);
        const PrimList empty;
        const bool refused = !sink.ok() && !sink.lost() && sink.error()[0] != '\0'
                          && sink.submit(empty, 16, 16) == WebGlSink::Result::unavailable && sink.frameCount() == 0
                          && sink.readPixels().w == 0;
        check(refused, std::string("a sink on ") + what + " ('" + selector + "') refuses: " + sink.error());
    }

    void afterRestore(void*);
    void afterLoss(void*);

    void finish()
    {
        // ---- 7. a second sink on the same canvas, after the first is gone
        page.sink.reset();
        page.sink = std::make_unique<WebGlSink>(kCanvas);
        if (!page.sink->ok())
            fail(std::string("a new sink on the same canvas, after the first was destroyed, is not ok(): ")
                 + page.sink->error());
        else
        {
            say("ok       a new sink on the same canvas, after the first was destroyed, is ok()");
            const PrimList list = record(*page.canvas, kCases[0]);
            const Image again = drawAndCompare(std::string("second sink, ") + kCases[0].name, list, true);
            check(!page.before.empty() && sameImage(again, page.before[0]),
                  "the second sink draws the first case byte for byte");
        }
        verdict();
    }

    void afterRestore(void*)
    {
        if (fg_page_restored_events() == 0)
        {
            page.waitedMs += kPollMs;
            if (page.waitedMs >= kEventTimeoutMs)
            {
                fail("webglcontextrestored was not delivered within 5 s of restoreContext()");
                verdict();
                return;
            }
            emscripten_set_timeout(afterRestore, kPollMs, nullptr);
            return;
        }
        // ---- 6. restored: the sink rebuilt, and draws what it drew
        say("restore  webglcontextrestored delivered after %.0f ms", page.waitedMs);
        WebGlSink& sink = *page.sink;
        check(sink.ok() && !sink.lost() && sink.restoreCount() == 1 && sink.error()[0] == '\0',
              "after the restore the sink is ok(): program, buffer and texture rebuilt (restoreCount "
                  + std::to_string(sink.restoreCount()) + ", error '" + sink.error() + "')");
        if (sink.ok())
        {
            std::vector<Image> after;
            drawCases("after the restore", false, after);
            for (std::size_t i = 0; i < kCaseCount && i < after.size() && i < page.before.size(); ++i)
                check(sameImage(after[i], page.before[i]),
                      std::string(kCases[i].name) + ": the frame after the restore is byte for byte the frame before "
                                                    "the loss");
        }
        finish();
    }

    void afterLoss(void*)
    {
        if (fg_page_lost_events() == 0)
        {
            page.waitedMs += kPollMs;
            if (page.waitedMs >= kEventTimeoutMs)
            {
                fail("webglcontextlost was not delivered within 5 s of loseContext()");
                verdict();
                return;
            }
            emscripten_set_timeout(afterLoss, kPollMs, nullptr);
            return;
        }
        // ---- 5. lost: the sink knows, and refuses
        say("loss     webglcontextlost delivered after %.0f ms", page.waitedMs);
        WebGlSink& sink = *page.sink;
        const uint32_t frames = sink.frameCount();
        const PrimList list = record(*page.canvas, kCases[0]);
        const WebGlSink::Result result = sink.submit(list, kLogicalW, kLogicalH);
        check(sink.lost() && !sink.ok() && result == WebGlSink::Result::lost && sink.frameCount() == frames
                  && sink.lastVertexCount() == 0 && sink.readPixels().w == 0 && sink.error()[0] == '\0',
              std::string("while the context is lost the sink reports lost(), submit() returns ") + resultName(result)
                  + ", no frame is counted and nothing is read back");

        page.waitedMs = 0.0;
        fg_page_restore();
        say("restore  WEBGL_lose_context.restoreContext() called");
        emscripten_set_timeout(afterRestore, kPollMs, nullptr);
    }

    void run()
    {
        say("FunkGui::web: WebGlSink against SoftRaster (%d x %d logical px; a frame passes when no channel sample "
            "differs by more than %d and at most %.0f per mille differ by more than %d)",
            kLogicalW, kLogicalH, kWorst, kOverPerMille, kTolerance);

        // ---- 1. the atlas
        funkgui::FontService& fonts = funkgui::FontService::get();
        page.atlas = &fonts.atlas();
        char hash[32];
        std::snprintf(hash, sizeof hash, "%016llx", static_cast<unsigned long long>(fonts.atlasHash()));
        if (!check(page.atlas->baked() && fonts.ok(),
                   std::string("the atlas is the committed bake of the embedded face (hash ") + hash + ")"))
        {
            verdict();
            return;
        }
        page.canvas = std::make_unique<Canvas>(*page.atlas);

        // ---- 2. sinks that cannot have a context
        refuses("#funkgui-no-such-canvas", "a selector that names no element");
        refuses("##", "a malformed selector");
        refuses("#funkgui-log", "an element that is not a canvas");
        refuses(kTakenCanvas, "a canvas that cannot give a WebGL2 context");

        // ---- 3. the sink
        page.sink = std::make_unique<WebGlSink>(kCanvas);
        if (!page.sink->ok())
        {
            fail(std::string("WebGlSink on '") + kCanvas + "' is not ok(): " + page.sink->error());
            verdict();
            return;
        }
        say("ok       WebGlSink on '%s' is ok(): context created, program linked, buffer and atlas texture built",
            kCanvas);
        fg_page_watch(kCanvas);
        char context[512];
        if (fg_page_context_text(context, static_cast<int>(sizeof context)) != 0)
            say("context  %s", context);
        const int attributes = fg_page_context_attributes();
        check(attributes == 0, "the context has no alpha, no antialias, no depth and no stencil (alpha "
                                   + std::to_string(attributes & 1) + ", antialias "
                                   + std::to_string((attributes >> 1) & 1) + ", depth "
                                   + std::to_string((attributes >> 2) & 1) + ", stencil "
                                   + std::to_string((attributes >> 3) & 1) + ")");
        {
            // An empty list clears and counts a frame; a size below 1 x 1 does nothing.
            PrimList nothing;
            nothing.info.logicalW = 8;
            nothing.info.logicalH = 8;
            nothing.info.clear = Col{ 10, 200, 30, 255 };
            const WebGlSink::Result cleared = page.sink->submit(nothing, 8, 8);
            const Image image = page.sink->readPixels();
            const bool flat = image.w == 8 && image.h == 8 && drawnPixels(image, nothing.info.clear) == 0
                           && image.rgba[3] == 255;
            check(cleared == WebGlSink::Result::empty && page.sink->frameCount() == 1 && flat
                      && page.sink->submit(nothing, 0, 8) == WebGlSink::Result::empty && page.sink->frameCount() == 1,
                  "an empty list clears the canvas to the frame's clear colour, opaque, and counts one frame");
        }

        // ---- 4. the frames
        drawCases("first", true, page.before);
        edgeRule();

        // ---- 5. the loss (continues in afterLoss, once the browser has delivered the event)
        if (fg_page_lose() == 0)
        {
            fail("the browser has no WEBGL_lose_context: the loss and restore could not be run");
            verdict();
            return;
        }
        say("loss     WEBGL_lose_context.loseContext() called");
        page.waitedMs = 0.0;
        emscripten_set_timeout(afterLoss, kPollMs, nullptr);
    }
}

int main()
{
    run();
    return 0;
}

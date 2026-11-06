// FUNKGUI_TEST name=fg.shader.draw timeout=300 gpu=1
//
// fg.shader.draw (v0.19.0; gpu/ShaderLibrary.h, gpu/BgfxSink.h): custom shaders drawn for real, on Metal with no
// window. BgfxContext is acquired headless (a null native window and a 0 x 0 backbuffer, bgfx's headless mode: Metal
// then has no swapchain), a frame recorded by a Canvas is submitted by BgfxSink into an offscreen BGRA8 framebuffer,
// blitted and read back, and its pixels are compared with what the program (fs_custom_test: shaders/
// fs_custom_prelude.sc + test/shaders/fs_custom_test.main.sc, compiled at build time with FunkGui's shaderc) and the
// UI program must have drawn. Needs a Metal device, not a window server; without one (a VM) every row is skipped and
// noted. Spec rows only.
//
// - draw.*: the frame (an rrect under everything, a clipped shader draw, an rrect over part of it, a shader draw whose
//   id has no program, a half-transparent shader draw reading v_px) is 4 bgfx draw calls (BgfxSink::lastDrawCalls and
//   bgfx's own count): the shader colours v_uv / v_px / u_params at known pixels to within 2/255, the clip and the rect
//   bound it to the pixel (the scissor), the later rrect covers it, the missing program draws nothing, alpha blends.
// - plain.*: a frame without shader draws is one draw call.
// - restart.*: releasing the last window shuts bgfx down (target() empty, the id still registered); acquiring again
//   builds the program from the kept binary at its first draw, and it draws the same pixel.
// - set.*: set() while bgfx runs: target() is Metal's, a refused binary leaves the program drawing.
// - checked.* (0.25.0): a new code (fs_custom_test's Metal code with one more comment line) checked by
//   shaders::check() on a worker thread while bgfx runs: set() of it on this thread runs no Metal compile, and it
//   draws what fs_custom_test draws; an unchecked new code's set() runs one. Their times are INFO lines.
// The draw.* frame as read back is written to shader-draw.png in the test's sandbox.

#import <Metal/Metal.h>

#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/canvas/SoftRaster.h>
#include <funkgui/core/Col.h>
#include <funkgui/gpu/BgfxContext.h>
#include <funkgui/gpu/BgfxSink.h>
#include <funkgui/gpu/ShaderLibrary.h>
#include <funkgui/shaders/fs_custom_test.mtl.h>
#include <funkgui/test/Harness.h>

#include "../../src/gpu/ShaderLibraryDetail.h"

#include <bgfx/bgfx.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace T = funkgui::test;
using funkgui::Col;
using funkgui::FrameInfo;
using funkgui::PrimList;

namespace
{
    constexpr int kW = 80, kH = 60;                 // logical
    constexpr float kDpi = 2.0f;
    constexpr int kPw = 160, kPh = 120;             // physical
    constexpr bgfx::ViewId kDrawView = 1, kBlitView = 2;
    constexpr Col kClear{ 10, 12, 14, 255 };
    constexpr Col kUnder{ 200, 40, 40, 255 };
    constexpr Col kOver{ 40, 200, 40, 255 };
    constexpr uint32_t kId = 1, kMissing = 2;

    FrameInfo frame()
    {
        FrameInfo f;
        f.logicalW = kW;
        f.logicalH = kH;
        f.dpi = kDpi;
        f.clear = kClear;
        f.textGamma = 0.714285731f;
        f.fixedClock = true;
        return f;
    }

    struct Target
    {
        bgfx::TextureHandle     colour = BGFX_INVALID_HANDLE;
        bgfx::TextureHandle     readback = BGFX_INVALID_HANDLE;
        bgfx::FrameBufferHandle fb = BGFX_INVALID_HANDLE;
        std::vector<uint8_t>    bgra = std::vector<uint8_t>(static_cast<size_t>(kPw) * kPh * 4u);

        bool make()
        {
            colour = bgfx::createTexture2D(kPw, kPh, false, 1, bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_RT);
            readback = bgfx::createTexture2D(kPw, kPh, false, 1, bgfx::TextureFormat::BGRA8,
                                             BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
            fb = bgfx::createFrameBuffer(1, &colour, false);
            return bgfx::isValid(colour) && bgfx::isValid(readback) && bgfx::isValid(fb);
        }

        void drop()
        {
            if (bgfx::isValid(fb)) bgfx::destroy(fb);
            if (bgfx::isValid(readback)) bgfx::destroy(readback);
            if (bgfx::isValid(colour)) bgfx::destroy(colour);
            fb = BGFX_INVALID_HANDLE;
            readback = colour = BGFX_INVALID_HANDLE;
        }

        // Submits the list, renders the frame, and reads the pixels back. Returns bgfx's draw count of that frame
        // (-1 when the read never completed).
        int draw(funkgui::BgfxSink& sink, const PrimList& list)
        {
            sink.submit(list, kDrawView, fb, kPw, kPh);
            bgfx::touch(kBlitView);
            bgfx::blit(kBlitView, readback, 0, 0, colour);
            const uint32_t ready = bgfx::readTexture(readback, bgra.data());
            // The first frame() renders it; bgfx's stats are those of the frame buffer now on the submit side, so
            // the frame's own draw count is readable after the next one.
            bgfx::frame();
            uint32_t now = bgfx::frame();
            const int draws = static_cast<int>(bgfx::getStats()->numDraw);
            std::printf("INFO     bgfx numDraw %d (BgfxSink %u)\n", draws, sink.lastDrawCalls());
            for (int i = 0; i < 16 && now < ready; ++i)
                now = bgfx::frame();
            return now >= ready ? draws : -1;
        }

        // The pixel at a logical point (device pixel floor(x * dpi), floor(y * dpi)) within tol of (r, g, b) / 255.
        bool near(float x, float y, double r, double g, double b, double tol = 2.0) const
        {
            const int px = static_cast<int>(x * kDpi), py = static_cast<int>(y * kDpi);
            const uint8_t* p = &bgra[(static_cast<size_t>(py) * kPw + static_cast<size_t>(px)) * 4u];
            const bool ok = std::abs(p[2] - r) <= tol && std::abs(p[1] - g) <= tol && std::abs(p[0] - b) <= tol;
            if (!ok)
                std::printf("INFO     pixel (%g, %g) = %u %u %u, want %.1f %.1f %.1f\n", static_cast<double>(x),
                            static_cast<double>(y), p[2], p[1], p[0], r, g, b);
            return ok;
        }
        bool is(float x, float y, Col c) const { return near(x, y, c.r, c.g, c.b, 0.0); }

        // The read-back pixels as a PNG (RGBA), for a person to look at.
        bool png(const char* path) const
        {
            funkgui::Image img;
            img.w = kPw;
            img.h = kPh;
            img.rgba.resize(bgra.size());
            for (size_t i = 0; i < bgra.size(); i += 4)
            {
                img.rgba[i] = bgra[i + 2];
                img.rgba[i + 1] = bgra[i + 1];
                img.rgba[i + 2] = bgra[i];
                img.rgba[i + 3] = bgra[i + 3];
            }
            return funkgui::writePng(img, path);
        }
    };

    // What fs_custom_test draws at the logical point (x, y) of a rect: (uv, b) at alpha a over `under`, in 0..255.
    void expect(const funkgui::Rect& r, float x, float y, float b, float a, Col under, double out[3])
    {
        const double u = (static_cast<double>(x) - r.x) / r.w, v = (static_cast<double>(y) - r.y) / r.h;
        const double src[3] = { u, v, b };
        const double dst[3] = { under.r / 255.0, under.g / 255.0, under.b / 255.0 };
        for (int i = 0; i < 3; ++i)
            out[i] = (src[i] * a + dst[i] * (1.0 - a)) * 255.0;
    }

    // fs_custom_test with one more comment line at the end of its Metal code (the code's size rewritten; its NUL and
    // what follows kept): a code no compile of this process has seen.
    std::vector<uint8_t> variant(const std::string& tag)
    {
        const std::vector<uint8_t> b(fs_custom_test_mtl, fs_custom_test_mtl + sizeof fs_custom_test_mtl);
        size_t at = 14;                              // magic, hash in, hash out, uniform count (shaderc v11)
        for (unsigned i = 0, n = b[12] | b[13] << 8; i < n; ++i)
            at += 1 + b[at] + 10;                    // name size, name, type, num, reg index and count, tex info, format
        const uint32_t size = b[at] | b[at + 1] << 8 | b[at + 2] << 16 | static_cast<uint32_t>(b[at + 3]) << 24;
        std::string code(reinterpret_cast<const char*>(&b[at + 4]), size);
        code += "\n// fg.shader.draw " + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
                + " " + tag + "\n";
        std::vector<uint8_t> out(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(at));
        const auto n = static_cast<uint32_t>(code.size());
        for (int i = 0; i < 4; ++i)
            out.push_back(static_cast<uint8_t>(n >> (8 * i)));
        out.insert(out.end(), code.begin(), code.end());
        out.insert(out.end(), b.begin() + static_cast<std::ptrdiff_t>(at + 4 + size), b.end());
        return out;
    }

    double msSince(std::chrono::steady_clock::time_point t0)
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }

    // The test frame: see the file comment.
    const funkgui::Rect kRect{ 10.0f, 15.0f, 40.0f, 30.0f };
    const funkgui::Rect kClip{ 20.0f, 10.0f, 50.0f, 40.0f };       // rect ∩ clip: x 20..50, y 15..45
    const funkgui::Rect kHalf{ 60.0f, 40.0f, 16.0f, 16.0f };

    void scene(funkgui::Canvas& c)
    {
        const float opaque[4] = { 0.0f, 0.0f, 0.25f, 1.0f };
        const float half[5] = { 0.0f, 0.0f, 0.5f, 0.5f, 1.0f };   // u_params[1].x = 1: the v_px path
        c.rrect(-4.0f, -4.0f, kW + 8.0f, kH + 8.0f, 0.0f, kUnder);
        c.pushClip(kClip);
        c.shader(kId, kRect, opaque, 4);
        c.popClip();
        c.rrect(40.0f, 30.0f, 30.0f, 20.0f, 0.0f, kOver);
        c.shader(kMissing, { 0.0f, 0.0f, 10.0f, 10.0f });
        c.shader(kId, kHalf, half, 5);
    }
}

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;      // the font atlas bakes through JUCE
    T::Probe P("fg.shader.draw", "", argc, argv);

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    const bool metal = device != nil;
    [device release];
    P.note("metal", metal ? "\"present\"" : "\"absent\"");
    if (!metal)
    {
        std::printf("INFO     no Metal device: nothing to draw on, every row skipped\n");
        return P.finish();
    }

    auto& ctx = funkgui::BgfxContext::get();
    if (!P.eq("context.acquired_headless", ctx.acquire(nullptr, 0, 0), 1))
        return P.finish();
    P.eq("context.metal", bgfx::getRendererType() == bgfx::RendererType::Metal, 1);
    const auto* caps = bgfx::getCaps();
    P.eq("context.blit_and_readback", (caps->supported & BGFX_CAPS_TEXTURE_BLIT) != 0
                                          && (caps->supported & BGFX_CAPS_TEXTURE_READ_BACK) != 0, 1);

    const funkgui::shaders::Target t = funkgui::shaders::target();
    P.eq("set.target_metal", t.platform == "osx" && t.profile == "metal", 1);
    std::string why;
    P.eq("set.registers", funkgui::shaders::set(kId, fs_custom_test_mtl, sizeof fs_custom_test_mtl, why), 1);
    if (!why.empty())
        std::printf("INFO     set: %s\n", why.c_str());

    funkgui::Canvas canvas(ctx.font());
    funkgui::BgfxSink sink;
    Target target;
    P.eq("context.target", target.make(), 1);

    // bgfx counts the views' touches as render items too: a frame with nothing in it is the base.
    canvas.begin(frame());
    const PrimList none = canvas.end();
    const int base = target.draw(sink, none);
    P.eq("context.base_frame", base >= 0 && sink.lastDrawCalls() == 0, 1);

    // ---- draw -----------------------------------------------------------------------------------------------------------
    canvas.begin(frame());
    scene(canvas);
    const PrimList list = canvas.end();
    const int draws = target.draw(sink, list);
    P.eq("draw.read_back", draws >= 0, 1);
    if (target.png("shader-draw.png"))                   // in the test's sandbox (the working directory)
        std::printf("INFO     wrote shader-draw.png\n");
    P.eq("draw.sink_calls", sink.lastDrawCalls(), 4);
    P.eq("draw.bgfx_calls", draws - base, 4);
    double want[3];
    expect(kRect, 25.25f, 20.25f, 0.25f, 1.0f, kUnder, want);
    P.eq("draw.uv", target.near(25.25f, 20.25f, want[0], want[1], want[2]), 1);
    expect(kRect, 45.25f, 25.25f, 0.25f, 1.0f, kUnder, want);
    P.eq("draw.uv_far", target.near(45.25f, 25.25f, want[0], want[1], want[2]), 1);
    expect(kRect, 20.25f, 30.25f, 0.25f, 1.0f, kUnder, want);
    P.eq("draw.clip_edge_in", target.near(20.25f, 30.25f, want[0], want[1], want[2]), 1);   // device x 40
    P.eq("draw.clip_edge_out", target.is(19.75f, 30.25f, kUnder), 1);                        // device x 39
    P.eq("draw.rect_edge_out", target.is(50.25f, 20.25f, kUnder), 1);                        // right of the rect
    P.eq("draw.rect_top_out", target.is(30.25f, 14.75f, kUnder), 1);                         // above the rect
    P.eq("draw.outside_clip", target.is(15.25f, 30.25f, kUnder), 1);                         // in the rect only
    P.eq("draw.covered", target.is(45.25f, 40.25f, kOver), 1);                               // the later rrect
    P.eq("draw.missing_program", target.is(5.25f, 5.25f, kUnder), 1);
    expect(kHalf, 72.25f, 52.25f, 0.5f, 0.5f, kUnder, want);
    P.eq("draw.v_px_and_alpha", target.near(72.25f, 52.25f, want[0], want[1], want[2], 3.0), 1);
    expect(kHalf, 64.25f, 44.25f, 0.5f, 0.5f, kOver, want);                                  // over the later rrect
    P.eq("draw.blends_over_later_prim", target.near(64.25f, 44.25f, want[0], want[1], want[2], 3.0), 1);

    // ---- plain ----------------------------------------------------------------------------------------------------------
    canvas.begin(frame());
    canvas.rrect(-4.0f, -4.0f, kW + 8.0f, kH + 8.0f, 0.0f, kUnder);
    canvas.rrect(40.0f, 30.0f, 30.0f, 20.0f, 0.0f, kOver);
    const PrimList plain = canvas.end();
    const int plainDraws = target.draw(sink, plain);
    P.eq("plain.sink_calls", sink.lastDrawCalls(), 1);
    P.eq("plain.bgfx_calls", plainDraws - base, 1);
    P.eq("plain.vertices", sink.lastVertexCount(), 12);
    P.eq("plain.pixels", target.is(25.25f, 20.25f, kUnder) && target.is(45.25f, 40.25f, kOver), 1);

    // ---- set while running ------------------------------------------------------------------------------------------------
    const uint8_t junk[8] = { 'F', 'S', 'H', 11, 0, 0, 0, 0 };
    P.eq("set.refused_running", funkgui::shaders::set(kId, junk, sizeof junk, why), 0);
    target.draw(sink, list);
    expect(kRect, 25.25f, 20.25f, 0.25f, 1.0f, kUnder, want);
    P.eq("set.old_program_draws", sink.lastDrawCalls() == 4 && target.near(25.25f, 20.25f, want[0], want[1], want[2]),
         1);

    // ---- checked: check() on a worker while bgfx runs, then set() -------------------------------------------------------
    namespace D = funkgui::shaders::detail;
    const std::vector<uint8_t> checkedBin = variant("checked"), uncheckedBin = variant("unchecked");
    bool workerOk = false;
    std::string workerWhy = "stale";
    std::thread([&] {
        workerOk = funkgui::shaders::check(checkedBin.data(), static_cast<uint32_t>(checkedBin.size()), workerWhy);
    }).join();
    P.eq("checked.worker_takes", workerOk && workerWhy.empty(), 1);
    if (!workerWhy.empty())
        std::printf("INFO     check on a worker: %s\n", workerWhy.c_str());
    uint64_t before = D::metalCompileCount();
    auto t0 = std::chrono::steady_clock::now();
    const bool checkedSet = funkgui::shaders::set(kId, checkedBin.data(), static_cast<uint32_t>(checkedBin.size()), why);
    const double checkedMs = msSince(t0);
    P.eq("checked.set", checkedSet, 1);
    P.eq("checked.set_compiles_none", static_cast<int64_t>(D::metalCompileCount() - before), 0);
    t0 = std::chrono::steady_clock::now();
    target.draw(sink, list);
    const double checkedFrameMs = msSince(t0);
    P.eq("checked.draws", sink.lastDrawCalls() == 4 && target.near(25.25f, 20.25f, want[0], want[1], want[2]), 1);
    before = D::metalCompileCount();
    t0 = std::chrono::steady_clock::now();
    const bool uncheckedSet = funkgui::shaders::set(kId, uncheckedBin.data(), static_cast<uint32_t>(uncheckedBin.size()),
                                                    why);
    const double uncheckedMs = msSince(t0);
    P.eq("checked.unchecked_set", uncheckedSet, 1);
    P.eq("checked.unchecked_set_compiles", static_cast<int64_t>(D::metalCompileCount() - before), 1);
    t0 = std::chrono::steady_clock::now();
    target.draw(sink, list);
    const double uncheckedFrameMs = msSince(t0);
    P.eq("checked.unchecked_draws", sink.lastDrawCalls() == 4
                                        && target.near(25.25f, 20.25f, want[0], want[1], want[2]), 1);
    std::printf("INFO     time: set() of a checked code %.3f ms, its frame %.2f ms; set() of an unchecked code %.2f ms, "
                "its frame %.2f ms (a frame: two bgfx::frame() and the read-back)\n", checkedMs, checkedFrameMs,
                uncheckedMs, uncheckedFrameMs);
    P.eq("checked.restored", funkgui::shaders::set(kId, fs_custom_test_mtl, sizeof fs_custom_test_mtl, why), 1);

    // ---- restart ----------------------------------------------------------------------------------------------------------
    target.drop();
    ctx.release(nullptr);
    P.eq("restart.down", !ctx.valid(), 1);
    P.eq("restart.target_empty", funkgui::shaders::target().platform.empty(), 1);
    P.eq("restart.still_registered", funkgui::shaders::has(kId), 1);
    if (P.eq("restart.acquired", ctx.acquire(nullptr, 0, 0), 1) && P.eq("restart.target", target.make(), 1))
    {
        const int again = target.draw(sink, list);
        P.eq("restart.bgfx_calls", again - base, 4);
        expect(kRect, 25.25f, 20.25f, 0.25f, 1.0f, kUnder, want);
        P.eq("restart.draws", target.near(25.25f, 20.25f, want[0], want[1], want[2]), 1);
        funkgui::shaders::remove(kId);
        target.draw(sink, list);
        P.eq("restart.removed_draws_nothing", sink.lastDrawCalls() == 2 && target.is(25.25f, 20.25f, kUnder), 1);
        target.drop();
        ctx.release(nullptr);
    }
    P.eq("restart.released", !ctx.valid(), 1);
    return P.finish();
}

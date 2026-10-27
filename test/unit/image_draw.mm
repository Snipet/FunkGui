// FUNKGUI_TEST name=fg.image.draw timeout=300 gpu=1
//
// fg.image.draw (v0.20.0; Canvas::image, gpu/BgfxSink.h): pictures drawn for real, on Metal with no window, as
// fg.shader.draw draws custom shaders. BgfxContext is acquired headless, a frame recorded by a Canvas is submitted by
// BgfxSink into an offscreen BGRA8 framebuffer, blitted and read back, and its pixels are compared with what the image
// program (shaders/vs_custom.sc + fs_image.sc) must have drawn, and with SoftRaster's image of the same PrimList at one
// sample per pixel. Needs a Metal device, not a window server; without one (a VM) every row is skipped and noted.
// Spec rows only.
//
// - draw.*: the frame (an rrect under everything; a 2 x 2 checker scaled up; an rrect over part of it; a white
//   picture with rounded corners; a half-opaque one under a clip; a uv crop of a row of four colours; a 64 x 64
//   one-texel checker shown at 8 x 8 logical px; a missing id) is 7 draw calls (BgfxSink::lastDrawCalls and bgfx's own
//   count): pure quadrants, the bilinear midpoint, a transparent corner, opacity, the clip's edge, the crop with no
//   bleed, the grey of a downscaled level, the later rrect over the picture, all within 3 / 255.
// - parity.*: every pixel against SoftRaster: none differs by more than 4, at most 1 % by more than 2.
// - textures.*: one texture per picture drawn (BgfxSink::imageTextureCount); a replaced picture is drawn in its new
//   pixels with the old texture gone; a removed one draws nothing and its texture is destroyed at the next submit.
// - restart.*: releasing the last window destroys every texture (the pictures stay registered); acquiring again
//   uploads them at their first draw, and they draw the same pixels.
// The draw.* frame as read back is written to image-draw.png in the test's sandbox.

#import <Metal/Metal.h>

#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/Images.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/canvas/SoftRaster.h>
#include <funkgui/core/Col.h>
#include <funkgui/gpu/BgfxContext.h>
#include <funkgui/gpu/BgfxSink.h>
#include <funkgui/test/Harness.h>

#include <bgfx/bgfx.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace T = funkgui::test;
namespace images = funkgui::images;
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
    constexpr Col kUnder{ 40, 60, 80, 255 };
    constexpr Col kOver{ 40, 200, 40, 255 };
    constexpr Col kRed{ 255, 0, 0, 255 }, kGreen{ 0, 255, 0, 255 }, kBlue{ 0, 0, 255, 255 };
    constexpr Col kYellow{ 255, 255, 0, 255 };
    constexpr Col kWhite{ 255, 255, 255, 255 };
    constexpr uint32_t kChecker = 1, kRow = 2, kPlain = 3, kFine = 4, kMissing = 99;

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

    void put(std::vector<uint8_t>& v, int i, Col c)
    {
        v[static_cast<size_t>(i) * 4u] = c.r;
        v[static_cast<size_t>(i) * 4u + 1u] = c.g;
        v[static_cast<size_t>(i) * 4u + 2u] = c.b;
        v[static_cast<size_t>(i) * 4u + 3u] = c.a;
    }

    void setPlain(Col c)
    {
        const uint8_t px[4] = { c.r, c.g, c.b, c.a };
        images::set(kPlain, 1, 1, px);
    }

    void registerPictures()
    {
        std::vector<uint8_t> v(16);
        put(v, 0, kRed);
        put(v, 1, kGreen);
        put(v, 2, kBlue);
        put(v, 3, kWhite);
        images::set(kChecker, 2, 2, v.data());
        put(v, 0, kRed);
        put(v, 1, kGreen);
        put(v, 2, kBlue);
        put(v, 3, kYellow);
        images::set(kRow, 4, 1, v.data());
        setPlain(kWhite);
        std::vector<uint8_t> fine(64u * 64u * 4u);
        for (int i = 0; i < 64 * 64; ++i)
        {
            const uint8_t g = ((i % 64) + (i / 64)) % 2 == 0 ? 0 : 255;
            put(fine, i, Col{ g, g, g, 255 });
        }
        images::set(kFine, 64, 64, fine.data());
        images::remove(kMissing);
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
            bgfx::frame();
            uint32_t now = bgfx::frame();
            const int draws = static_cast<int>(bgfx::getStats()->numDraw);
            std::printf("INFO     bgfx numDraw %d (BgfxSink %u)\n", draws, sink.lastDrawCalls());
            for (int i = 0; i < 16 && now < ready; ++i)
                now = bgfx::frame();
            return now >= ready ? draws : -1;
        }

        const uint8_t* at(int px, int py) const
        {
            return &bgra[(static_cast<size_t>(py) * kPw + static_cast<size_t>(px)) * 4u];
        }

        // The pixel at a logical point (device pixel floor(x * dpi), floor(y * dpi)) within tol of (r, g, b).
        bool near(float x, float y, double r, double g, double b, double tol = 3.0) const
        {
            const uint8_t* p = at(static_cast<int>(x * kDpi), static_cast<int>(y * kDpi));
            const bool ok = std::abs(p[2] - r) <= tol && std::abs(p[1] - g) <= tol && std::abs(p[0] - b) <= tol;
            if (!ok)
                std::printf("INFO     pixel (%g, %g) = %u %u %u, want %.1f %.1f %.1f\n", static_cast<double>(x),
                            static_cast<double>(y), p[2], p[1], p[0], r, g, b);
            return ok;
        }
        bool is(float x, float y, Col c, double tol = 1.0) const { return near(x, y, c.r, c.g, c.b, tol); }

        funkgui::Image image() const
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
            return img;
        }
    };

    double over(double a, double b, double t) { return a * t + b * (1.0 - t); }

    // The test frame: see the file comment.
    void scene(funkgui::Canvas& c)
    {
        c.rrect(-4.0f, -4.0f, kW + 8.0f, kH + 8.0f, 0.0f, kUnder);
        c.image(kChecker, { 8.25f, 8.25f, 32.0f, 32.0f });
        c.rrect(30.0f, 30.0f, 10.0f, 10.0f, 0.0f, kOver);
        c.image(kPlain, { 44.0f, 6.0f, 30.0f, 24.0f }, 8.0f);
        c.pushClip({ 50.0f, 34.0f, 20.0f, 20.0f });
        c.image(kPlain, { 44.0f, 32.0f, 30.0f, 24.0f }, 0.0f, 0.5f);
        c.popClip();
        c.image(kRow, { 8.0f, 44.0f, 32.0f, 8.0f }, 0.0f, 1.0f, { 0.5f, 0.0f, 0.5f, 1.0f });
        c.image(kFine, { 72.0f, 50.0f, 6.0f, 6.0f });
        c.image(kMissing, { 0.0f, 0.0f, 6.0f, 6.0f });
    }

    // Every pixel's largest channel difference against SoftRaster's image: the largest, and how many exceed 2.
    void compare(const funkgui::Image& gpu, const funkgui::Image& cpu, int& worst, int& over2)
    {
        worst = 0;
        over2 = 0;
        if (gpu.rgba.size() != cpu.rgba.size())
        {
            worst = 255;
            return;
        }
        for (size_t i = 0; i < gpu.rgba.size(); i += 4)
        {
            int d = 0;
            for (size_t c = 0; c < 3; ++c)
                d = std::max(d, std::abs(static_cast<int>(gpu.rgba[i + c]) - static_cast<int>(cpu.rgba[i + c])));
            worst = std::max(worst, d);
            over2 += d > 2 ? 1 : 0;
        }
    }
}

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;      // the font atlas bakes through JUCE
    T::Probe P("fg.image.draw", "", argc, argv);

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    const bool metal = device != nil;
    [device release];
    P.note("metal", metal ? "\"present\"" : "\"absent\"");
    if (!metal)
    {
        std::printf("INFO     no Metal device: nothing to draw on, every row skipped\n");
        return P.finish();
    }

    registerPictures();
    auto& ctx = funkgui::BgfxContext::get();
    if (!P.eq("context.acquired_headless", ctx.acquire(nullptr, 0, 0), 1))
        return P.finish();
    P.eq("context.metal", bgfx::getRendererType() == bgfx::RendererType::Metal, 1);

    funkgui::Canvas canvas(ctx.font());
    funkgui::BgfxSink sink;
    Target target;
    P.eq("context.target", target.make(), 1);

    canvas.begin(frame());
    const PrimList none = canvas.end();
    const int base = target.draw(sink, none);
    P.eq("context.base_frame", base >= 0 && sink.lastDrawCalls() == 0, 1);
    P.eq("textures.none_before", funkgui::BgfxSink::imageTextureCount(), 0);

    // ---- draw -------------------------------------------------------------------------------------------------------
    canvas.begin(frame());
    scene(canvas);
    const PrimList list = canvas.end();
    P.eq("draw.recorded", list.images.size() == 5 && list.missingImages == 1, 1);
    const int draws = target.draw(sink, list);
    P.eq("draw.read_back", draws >= 0, 1);
    if (funkgui::writePng(target.image(), "image-draw.png"))   // in the test's sandbox (the working directory)
        std::printf("INFO     wrote image-draw.png\n");
    P.eq("draw.sink_calls", sink.lastDrawCalls(), 7);
    P.eq("draw.bgfx_calls", draws - base, 7);
    P.eq("textures.one_per_picture", funkgui::BgfxSink::imageTextureCount(), 4);

    // The checker: dst 8.25 .. 40.25; texel centres at a quarter and three quarters of it.
    P.eq("draw.checker_red", target.is(12.0f, 12.0f, kRed), 1);
    P.eq("draw.checker_green", target.is(36.0f, 12.0f, kGreen), 1);
    P.eq("draw.checker_blue", target.is(12.0f, 34.0f, kBlue), 1);
    // Device pixel 48's centre is logical 24.25: dst + 16, the midpoint between the two top texels.
    P.eq("draw.checker_midpoint", target.near(24.0f, 12.0f, 127.5, 127.5, 0.0), 1);
    P.eq("draw.covered", target.is(35.0f, 35.0f, kOver), 1);
    P.eq("draw.corner_transparent", target.is(44.0f, 6.0f, kUnder), 1);
    P.eq("draw.corner_inside", target.is(50.0f, 12.0f, kWhite), 1);
    P.eq("draw.opacity_in_clip", target.near(55.0f, 40.0f, over(255, kUnder.r, 0.5), over(255, kUnder.g, 0.5),
                                             over(255, kUnder.b, 0.5)), 1);
    P.eq("draw.clip_left_edge_out", target.is(49.75f, 40.0f, kUnder), 1);
    P.eq("draw.clip_left_edge_in", target.near(50.0f, 40.0f, over(255, kUnder.r, 0.5), over(255, kUnder.g, 0.5),
                                               over(255, kUnder.b, 0.5)), 1);
    P.eq("draw.clip_bottom_out", target.is(55.0f, 54.0f, kUnder), 1);
    P.eq("draw.crop_blue", target.is(10.0f, 48.0f, kBlue), 1);
    P.eq("draw.crop_yellow", target.is(38.0f, 48.0f, kYellow), 1);
    P.eq("draw.downscaled_grey", target.near(74.5f, 52.5f, 128.0, 128.0, 128.0)
                                     && target.near(75.5f, 53.5f, 128.0, 128.0, 128.0), 1);
    P.eq("draw.missing_draws_nothing", target.is(2.0f, 2.0f, kUnder), 1);

    // ---- parity with SoftRaster -------------------------------------------------------------------------------------
    {
        const funkgui::Image cpu = funkgui::rasterise(list, ctx.font(), 1);
        int worst = 0, over2 = 0;
        compare(target.image(), cpu, worst, over2);
        std::printf("INFO     against SoftRaster: largest difference %d, %d pixels over 2 of %d\n", worst, over2,
                    kPw * kPh);
        P.eq("parity.worst_within_4", worst <= 4, 1);
        P.eq("parity.over_2_under_1_percent", over2 * 100 <= kPw * kPh, 1);
    }

    // ---- textures: replaced, removed --------------------------------------------------------------------------------
    setPlain(kRed);
    target.draw(sink, list);
    P.eq("textures.replaced_draws_new_pixels", target.is(50.0f, 12.0f, kRed), 1);
    P.eq("textures.replaced_old_gone", funkgui::BgfxSink::imageTextureCount(), 4);
    images::remove(kRow);
    target.draw(sink, list);
    P.eq("textures.removed_destroyed", funkgui::BgfxSink::imageTextureCount(), 3);
    P.eq("textures.removed_draws_nothing", sink.lastDrawCalls() == 6 && target.is(10.0f, 48.0f, kUnder), 1);
    canvas.begin(frame());
    canvas.rrect(-4.0f, -4.0f, kW + 8.0f, kH + 8.0f, 0.0f, kUnder);
    const PrimList plain = canvas.end();
    images::remove(kFine);
    target.draw(sink, plain);
    P.eq("textures.swept_without_images_in_frame", funkgui::BgfxSink::imageTextureCount(), 2);
    P.eq("textures.plain_frame_one_call", sink.lastDrawCalls(), 1);
    registerPictures();

    // ---- restart ----------------------------------------------------------------------------------------------------
    target.drop();
    ctx.release(nullptr);
    P.eq("restart.down", !ctx.valid(), 1);
    P.eq("restart.textures_gone", funkgui::BgfxSink::imageTextureCount(), 0);
    P.eq("restart.still_registered", images::has(kChecker) && images::has(kPlain), 1);
    if (P.eq("restart.acquired", ctx.acquire(nullptr, 0, 0), 1) && P.eq("restart.target", target.make(), 1))
    {
        const int again = target.draw(sink, list);
        P.eq("restart.bgfx_calls", again - base, 7);
        P.eq("restart.uploaded_again", funkgui::BgfxSink::imageTextureCount(), 4);
        P.eq("restart.draws", target.is(12.0f, 12.0f, kRed) && target.is(50.0f, 12.0f, kWhite), 1);
        target.drop();
        ctx.release(nullptr);
    }
    P.eq("restart.released", !ctx.valid(), 1);
    for (const uint32_t id : { kChecker, kRow, kPlain, kFine })
        images::remove(id);
    return P.finish();
}

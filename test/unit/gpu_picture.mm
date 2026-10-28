// FUNKGUI_TEST name=fg.gpu.picture timeout=300 gpu=1
//
// fg.gpu.picture (v0.23.0; gpu/GpuPicture.h): GPU pictures with no window in the process, on Metal. A frame recorded by
// a Canvas (an rrect panel, text, a segment, a filled area, a disc, a picture of the registry, a custom shader with an
// rrect over its corner, text after it) is drawn offscreen by renderGpuPicture() and read back, and compared with what
// the shader must have drawn and with SoftRaster's image of the same list. Needs a Metal device, not a window server;
// without one (a VM) every row is skipped and noted. Spec rows only.
//
// - nowindow.*  no window, called on the message thread: bgfx starts headless on first use and is held; the size is
//               SoftRaster's; 4 draw calls; every pixel of the shader's region is the shader's colour (v_uv, the
//               params), none is the placeholder, the rrect after it covers it; away from the shader every pixel is
//               within fg.image.draw's bounds of SoftRaster (none off by more than 4, at most 1 % by more than 2);
//               opaque. The pictures are written to gpu-picture.png and gpu-picture-softraster.png in the sandbox.
// - again.*     a second call reuses the hold and draws the same bytes.
// - scale.*     the same list recorded at dpi 1.25: SoftRaster's size, within the same bounds away from the shader.
// - size.*      a 0 x 0 frame and one over 16384 px are badSize, and start nothing.
// - thread.*    from another thread while the message thread turns its run loop: posted, drawn, the same bytes; while
//               the message thread is blocked: timedOut after the timeout, no deadlock, and the job, reached later, is
//               dropped (abandoned) and draws nothing; from a thread that holds the MessageManagerLock: timedOut at
//               once.
// - tick.*      from the render thread, inside a frame: a FrameClient on the FramePump (its fallback timer, on the
//               message thread) first submits a frame of its own, as an editor earlier in the pump's list, then asks
//               for the picture inside the same tick: the picture is drawn inline (the same bytes), and the client's
//               own frame, presented by the picture's bgfx::frame(), reads back as drawn.
// - shared.*    bgfx started by someone else (BgfxContext acquired headless by the test, as an editor holds it): the
//               picture draws on it, starts and holds nothing, the same bytes.
// - release.*   releaseGpuPictureContext(): bgfx shuts down, the shader and the picture stay registered; the next call
//               starts it again and draws the same bytes; a release from another thread.
// - shutdown.*  a hold left at JUCE's shutdown (the last ScopedJuceInitialiser_GUI) is released: bgfx is down after it.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/Images.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/canvas/SoftRaster.h>
#include <funkgui/core/Col.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/gpu/BgfxContext.h>
#include <funkgui/gpu/BgfxSink.h>
#include <funkgui/gpu/FramePump.h>
#include <funkgui/gpu/GpuPicture.h>
#include <funkgui/gpu/ShaderLibrary.h>
#include <funkgui/shaders/fs_custom_test.mtl.h>
#include <funkgui/test/Harness.h>
#include <funkgui/text/FontService.h>

#include <bgfx/bgfx.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <atomic>
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
using funkgui::GpuPicture;
using funkgui::Image;
using funkgui::PrimList;
using FRect = funkgui::Rect;   // macOS has a ::Rect of its own (MacTypes.h)

namespace
{
    constexpr int kW = 160, kH = 100;               // logical
    constexpr float kDpi = 2.0f;
    constexpr int kPw = 320, kPh = 200;             // physical
    constexpr Col kClear{ 18, 20, 24, 255 };
    constexpr Col kPanel{ 40, 60, 80, 255 };
    constexpr Col kBorder{ 120, 130, 140, 255 };
    constexpr Col kInk{ 230, 232, 236, 255 };
    constexpr Col kAccent{ 200, 120, 40, 255 };
    constexpr Col kOver{ 40, 200, 40, 255 };
    constexpr Col kRed{ 255, 0, 0, 255 }, kGreen{ 0, 255, 0, 255 }, kBlue{ 0, 0, 255, 255 };
    constexpr Col kWhite{ 255, 255, 255, 255 };
    constexpr uint32_t kShaderId = 7, kChecker = 11;
    constexpr bgfx::ViewId kOwnView = 1, kOwnBlitView = 2;   // the tick client's, as an editor's

    const FRect kShaderRect{ 70.0f, 40.0f, 40.0f, 30.0f };    // whole device px at dpi 2
    const FRect kOverRect{ 100.0f, 60.0f, 20.0f, 16.0f };     // over the shader's bottom-right corner

    FrameInfo frame(float dpi)
    {
        FrameInfo f;
        f.logicalW = kW;
        f.logicalH = kH;
        f.dpi = dpi;
        f.clear = kClear;
        f.textGamma = 0.714285731f;
        f.fixedClock = true;
        return f;
    }

    void scene(funkgui::Canvas& c)
    {
        c.rrect(4.0f, 4.0f, kW - 8.0f, kH - 8.0f, 6.0f, kPanel, 1.0f, kBorder);
        c.text("GPU PICTURE 0.23", 10.0f, 9.0f, funkgui::type::kLabel, kInk);
        c.segment(10.0f, 28.0f, 60.0f, 34.0f, 1.5f, kInk);
        c.area(66.0f, 120.0f, 30.0f, 22.0f, 36.0f, 36.0f, kAccent, 1.0f, kInk);
        c.disc(138.0f, 22.0f, 8.0f, kBorder, 1.5f, kInk);
        c.image(kChecker, { 10.0f, 44.0f, 24.0f, 24.0f }, 3.0f);
        const float params[4] = { 0.0f, 0.0f, 0.25f, 1.0f };
        c.shader(kShaderId, kShaderRect, params, 4);
        c.rrect(kOverRect.x, kOverRect.y, kOverRect.w, kOverRect.h, 0.0f, kOver);
        c.text("SHADER", 10.0f, 78.0f, funkgui::type::kCaption, kInk);
    }

    PrimList record(float dpi)
    {
        funkgui::Canvas canvas(funkgui::FontService::get().atlas());
        canvas.begin(frame(dpi));
        scene(canvas);
        return canvas.end();
    }

    void registerChecker()
    {
        const Col px[4] = { kRed, kGreen, kBlue, kWhite };
        uint8_t rgba[16];
        for (int i = 0; i < 4; ++i)
        {
            rgba[i * 4] = px[i].r;
            rgba[i * 4 + 1] = px[i].g;
            rgba[i * 4 + 2] = px[i].b;
            rgba[i * 4 + 3] = px[i].a;
        }
        funkgui::images::set(kChecker, 2, 2, rgba);
    }

    const uint8_t* at(const Image& img, int px, int py)
    {
        return &img.rgba[(static_cast<size_t>(py) * static_cast<size_t>(img.w) + static_cast<size_t>(px)) * 4u];
    }

    // What fs_custom_test draws at the device pixel (px, py) of the dpi-2 frame: (v_uv, b), opaque.
    void expectShader(int px, int py, double out[3])
    {
        const double x = (px + 0.5) / kDpi, y = (py + 0.5) / kDpi;
        out[0] = (x - kShaderRect.x) / kShaderRect.w * 255.0;
        out[1] = (y - kShaderRect.y) / kShaderRect.h * 255.0;
        out[2] = 0.25 * 255.0;
    }

    bool insideRect(const FRect& r, double x, double y)
    {
        return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
    }

    bool near(const uint8_t* p, Col c, int tol)
    {
        return std::abs(p[0] - c.r) <= tol && std::abs(p[1] - c.g) <= tol && std::abs(p[2] - c.b) <= tol;
    }

    // Every pixel's largest RGB difference against SoftRaster, over the pixels whose centre lies outside the shader's
    // rect grown by `grow` device px: the largest, how many exceed 2, and how many were compared.
    struct Parity
    {
        int worst = 0, over2 = 0, counted = 0, worstX = 0, worstY = 0;
        bool sameSize = false;
        bool ok() const { return sameSize && worst <= 4 && over2 * 100 <= counted; }
    };

    Parity parity(const Image& gpu, const Image& cpu, float dpi, float grow)
    {
        Parity p;
        p.sameSize = gpu.w == cpu.w && gpu.h == cpu.h && gpu.rgba.size() == cpu.rgba.size() && gpu.w > 0;
        if (!p.sameSize)
            return p;
        const FRect skip{ kShaderRect.x - grow / dpi, kShaderRect.y - grow / dpi, kShaderRect.w + 2.0f * grow / dpi,
                         kShaderRect.h + 2.0f * grow / dpi };
        for (int y = 0; y < gpu.h; ++y)
            for (int x = 0; x < gpu.w; ++x)
            {
                if (insideRect(skip, (x + 0.5) / dpi, (y + 0.5) / dpi))
                    continue;
                const uint8_t* a = at(gpu, x, y);
                const uint8_t* b = at(cpu, x, y);
                int d = 0;
                for (int c = 0; c < 3; ++c)
                    d = std::max(d, std::abs(static_cast<int>(a[c]) - static_cast<int>(b[c])));
                if (d > p.worst)
                {
                    p.worst = d;
                    p.worstX = x;
                    p.worstY = y;
                }
                p.over2 += d > 2 ? 1 : 0;
                ++p.counted;
            }
        return p;
    }

    void print(const char* what, const Parity& p)
    {
        std::printf("INFO     %s against SoftRaster: largest difference %d (at %d,%d), %d of %d pixels over 2\n", what,
                    p.worst, p.worstX, p.worstY, p.over2, p.counted);
    }

    bool allOpaque(const Image& img)
    {
        for (size_t i = 3; i < img.rgba.size(); i += 4u)
            if (img.rgba[i] != 255u)
                return false;
        return !img.rgba.empty();
    }

    void spin(double seconds)
    {
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
    }

    // Turns the run loop until flag is set (at most `seconds`).
    bool spinUntil(const std::atomic<bool>& flag, double seconds)
    {
        const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
        while (!flag.load() && std::chrono::steady_clock::now() < end)
            spin(0.01);
        return flag.load();
    }

    double msSince(std::chrono::steady_clock::time_point t0)
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }

    // An offscreen target with a read-back, as fg.shader.draw's: the tick client's own "editor" frame.
    struct Target
    {
        bgfx::TextureHandle     colour = BGFX_INVALID_HANDLE;
        bgfx::TextureHandle     readback = BGFX_INVALID_HANDLE;
        bgfx::FrameBufferHandle fb = BGFX_INVALID_HANDLE;
        std::vector<uint8_t>    bgra = std::vector<uint8_t>(static_cast<size_t>(kPw) * kPh * 4u, 0x7fu);

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
            bgfx::setViewFrameBuffer(kOwnView, BGFX_INVALID_HANDLE);
            if (bgfx::isValid(fb)) bgfx::destroy(fb);
            if (bgfx::isValid(readback)) bgfx::destroy(readback);
            if (bgfx::isValid(colour)) bgfx::destroy(colour);
            fb = BGFX_INVALID_HANDLE;
            readback = colour = BGFX_INVALID_HANDLE;
        }

        // The pixel at device (px, py) as RGB.
        bool is(int px, int py, Col c) const
        {
            const uint8_t* p = &bgra[(static_cast<size_t>(py) * kPw + static_cast<size_t>(px)) * 4u];
            return p[2] == c.r && p[1] == c.g && p[0] == c.b;
        }
    };

    // The tick client: an editor-like FrameClient that asks for the picture inside the pump's frame (see the header).
    class TickClient final : public funkgui::FrameClient
    {
    public:
        const PrimList* list = nullptr;
        GpuPicture picture;
        Target own;
        bool targetMade = false;
        std::atomic<bool> asked{ false }, settled{ false };
        int framesAfter = 0;

        Result submitFrame(float) override
        {
            Result r;
            if (!asked.load())
            {
                if (!funkgui::BgfxContext::get().valid())
                    return r;
                targetMade = own.make();
                if (targetMade)
                {
                    // 1. Its own frame, submitted and not yet rendered, as an editor earlier in the pump's list.
                    funkgui::Canvas canvas(funkgui::FontService::get().atlas());
                    canvas.begin(frame(kDpi));
                    canvas.rrect(-4.0f, -4.0f, kW + 8.0f, kH + 8.0f, 0.0f, kOver);
                    sink_.submit(canvas.end(), kOwnView, own.fb, kPw, kPh);
                }
                // 2. The picture, inside the tick, on the thread that drives bgfx.
                picture = funkgui::renderGpuPicture(*list);
                // 3. Its own frame's read-back, after the picture's frames.
                if (targetMade)
                {
                    bgfx::touch(kOwnBlitView);
                    bgfx::blit(kOwnBlitView, own.readback, 0, 0, own.colour);
                    static_cast<void>(bgfx::readTexture(own.readback, own.bgra.data()));
                }
                asked.store(true);
                r.submitted = true;
                return r;
            }
            if (framesAfter < 6)
            {
                ++framesAfter;                       // the pump's bgfx::frame() lands the read
                r.submitted = true;
                return r;
            }
            settled.store(true);
            return r;
        }

        void* frameClockView() const override { return nullptr; }   // no view: the pump's fallback timer

    private:
        funkgui::BgfxSink sink_;
    };

    struct ThreadResult
    {
        GpuPicture picture;
        double ms = 0.0;
        bool locked = false;
    };
}

int main(int argc, char** argv)
{
    T::Probe P("fg.gpu.picture", "", argc, argv);
    auto& ctx = funkgui::BgfxContext::get();
    {
        const juce::ScopedJuceInitialiser_GUI juceInit;      // the message thread (this one) and the font's bake

        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        const bool metal = device != nil;
        [device release];
        P.note("metal", metal ? "\"present\"" : "\"absent\"");
        if (!metal)
        {
            std::printf("INFO     no Metal device: nothing to draw on, every row skipped\n");
            return P.finish();
        }

        P.eq("api.supported", funkgui::gpuPicturesSupported(), 1);
        P.eq("api.status_names", std::string(funkgui::toString(GpuPicture::Status::timedOut)) == "timedOut"
                                     && std::string(funkgui::toString(GpuPicture::Status::ok)) == "ok", 1);
        P.eq("nowindow.bgfx_down_before", ctx.valid() ? 1 : 0, 0);
        P.eq("nowindow.not_held_before", funkgui::gpuPictureContextHeld(), 0);

        // The shader and the picture: registered once, before bgfx runs (both registries keep them across restarts).
        std::string why;
        P.eq("nowindow.shader_registered", funkgui::shaders::set(kShaderId, fs_custom_test_mtl,
                                                                 sizeof fs_custom_test_mtl, why), 1);
        if (!why.empty())
            std::printf("INFO     set: %s\n", why.c_str());
        registerChecker();

        const PrimList list = record(kDpi);
        P.eq("nowindow.recorded", list.shaders.size() == 1 && list.images.size() == 1 && list.missingImages == 0, 1);
        const Image cpu = funkgui::rasterise(list, funkgui::FontService::get().atlas(), 1);

        // ---- nowindow: the first picture starts bgfx headless and holds it ------------------------------------------
        const auto counters0 = funkgui::gpuPictureCounters();
        const GpuPicture first = funkgui::renderGpuPicture(list);
        std::printf("INFO     first picture: %s, %d x %d, %u draw calls, started %d\n", funkgui::toString(first.status),
                    first.image.w, first.image.h, first.drawCalls, first.startedContext ? 1 : 0);
        if (!P.eq("nowindow.ok", first.ok(), 1))
            return P.finish();
        P.eq("nowindow.started_context", first.startedContext, 1);
        P.eq("nowindow.held", funkgui::gpuPictureContextHeld(), 1);
        P.eq("nowindow.bgfx_up", ctx.valid() ? 1 : 0, 1);
        P.eq("nowindow.metal", bgfx::getRendererType() == bgfx::RendererType::Metal, 1);
        P.eq("nowindow.size", first.image.w == kPw && first.image.h == kPh && cpu.w == kPw && cpu.h == kPh, 1);
        P.eq("nowindow.draw_calls", first.drawCalls, 4);   // the run, the picture, the shader, the run after
        P.eq("nowindow.counted", funkgui::gpuPictureCounters().drawn - counters0.drawn, 1);
        P.eq("nowindow.opaque", allOpaque(first.image), 1);
        if (funkgui::writePng(first.image, "gpu-picture.png") && funkgui::writePng(cpu, "gpu-picture-softraster.png"))
            std::printf("INFO     wrote gpu-picture.png and gpu-picture-softraster.png\n");

        // The shader's region (device px 140..219 x 80..139): every pixel the shader's own colour, none the
        // placeholder, except where the rrect drawn after it lies: inside that rrect, its colour; on its antialiased
        // edge (one device px either side), a blend of the two, not checked.
        {
            const float px1 = 1.0f / kDpi;
            const FRect overIn{ kOverRect.x + px1, kOverRect.y + px1, kOverRect.w - 2.0f * px1,
                                kOverRect.h - 2.0f * px1 };
            const FRect overOut{ kOverRect.x - px1, kOverRect.y - px1, kOverRect.w + 2.0f * px1,
                                 kOverRect.h + 2.0f * px1 };
            int shaderPx = 0, wrong = 0, placeholder = 0, covered = 0, coveredWrong = 0, cpuPlaceholder = 0;
            for (int py = 0; py < kPh; ++py)
                for (int px = 0; px < kPw; ++px)
                {
                    const double x = (px + 0.5) / kDpi, y = (py + 0.5) / kDpi;
                    if (!insideRect(kShaderRect, x, y))
                        continue;
                    const uint8_t* g = at(first.image, px, py);
                    if (insideRect(overIn, x, y))
                    {
                        ++covered;
                        coveredWrong += near(g, kOver, 1) ? 0 : 1;
                        continue;
                    }
                    if (insideRect(overOut, x, y))
                        continue;                    // the later rrect's soft edge
                    ++shaderPx;
                    const uint8_t* c = at(cpu, px, py);
                    if (near(c, funkgui::shaderPlaceholder::fill, 0) || near(c, funkgui::shaderPlaceholder::stripe, 0)
                        || near(c, funkgui::shaderPlaceholder::border, 0))
                        ++cpuPlaceholder;
                    double want[3];
                    expectShader(px, py, want);
                    bool ok = true;
                    for (int ch = 0; ch < 3; ++ch)
                        ok = ok && std::abs(g[ch] - want[ch]) <= 2.0;
                    wrong += ok ? 0 : 1;
                    placeholder += near(g, funkgui::shaderPlaceholder::fill, 2)
                                           || near(g, funkgui::shaderPlaceholder::stripe, 2)
                                           || near(g, funkgui::shaderPlaceholder::border, 2)
                                       ? 1
                                       : 0;
                }
            std::printf("INFO     shader region: %d pixels, %d off the shader's colour by more than 2, %d placeholder-"
                        "coloured (SoftRaster: %d); %d inside the later rrect (%d wrong)\n",
                        shaderPx, wrong, placeholder, cpuPlaceholder, covered, coveredWrong);
            constexpr int kShaderOnly = 80 * 60 - 21 * 21, kCovered = 19 * 19;
            P.eq("nowindow.shader_pixels", shaderPx, kShaderOnly);
            P.eq("nowindow.shader_drawn", wrong, 0);
            P.eq("nowindow.shader_not_placeholder", placeholder, 0);
            P.eq("nowindow.softraster_has_placeholder", cpuPlaceholder, kShaderOnly);
            P.eq("nowindow.shader_covered_by_later_rrect", covered == kCovered && coveredWrong == 0, 1);
        }
        P.eq("nowindow.image_red", near(at(first.image, 26, 94), kRed, 1), 1);       // logical (13, 47): a quarter in
        P.eq("nowindow.image_white", near(at(first.image, 62, 130), kWhite, 1), 1);  // logical (31, 65)
        {
            const Parity p = parity(first.image, cpu, kDpi, 0.0f);
            print("nowindow", p);
            P.eq("nowindow.parity_size", p.sameSize, 1);
            P.eq("nowindow.parity_worst_within_4", p.worst <= 4, 1);
            P.eq("nowindow.parity_over_2_under_1_percent", p.over2 * 100 <= p.counted, 1);
        }

        // ---- again: the hold is reused ------------------------------------------------------------------------------
        {
            const GpuPicture again = funkgui::renderGpuPicture(list);
            P.eq("again.ok", again.ok(), 1);
            P.eq("again.reused", !again.startedContext && funkgui::gpuPictureContextHeld(), 1);
            P.eq("again.same_bytes", again.image.rgba == first.image.rgba, 1);
        }

        // ---- scale: dpi 1.25 ----------------------------------------------------------------------------------------
        {
            const PrimList scaled = record(1.25f);
            const Image cpuScaled = funkgui::rasterise(scaled, funkgui::FontService::get().atlas(), 1);
            const GpuPicture pic = funkgui::renderGpuPicture(scaled);
            P.eq("scale.ok", pic.ok(), 1);
            P.eq("scale.size", pic.image.w == 200 && pic.image.h == 125 && cpuScaled.w == 200 && cpuScaled.h == 125, 1);
            const Parity p = parity(pic.image, cpuScaled, 1.25f, 1.0f);
            print("scale 1.25", p);
            P.eq("scale.parity", p.ok(), 1);
        }

        // ---- size: nothing to draw, or too large --------------------------------------------------------------------
        {
            PrimList empty;
            empty.info = frame(kDpi);
            empty.info.logicalW = 0;
            P.eq("size.zero_is_bad", funkgui::renderGpuPicture(empty).status == GpuPicture::Status::badSize, 1);
            empty.info.logicalW = 9000;
            empty.info.logicalH = 10;
            P.eq("size.huge_is_bad", funkgui::renderGpuPicture(empty).status == GpuPicture::Status::badSize, 1);
        }

        // ---- thread: posted from another thread ---------------------------------------------------------------------
        {
            ThreadResult r;
            std::atomic<bool> done{ false };
            std::thread t([&]
            {
                funkgui::GpuPictureOptions o;
                o.timeoutMs = 10000;
                r.picture = funkgui::renderGpuPicture(list, o);
                done.store(true);
            });
            spinUntil(done, 15.0);
            t.join();
            P.eq("thread.posted_ok", r.picture.ok(), 1);
            P.eq("thread.same_bytes", r.picture.image.rgba == first.image.rgba, 1);
        }
        {
            // The message thread blocked (joining the caller, not turning its loop): the call times out, nothing hangs.
            const auto before = funkgui::gpuPictureCounters();
            ThreadResult r;
            std::thread t([&]
            {
                funkgui::GpuPictureOptions o;
                o.timeoutMs = 300;
                const auto t0 = std::chrono::steady_clock::now();
                r.picture = funkgui::renderGpuPicture(list, o);
                r.ms = msSince(t0);
            });
            t.join();
            std::printf("INFO     blocked message thread: %s after %.0f ms\n", funkgui::toString(r.picture.status),
                        r.ms);
            P.eq("thread.blocked_times_out", r.picture.status == GpuPicture::Status::timedOut, 1);
            P.eq("thread.blocked_waited_the_timeout", r.ms >= 290.0 && r.ms < 5000.0, 1);
            spin(0.3);                                 // the message thread reaches the job: dropped
            const auto after = funkgui::gpuPictureCounters();
            P.eq("thread.abandoned_job_dropped", after.abandoned - before.abandoned, 1);
            P.eq("thread.abandoned_job_drew_nothing", after.drawn - before.drawn, 0);
        }
        {
            // A thread that holds the MessageManagerLock: the message thread waits for it, so the call returns at once.
            ThreadResult r;
            std::atomic<bool> done{ false };
            std::thread t([&]
            {
                const juce::MessageManagerLock mml;
                r.locked = mml.lockWasGained();
                if (r.locked)
                {
                    funkgui::GpuPictureOptions o;
                    o.timeoutMs = 5000;
                    const auto t0 = std::chrono::steady_clock::now();
                    r.picture = funkgui::renderGpuPicture(list, o);
                    r.ms = msSince(t0);
                }
                done.store(true);
            });
            spinUntil(done, 15.0);
            t.join();
            std::printf("INFO     lock holder: %s after %.1f ms\n", funkgui::toString(r.picture.status), r.ms);
            P.eq("thread.lock_holder_locked", r.locked, 1);
            P.eq("thread.lock_holder_times_out_at_once",
                 r.picture.status == GpuPicture::Status::timedOut && r.ms < 100.0, 1);
        }

        // ---- tick: from the render thread, inside the pump's frame --------------------------------------------------
        {
            TickClient client;
            client.list = &list;
            auto& pump = funkgui::FramePump::get();
            pump.add(&client);
            spinUntil(client.settled, 10.0);
            pump.remove(&client);
            std::printf("INFO     inside a tick: %s, %u draw calls\n", funkgui::toString(client.picture.status),
                        client.picture.drawCalls);
            P.eq("tick.asked_inside_a_tick", client.asked.load(), 1);
            P.eq("tick.ok", client.picture.ok(), 1);
            P.eq("tick.same_bytes", client.picture.image.rgba == first.image.rgba, 1);
            P.eq("tick.own_frame_target", client.targetMade, 1);
            P.eq("tick.own_frame_drawn", client.own.is(10, 10, kOver) && client.own.is(kPw - 10, kPh - 10, kOver), 1);
            client.own.drop();
        }

        // ---- release: the hold let go, and taken again --------------------------------------------------------------
        P.eq("release.released", funkgui::releaseGpuPictureContext(), 1);
        P.eq("release.not_held", funkgui::gpuPictureContextHeld(), 0);
        P.eq("release.bgfx_down", ctx.valid() ? 1 : 0, 0);
        P.eq("release.still_registered", funkgui::shaders::has(kShaderId) && funkgui::images::has(kChecker), 1);
        {
            const GpuPicture back = funkgui::renderGpuPicture(list);
            P.eq("release.restarted", back.ok() && back.startedContext && funkgui::gpuPictureContextHeld(), 1);
            P.eq("release.same_bytes", back.image.rgba == first.image.rgba, 1);
        }
        {
            std::atomic<bool> done{ false };
            bool released = false;
            std::thread t([&]
            {
                released = funkgui::releaseGpuPictureContext(10000);
                done.store(true);
            });
            spinUntil(done, 15.0);
            t.join();
            P.eq("release.from_another_thread", released && !funkgui::gpuPictureContextHeld() && !ctx.valid(), 1);
        }

        // ---- shared: bgfx held by someone else ----------------------------------------------------------------------
        if (P.eq("shared.acquired", ctx.acquire(nullptr, 0, 0), 1))
        {
            const GpuPicture pic = funkgui::renderGpuPicture(list);
            P.eq("shared.ok", pic.ok(), 1);
            P.eq("shared.started_nothing", !pic.startedContext && !funkgui::gpuPictureContextHeld(), 1);
            P.eq("shared.same_bytes", pic.image.rgba == first.image.rgba, 1);
            ctx.release(nullptr);
            P.eq("shared.down_with_its_owner", ctx.valid() ? 1 : 0, 0);
        }

        // ---- shutdown: a hold left for JUCE's shutdown --------------------------------------------------------------
        P.eq("shutdown.held_again", funkgui::renderGpuPicture(list).ok() && funkgui::gpuPictureContextHeld(), 1);
    }                                                // ~ScopedJuceInitialiser_GUI: DeletedAtShutdown::deleteAll()
    P.eq("shutdown.released", funkgui::gpuPictureContextHeld(), 0);
    P.eq("shutdown.bgfx_down", ctx.valid() ? 1 : 0, 0);
    funkgui::shaders::remove(kShaderId);
    funkgui::images::remove(kChecker);
    return P.finish();
}

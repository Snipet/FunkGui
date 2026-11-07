// FUNKGUI_TEST name=fg.gpu.instancing timeout=600 gpu=1
//
// fg.gpu.instancing (v0.26.0; gpu/BgfxSink.h "primitives as instances"): the instanced UI program draws what the
// vertex path draws, pixel for pixel, on Metal with no window (BgfxContext acquired headless, as fg.image.draw). Every
// frame of instance_scenes.h (every kind and variant, every colour byte in every channel of both colours, 5,000 seeded
// primitives, a frame split into three runs by two image draws) is submitted by BgfxSink twice, with
// BgfxContext::Config::instancing off (expand()'s six vertices per primitive, the path before v0.26.0) and on, into
// offscreen framebuffers that are blitted and read back:
// - <scene>.<dpi>.bgra8_identical: the BGRA8 frames (the backbuffer's format) are equal byte for byte;
// - <scene>.<dpi>.float_identical: the RGBA32F frames are equal bit for bit, so no varying the fragment shader read
//   differed by as much as one unit in the last place (an 8-bit target would round most such differences away); with
//   the colours scene this checks that u_unorm8 (canvas/Expand.h unorm8Values) holds the very values Metal's vertex
//   fetch makes of every normalised colour byte;
// - .drawn: the frame is not the clear colour alone; .geometry: instances and vertices as configured; .draws and
//   .vertices: the same draw calls (BgfxSink's count and bgfx's) and vertices both ways; .bytes: what each path copied
//   (80 per primitive as instances, 384 as vertices, 384 per image draw).
// Also: context.instancing (bgfx reports instancing and took the program), fallback.* (a frame holding a primitive
// whose kind is not instanceable draws as vertices, the same pixels), restart.* (the program, quad and table are made
// again when bgfx starts again, and draw the same pixels), overflow.* (bgfx started again with a 4,224-byte transient
// buffer, frames submitted with no bgfx::frame() between them as editors share one: a frame that does not fit as
// vertices drops and takes nothing, so one that fills the buffer to its last byte fits after it; records into that
// full buffer drop; a frame whose records would fit but whose image draws' vertices would not drops and takes
// nothing, so a smaller frame of records fits after it). Needs a Metal device, not a window server; without one (a
// VM) every row is skipped and noted. The float rows are skipped and noted where bgfx offers no RGBA32F framebuffer.
// Spec rows only. The instanced kinds and stress frames are written to gpu-instancing-<scene>.png in the sandbox.
//
// Measuring (no rows; INFO lines): the stress frame is drawn 200 times each way after a warm-up, timing
// BgfxSink::submit() and the bgfx::frame() that renders it (single-threaded bgfx: the encoding and the copy of the
// transient buffers into Metal's happen there) on this thread, and printing their medians with the bytes uploaded.
// Run by hand with --dumps <dir> [--frames <n>] (taken out of argv before the harness sees it), every <dir>/*.dump
// (FunkGuiGalleryProbe --dump-dir writes them) is compared both ways as above and measured the same way, n frames
// each (default 200); its rows are dump.<file>.* (spec rows, so a hand run fails on a difference).

#import <Metal/Metal.h>

#include "instance_scenes.h"

#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/Expand.h>
#include <funkgui/canvas/Images.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/canvas/SoftRaster.h>
#include <funkgui/core/Col.h>
#include <funkgui/gpu/BgfxContext.h>
#include <funkgui/gpu/BgfxSink.h>
#include <funkgui/test/Harness.h>
#include <funkgui/text/FontService.h>

#include <bgfx/bgfx.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace T = funkgui::test;
namespace S = instance_scenes;
namespace images = funkgui::images;
using funkgui::BgfxSink;
using funkgui::PrimList;

namespace
{
    constexpr bgfx::ViewId kDrawView = 1, kBlitView = 2;

    // An offscreen framebuffer of one format, blitted into a texture the CPU reads back.
    struct Target
    {
        int w = 0, h = 0, bpp = 4;
        bgfx::TextureFormat::Enum format = bgfx::TextureFormat::BGRA8;
        bgfx::TextureHandle     colour = BGFX_INVALID_HANDLE;
        bgfx::TextureHandle     readback = BGFX_INVALID_HANDLE;
        bgfx::FrameBufferHandle fb = BGFX_INVALID_HANDLE;
        std::vector<uint8_t>    pixels;

        bool make(int width, int height, bgfx::TextureFormat::Enum f)
        {
            w = width;
            h = height;
            format = f;
            bpp = f == bgfx::TextureFormat::RGBA32F ? 16 : 4;
            pixels.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * static_cast<size_t>(bpp), 0);
            colour = bgfx::createTexture2D(static_cast<uint16_t>(w), static_cast<uint16_t>(h), false, 1, f,
                                           BGFX_TEXTURE_RT);
            readback = bgfx::createTexture2D(static_cast<uint16_t>(w), static_cast<uint16_t>(h), false, 1, f,
                                             BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
            if (bgfx::isValid(colour))
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

        // Submits the list, renders the frame and reads it back. bgfx's draw count of that frame, -1 when the read
        // never completed.
        int draw(BgfxSink& sink, const PrimList& list)
        {
            std::fill(pixels.begin(), pixels.end(), uint8_t{ 0xAB });
            sink.submit(list, kDrawView, fb, w, h);
            bgfx::touch(kBlitView);
            bgfx::blit(kBlitView, readback, 0, 0, colour);
            const uint32_t ready = bgfx::readTexture(readback, pixels.data());
            bgfx::frame();
            uint32_t now = bgfx::frame();                // bgfx's stats are the frame before the last one run
            const int draws = static_cast<int>(bgfx::getStats()->numDraw);
            for (int i = 0; i < 16 && now < ready; ++i)
                now = bgfx::frame();
            return now >= ready ? draws : -1;
        }

        funkgui::Image image() const                 // BGRA8 only
        {
            funkgui::Image img;
            img.w = w;
            img.h = h;
            img.rgba.resize(pixels.size());
            for (size_t i = 0; i + 3 < pixels.size(); i += 4)
            {
                img.rgba[i] = pixels[i + 2];
                img.rgba[i + 1] = pixels[i + 1];
                img.rgba[i + 2] = pixels[i];
                img.rgba[i + 3] = pixels[i + 3];
            }
            return img;
        }
    };

    void setInstancing(bool on)
    {
        auto& ctx = funkgui::BgfxContext::get();
        funkgui::BgfxContext::Config c = ctx.config();
        c.instancing = on;
        static_cast<void>(ctx.configure(c));
    }

    // The pixels that differ (whole pixels of bpp bytes), logging the first.
    int64_t differing(const Target& a, const std::vector<uint8_t>& ref, const std::string& key)
    {
        if (a.pixels.size() != ref.size())
            return -1;
        int64_t n = 0;
        const auto px = static_cast<size_t>(a.bpp);
        for (size_t i = 0; i < ref.size(); i += px)
            if (std::memcmp(&a.pixels[i], &ref[i], px) != 0)
            {
                if (n == 0)
                {
                    const size_t p = i / px;
                    std::printf("INFO     %s: first difference at pixel (%zu, %zu)\n", key.c_str(),
                                p % static_cast<size_t>(a.w), p / static_cast<size_t>(a.w));
                }
                ++n;
            }
        return n;
    }

    // Pixels that are not the clear colour (BGRA8): the frame drew something.
    int64_t drawnPixels(const Target& t, funkgui::Col clear)
    {
        int64_t n = 0;
        for (size_t i = 0; i + 3 < t.pixels.size(); i += 4)
            n += (t.pixels[i] != clear.b || t.pixels[i + 1] != clear.g || t.pixels[i + 2] != clear.r) ? 1 : 0;
        return n;
    }

    struct Pass
    {
        int draws = 0, bgfxDraws = 0;
        uint32_t vertices = 0, bytes = 0;
        BgfxSink::Geometry geometry = BgfxSink::Geometry::instances;
        std::vector<uint8_t> bgra, rgba32f;
        bool read = false;
    };

    Pass run(BgfxSink& sink, const PrimList& list, Target& bgra, Target* f32, bool instancing)
    {
        setInstancing(instancing);
        Pass p;
        p.bgfxDraws = bgra.draw(sink, list);
        p.read = p.bgfxDraws >= 0;
        p.draws = static_cast<int>(sink.lastDrawCalls());
        p.vertices = sink.lastVertexCount();
        p.bytes = sink.lastUploadBytes();
        p.geometry = sink.lastGeometry();
        p.bgra = bgra.pixels;
        if (f32 != nullptr)
        {
            p.read = p.read && f32->draw(sink, list) >= 0;
            p.rgba32f = f32->pixels;
        }
        return p;
    }

    bool floatTargets()
    {
        const bgfx::Caps* caps = bgfx::getCaps();
        const uint32_t need = BGFX_CAPS_FORMAT_TEXTURE_2D | BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER;
        return caps != nullptr && (caps->formats[bgfx::TextureFormat::RGBA32F] & need) == need
               && (caps->supported & BGFX_CAPS_TEXTURE_READ_BACK) != 0;
    }

    // Both paths over one list at one size: the rows above, under key. drawn: at least a twentieth of the pixels are
    // not the clear colour (a scene's frame), or at least one (a dump: a gallery state may draw little).
    void compare(T::Probe& P, BgfxSink& sink, const PrimList& list, int physW, int physH, const std::string& key,
                 const char* pngName = nullptr, bool dump = false)
    {
        Target bgra, f32;
        const bool haveF32 = floatTargets() && f32.make(physW, physH, bgfx::TextureFormat::RGBA32F);
        if (!P.eq(key + ".targets", bgra.make(physW, physH, bgfx::TextureFormat::BGRA8), 1))
            return;
        const Pass ref = run(sink, list, bgra, haveF32 ? &f32 : nullptr, false);
        const Pass inst = run(sink, list, bgra, haveF32 ? &f32 : nullptr, true);
        const auto prims = static_cast<uint32_t>(list.prims.size());
        const auto quads = static_cast<uint32_t>(list.shaders.size() + list.images.size());
        P.eq(key + ".read", ref.read && inst.read, 1);
        P.eq(key + ".geometry", ref.geometry == BgfxSink::Geometry::vertices
                                    && inst.geometry == BgfxSink::Geometry::instances, 1);
        if (!P.eq(key + ".draws", ref.draws == inst.draws && ref.bgfxDraws == inst.bgfxDraws && inst.draws > 0, 1))
            std::printf("INFO     %s: draw calls %d / %d, bgfx %d / %d (vertices / instances)\n", key.c_str(), ref.draws,
                        inst.draws, ref.bgfxDraws, inst.bgfxDraws);
        P.eq(key + ".vertices", ref.vertices == inst.vertices && inst.vertices == 6u * (prims + quads), 1);
        P.eq(key + ".bytes", ref.bytes == 384u * (prims + quads) && inst.bytes == 80u * prims + 384u * quads, 1);
        bgra.pixels = inst.bgra;
        P.eq(key + ".drawn", drawnPixels(bgra, list.info.clear) > (dump ? 0 : static_cast<int64_t>(physW) * physH / 20),
             1);
        P.eq(key + ".bgra8_identical", differing(bgra, ref.bgra, key + ".bgra8"), 0);
        if (haveF32)
        {
            f32.pixels = inst.rgba32f;
            P.eq(key + ".float_identical", differing(f32, ref.rgba32f, key + ".float"), 0);
            // The float frame is a picture, not the read-back's fill or nothing: most pixels hold a colour in 0..1.
            int64_t painted = 0;
            for (size_t i = 0; i + 15 < f32.pixels.size(); i += 16)
            {
                float rgba[4];
                std::memcpy(rgba, &f32.pixels[i], sizeof rgba);
                painted += (rgba[0] >= 0.0f && rgba[0] <= 1.0f && rgba[3] > 0.0f && rgba[0] + rgba[1] + rgba[2] > 0.0f)
                               ? 1 : 0;
            }
            P.eq(key + ".float_drawn", painted > (dump ? 0 : static_cast<int64_t>(physW) * physH / 2), 1);
        }
        std::printf("INFO     %s: %u primitives, %d draw calls, %u bytes as instances, %u as vertices\n", key.c_str(),
                    prims, inst.draws, inst.bytes, ref.bytes);
        if (pngName != nullptr && funkgui::writePng(bgra.image(), pngName))   // in the sandbox (the working directory)
            std::printf("INFO     wrote %s\n", pngName);
        bgra.drop();
        f32.drop();
    }

    double median(std::vector<double> v)
    {
        if (v.empty())
            return 0.0;
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    }

    // This thread's CPU time in microseconds: what the message thread spends, without the time it waits.
    double threadCpuUs()
    {
        timespec ts{};
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
        return static_cast<double>(ts.tv_sec) * 1e6 + static_cast<double>(ts.tv_nsec) * 1e-3;
    }

    // The measurement (the file comment). Each frame: submit() and the bgfx::frame() that renders it, as this thread's
    // CPU time and as wall time, and the GPU time bgfx reports for it. The two paths alternate in blocks of 20 frames
    // after a warm-up, so a change in the machine's load falls on both; medians in microseconds.
    void measure(BgfxSink& sink, const PrimList& list, int physW, int physH, const std::string& what, int frames)
    {
        Target t;
        if (!t.make(physW, physH, bgfx::TextureFormat::BGRA8))
            return;
        using Clock = std::chrono::steady_clock;
        const auto us = [](Clock::duration d) { return std::chrono::duration<double, std::micro>(d).count(); };
        struct Series
        {
            std::vector<double> submitCpu, frameCpu, wall, gpu;
            uint32_t bytes = 0;
        };
        Series series[2];
        for (const bool instancing : { false, true })
        {
            setInstancing(instancing);
            for (int i = 0; i < 20; ++i)
            {
                sink.submit(list, kDrawView, t.fb, physW, physH);
                bgfx::frame();
            }
        }
        for (int done = 0; done < frames;)
        {
            const int block = std::min(20, frames - done);
            for (const bool instancing : { false, true })
            {
                Series& s = series[instancing ? 1 : 0];
                setInstancing(instancing);
                for (int i = 0; i < block; ++i)
                {
                    const auto w0 = Clock::now();
                    const double c0 = threadCpuUs();
                    sink.submit(list, kDrawView, t.fb, physW, physH);
                    const double c1 = threadCpuUs();
                    bgfx::frame();
                    const double c2 = threadCpuUs();
                    const auto w1 = Clock::now();
                    s.submitCpu.push_back(c1 - c0);
                    s.frameCpu.push_back(c2 - c1);
                    s.wall.push_back(us(w1 - w0));
                    s.bytes = sink.lastUploadBytes();
                    const bgfx::Stats* st = bgfx::getStats();
                    // bgfx reports a frame some frames after it ran: the first frames of a block are the other path's.
                    if (i >= 4 && st != nullptr && st->gpuTimerFreq > 0 && st->gpuTimeEnd > st->gpuTimeBegin)
                        s.gpu.push_back(static_cast<double>(st->gpuTimeEnd - st->gpuTimeBegin) * 1e6
                                        / static_cast<double>(st->gpuTimerFreq));
                }
            }
            done += block;
        }
        for (int k = 0; k < 2; ++k)
        {
            const Series& s = series[k];
            std::printf("INFO     measure %s %s: %zu primitives, %u bytes uploaded a frame; CPU submit %.1f us + "
                        "frame %.1f us = %.1f us; wall %.1f us; GPU %.1f us (medians of %d)\n",
                        what.c_str(), k == 1 ? "instances" : "vertices ", list.prims.size(), s.bytes,
                        median(s.submitCpu), median(s.frameCpu), median(s.submitCpu) + median(s.frameCpu),
                        median(s.wall), median(s.gpu), frames);
        }
        t.drop();
    }

    bool readFile(const std::filesystem::path& p, std::string& out)
    {
        std::ifstream f(p, std::ios::binary);
        if (!f)
            return false;
        std::ostringstream s;
        s << f.rdbuf();
        out = s.str();
        return true;
    }

    std::string keyOf(std::string name)
    {
        for (char& ch : name)
        {
            if (ch >= 'A' && ch <= 'Z')
                ch = static_cast<char>(ch - 'A' + 'a');
            if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_'))
                ch = '_';
        }
        return name;
    }

    void registerPictures()
    {
        std::vector<uint8_t> a(4u * 4u * 4u), b(2u * 2u * 4u);
        for (size_t i = 0; i < a.size(); i += 4)
        {
            a[i] = static_cast<uint8_t>(i * 13);
            a[i + 1] = static_cast<uint8_t>(255 - i * 7);
            a[i + 2] = 90;
            a[i + 3] = 255;
        }
        for (size_t i = 0; i < b.size(); i += 4)
        {
            b[i] = 250;
            b[i + 1] = static_cast<uint8_t>(i * 40);
            b[i + 2] = 30;
            b[i + 3] = 255;
        }
        images::set(S::kSplitImageA, 4, 4, a.data());
        images::set(S::kSplitImageB, 2, 2, b.data());
    }
}

int main(int argc, char** argv)
{
    // The hand-run flags (the file comment), taken out before the harness reads argv.
    std::string dumpDir;
    int frames = 200;
    std::vector<char*> args;
    for (int i = 0; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--dumps" && i + 1 < argc)
            dumpDir = argv[++i];
        else if (a == "--frames" && i + 1 < argc)
            frames = std::clamp(std::atoi(argv[++i]), 1, 100000);
        else
            args.push_back(argv[i]);
    }

    const juce::ScopedJuceInitialiser_GUI juceInit;      // the font atlas bakes through JUCE
    T::Probe P("fg.gpu.instancing", "", static_cast<int>(args.size()), args.data());

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
    P.eq("context.instancing", ctx.instancing(), 1);
    P.eq("context.instancing_by_default", funkgui::BgfxContext::Config{}.instancing, 1);
    P.note("float_targets", floatTargets() ? "\"rgba32f\"" : "\"none: the float rows are skipped\"");
    registerPictures();

    auto& fonts = funkgui::FontService::get();
    const funkgui::FontAtlasSdf& atlas = fonts.atlas();  // baked on first use (the context's font texture has it)
    if (!P.eq("font.ok", fonts.ok(), 1))
    {
        ctx.release(nullptr);
        return P.finish();
    }
    funkgui::Canvas canvas(atlas);
    BgfxSink sink;

    struct Scene { const char* name; void (*record)(funkgui::Canvas&); };
    const Scene scenes[] = { { "kinds", S::kinds }, { "colours", S::colours }, { "stress", S::stress },
                             { "split", S::split } };
    struct Scale { float dpi; const char* name; };
    for (const Scale sc : { Scale{ 1.0f, "dpi1" }, Scale{ 2.0f, "dpi2" } })
        for (const Scene& scene : scenes)
        {
            canvas.begin(S::frame(sc.dpi));
            scene.record(canvas);
            const PrimList& list = canvas.end();
            const int pw = static_cast<int>(std::lround(S::kW * sc.dpi));
            const int ph = static_cast<int>(std::lround(S::kH * sc.dpi));
            const std::string name = scene.name;
            const std::string key = name + "." + sc.name;
            const std::string png = "gpu-instancing-" + name + ".png";
            const bool keep = sc.dpi == 2.0f && (name == "kinds" || name == "stress");
            compare(P, sink, list, pw, ph, key, keep ? png.c_str() : nullptr);
        }

    // A frame with one primitive that is not instanceable draws as vertices, with the same pixels.
    {
        canvas.begin(S::frame(2.0f));
        S::kinds(canvas);
        PrimList odd = canvas.end();
        odd.prims[3].d2[2] = -0.0f;                      // an rrect whose kind is -0: the shader reads it as 0
        Target bgra;
        if (P.eq("fallback.target", bgra.make(S::kW * 2, S::kH * 2, bgfx::TextureFormat::BGRA8), 1))
        {
            const Pass ref = run(sink, odd, bgra, nullptr, false);
            const Pass inst = run(sink, odd, bgra, nullptr, true);
            P.eq("fallback.geometry", inst.geometry == BgfxSink::Geometry::vertices, 1);
            P.eq("fallback.bytes", inst.bytes == 384u * static_cast<uint32_t>(odd.prims.size()), 1);
            bgra.pixels = inst.bgra;
            P.eq("fallback.bgra8_identical", differing(bgra, ref.bgra, "fallback"), 0);
            bgra.drop();
        }
    }

    // The float comparison sees what an 8-bit frame rounds away: one rrect's local x moved by one unit in the last
    // place changes the float frame (and is noted for the 8-bit one).
    if (floatTargets())
    {
        canvas.begin(S::frame(2.0f));
        S::kinds(canvas);
        PrimList moved = canvas.end();
        moved.prims[1].d0[0] = std::nextafter(moved.prims[1].d0[0], 0.0f);
        canvas.begin(S::frame(2.0f));
        S::kinds(canvas);
        const PrimList& base = canvas.end();
        Target f32, bgra;
        if (P.eq("sensitivity.targets", f32.make(S::kW * 2, S::kH * 2, bgfx::TextureFormat::RGBA32F)
                                            && bgra.make(S::kW * 2, S::kH * 2, bgfx::TextureFormat::BGRA8), 1))
        {
            const Pass a = run(sink, base, bgra, &f32, true);
            const Pass b = run(sink, moved, bgra, &f32, true);
            f32.pixels = b.rgba32f;
            bgra.pixels = b.bgra;
            const int64_t floats = differing(f32, a.rgba32f, "sensitivity.float");
            const int64_t bytes = differing(bgra, a.bgra, "sensitivity.bgra8");
            std::printf("INFO     one ulp of local x: %lld float pixels differ, %lld 8-bit pixels\n",
                        static_cast<long long>(floats), static_cast<long long>(bytes));
            P.eq("sensitivity.float_sees_one_ulp", floats > 0, 1);
        }
        f32.drop();
        bgra.drop();
    }

    // The measurement of the stress frame, and of 5,000 primitives a device px or two across, whose pixels cost
    // next to nothing, so that the time per primitive shows (INFO lines only).
    {
        canvas.begin(S::frame(2.0f));
        S::stress(canvas);
        measure(sink, canvas.end(), S::kW * 2, S::kH * 2, "stress.dpi2", frames);
        canvas.begin(S::frame(2.0f));
        S::Lcg g;
        for (int i = 0; i < S::kStressPrims; ++i)
            canvas.rrect(g.in(0.0f, S::kW), g.in(0.0f, S::kH), 0.5f, 0.5f, 0.0f, funkgui::Col{ g.byte(), 90, 200, 255 });
        measure(sink, canvas.end(), S::kW * 2, S::kH * 2, "specks.dpi2", frames);
    }

    // Hand runs: every dump of a directory, compared and measured.
    if (!dumpDir.empty())
    {
        std::vector<std::filesystem::path> dumps;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(dumpDir, ec))
            if (e.path().extension() == ".dump")
                dumps.push_back(e.path());
        std::sort(dumps.begin(), dumps.end());
        P.eq("dump.found", !dumps.empty(), 1);
        for (const auto& path : dumps)
        {
            std::string text;
            PrimList l;
            const std::string key = "dump." + keyOf(path.stem().string());
            if (!P.eq(key + ".parsed", readFile(path, text) && PrimList::parseText(text, l), 1))
                continue;
            const int pw = static_cast<int>(std::lround(static_cast<float>(l.info.logicalW) * l.info.dpi));
            const int ph = static_cast<int>(std::lround(static_cast<float>(l.info.logicalH) * l.info.dpi));
            compare(P, sink, l, pw, ph, key, nullptr, true);
            measure(sink, l, pw, ph, key, frames);
        }
    }

    // bgfx down and up again: the instanced program, its quad and table are made again and draw the same pixels.
    setInstancing(true);
    ctx.release(nullptr);
    P.eq("restart.released", !ctx.valid(), 1);
    if (P.eq("restart.acquired", ctx.acquire(nullptr, 0, 0), 1))
    {
        P.eq("restart.instancing", ctx.instancing(), 1);
        canvas.begin(S::frame(2.0f));
        S::kinds(canvas);
        compare(P, sink, canvas.end(), S::kW * 2, S::kH * 2, "restart.kinds");
        ctx.release(nullptr);
    }

    // A small transient buffer (bgfx started again with it): the overflow paths, frames submitted with no bgfx::frame()
    // between them, as editors share one. 4,224 bytes are 11 primitives as vertices exactly (11 x 384), a multiple of
    // 64 and not of 80.
    {
        constexpr uint32_t kSmall = 11u * 384u;
        funkgui::BgfxContext::Config c = ctx.config();
        const uint32_t defaultBytes = c.transientVbBytes;
        c.transientVbBytes = kSmall;
        static_cast<void>(ctx.configure(c));
        Target t;
        const bool acquired = ctx.acquire(nullptr, 0, 0);
        if (P.eq("overflow.acquired", acquired && ctx.transientVbBytes() == kSmall, 1)
            && P.eq("overflow.target", t.make(S::kW, S::kH, bgfx::TextureFormat::BGRA8), 1))
        {
            const auto record = [&canvas](int prims, int imageDraws) {
                canvas.begin(S::frame(1.0f));
                for (int i = 0; i < prims; ++i)
                    canvas.rrect(4.0f + 8.0f * static_cast<float>(i % 45), 4.0f + 8.0f * static_cast<float>(i / 45),
                                 6.0f, 6.0f, 1.0f, funkgui::Col{ 200, 120, 40, 255 });
                for (int i = 0; i < imageDraws; ++i)
                    canvas.image(S::kSplitImageA, { 20.0f + 40.0f * static_cast<float>(i), 100.0f, 30.0f, 30.0f },
                                 0.0f);
                return PrimList(canvas.end());
            };
            const PrimList one = record(1, 0), eleven = record(11, 0), twelve = record(12, 0), forty = record(40, 0);
            const PrimList quads = record(45, 2);
            P.eq("overflow.lists", one.prims.size() == 1 && eleven.prims.size() == 11 && twelve.prims.size() == 12
                                       && forty.prims.size() == 40 && quads.prims.size() == 45
                                       && quads.images.size() == 2, 1);
            using R = BgfxSink::Result;
            const auto submit = [&](const PrimList& l) { return sink.submit(l, kDrawView, t.fb, S::kW, S::kH); };
            const uint32_t before = sink.overflowCount();

            // As vertices (Config::instancing off, the path before v0.26.0): 12 primitives (4,608 bytes) drop; 11 then
            // fill the buffer to its last byte, so the dropped frame took nothing.
            setInstancing(false);
            P.eq("overflow.vertices_dropped",
                 submit(twelve) == R::droppedOverflow && sink.overflowCount() == before + 1, 1);
            P.eq("overflow.vertices_fill_after_drop",
                 submit(eleven) == R::submitted && sink.lastGeometry() == BgfxSink::Geometry::vertices
                     && sink.lastUploadBytes() == kSmall, 1);
            // Records into that full buffer: none fits (bgfx's own count of 80-byte records wraps there).
            setInstancing(true);
            P.eq("overflow.full_buffer_records_dropped",
                 submit(one) == R::droppedOverflow && sink.overflowCount() == before + 2, 1);
            bgfx::frame();

            // As instances: 45 records (3,600 bytes) would fit, but their two image draws' 768 bytes after them would
            // not: dropped, taking nothing, so 40 records (3,200 bytes) fit after it.
            P.eq("overflow.quads_dropped",
                 submit(quads) == R::droppedOverflow && sink.overflowCount() == before + 3, 1);
            P.eq("overflow.records_fit_after_drop",
                 submit(forty) == R::submitted && sink.lastGeometry() == BgfxSink::Geometry::instances
                     && sink.lastUploadBytes() == 40u * 80u, 1);
            bgfx::frame();
            P.eq("overflow.instances_fit",
                 submit(twelve) == R::submitted && sink.lastUploadBytes() == 12u * 80u
                     && sink.overflowCount() == before + 3, 1);
            bgfx::frame();
        }
        t.drop();
        if (acquired)
            ctx.release(nullptr);
        c.transientVbBytes = defaultBytes;
        static_cast<void>(ctx.configure(c));
        P.eq("overflow.restored", ctx.config().transientVbBytes == defaultBytes && ctx.config().instancing, 1);
    }
    images::remove(S::kSplitImageA);
    images::remove(S::kSplitImageB);
    return P.finish();
}

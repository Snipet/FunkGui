// FUNKGUI_TEST name=fg.gpu.flip timeout=120 gpu=1
//
// fg.gpu.flip (v0.25.1; src/gpu/BgfxReset.h): a frame is presented by the bgfx::frame() that renders it, on Metal.
// Since 0.25.0 the FramePump runs no bgfx::frame() while no editor draws, and bgfx presented each frame only at the
// start of the next one, so the last frame drawn stayed rendered but unpresented until something drew again (a hover a
// step behind the pointer, a view switch shown at the next pointer move). The windows are CAMetalLayers of the test's,
// as in fg.gpu.picture (bgfx takes a layer as the window itself), drawn into as an editor draws (BgfxSink::submit on
// the window's view and framebuffer, then bgfx::frame()). Each layer counts the drawables bgfx takes from it and those
// Metal reports presented: a presented handler on each drawable, which Metal calls once the drawable is presented (on
// a Metal thread, with or without a window server) and never for one it was not asked to present. After a frame the
// rows wait, up to two seconds and running no other frame, until every drawable taken from the window is presented:
// before 0.25.1 the last one never was, so each row below that follows a frame fails there (the counts are one short,
// the previous frame's present arriving with the next frame).
// Needs a Metal device, not a window server; without one (a VM) every row is skipped and noted.
//   flags.*      BgfxContext's reset flags, given at init and at every reset: flip after render, no vsync
//   init.*       bgfx started on a window (its primary): nothing presented before a frame; its first frame is
//                presented with no frame after it, and so is the next, one present each
//   reset.*      the primary resized (bgfx::reset): a frame at the new size is presented with no frame after it
//   secondary.*  a second window (a framebuffer of its own): a frame drawn into it alone is presented, and nothing is
//                presented to the primary, which drew nothing
//   pump.*       through the FramePump, ticked by hand: a client draws one frame and then nothing, as an editor leaves
//                its picture up; the pump runs one bgfx::frame() and none after it, and that frame is presented
//   rebuilt.*    the primary closed: bgfx starts again on the second window (bgfx::init again), whose next frame is
//                presented with no frame after it
//   teardown.*   every window closed: bgfx down
// Spec rows only.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "../../src/gpu/BgfxReset.h"

#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/core/Col.h>
#include <funkgui/gpu/BgfxContext.h>
#include <funkgui/gpu/BgfxSink.h>
#include <funkgui/gpu/FramePump.h>
#include <funkgui/test/Harness.h>
#include <funkgui/text/FontService.h>

#include <bgfx/bgfx.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

// The drawables bgfx took from the layer in slot i, and those Metal presented. Presents are counted on a Metal thread.
static constexpr int kSlots = 2;
static std::atomic<int> gTaken[kSlots];
static std::atomic<int> gPresented[kSlots];

@interface FgFlipLayer : CAMetalLayer
@property (nonatomic) int slot;
@end

@implementation FgFlipLayer
- (id<CAMetalDrawable>)nextDrawable
{
    id<CAMetalDrawable> drawable = [super nextDrawable];
    const int slot = self.slot;
    if (drawable != nil && slot >= 0 && slot < kSlots)
    {
        gTaken[slot].fetch_add(1);
        [drawable addPresentedHandler:^(id<MTLDrawable>) { gPresented[slot].fetch_add(1); }];
    }
    return drawable;
}
@end

namespace T = funkgui::test;
using funkgui::BgfxContext;
using funkgui::PrimList;

namespace
{
    constexpr int kPw = 64, kPh = 48;                // a window's drawable in device px (32 x 24 at dpi 2)
    constexpr int kResizedPw = 80, kResizedPh = 60;  // the primary's after reset.*

    PrimList frameAt(int pw, int ph)
    {
        funkgui::FrameInfo f;
        f.logicalW = pw / 2;
        f.logicalH = ph / 2;
        f.dpi = 2.0f;
        f.clear = funkgui::Col{ 18, 20, 24, 255 };
        f.textGamma = 0.714285731f;
        f.fixedClock = true;
        funkgui::Canvas canvas(funkgui::FontService::get().atlas());
        canvas.begin(f);
        canvas.rrect(2.0f, 2.0f, static_cast<float>(f.logicalW) - 4.0f, static_cast<float>(f.logicalH) - 4.0f, 3.0f,
                     funkgui::Col{ 40, 60, 80, 255 }, 1.0f, funkgui::Col{ 120, 130, 140, 255 });
        return canvas.end();
    }

    void submit(BgfxContext& ctx, const PrimList& f, void* window, int pw, int ph)
    {
        funkgui::BgfxSink sink;
        static_cast<void>(sink.submit(f, ctx.viewIdFor(window), ctx.framebufferFor(window), pw, ph));
    }

    // One frame drawn into `window` alone, as the pump runs it: the editor's submit, then bgfx's frame.
    void drawFrame(BgfxContext& ctx, const PrimList& f, void* window, int pw, int ph)
    {
        submit(ctx, f, window, pw, ph);
        bgfx::frame();
        ctx.afterFrame();
    }

    // Waits, running no frame, until every drawable bgfx took from the layer in `slot` is presented, or two seconds
    // pass. Whether they all were.
    bool allPresented(int slot)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (gPresented[slot].load() < gTaken[slot].load() && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return gPresented[slot].load() == gTaken[slot].load();
    }

    // Registered and backing bgfx's default swapchain: no framebuffer of its own, view 0.
    bool isPrimary(BgfxContext& ctx, void* w)
    {
        return ctx.owns(w) && !bgfx::isValid(ctx.framebufferFor(w)) && ctx.viewIdFor(w) == 0;
    }

    // An editor as the pump sees it: one frame drawn when asked, nothing at every other tick (its last picture left
    // up). No view of its own for a display link: the pump is ticked by hand.
    class OneFrameClient final : public funkgui::FrameClient
    {
    public:
        void* window = nullptr;
        const PrimList* list = nullptr;
        bool drawNext = false;
        int ran = 0, drew = 0;

        Result submitFrame(float) override
        {
            ++ran;
            Result r;
            if (drawNext && window != nullptr && list != nullptr)
            {
                submit(BgfxContext::get(), *list, window, kPw, kPh);
                drawNext = false;
                ++drew;
                r.submitted = true;
            }
            return r;
        }
        void* frameClockView() const override { return nullptr; }
    };
}

int main(int argc, char** argv)
{
    T::Probe P("fg.gpu.flip", "", argc, argv);
    auto& ctx = BgfxContext::get();

    // ---- flags --------------------------------------------------------------------------------------------------------
    P.eq("flags.flip_after_render", (funkgui::detail::kBgfxResetFlags & BGFX_RESET_FLIP_AFTER_RENDER) != 0, 1);
    P.eq("flags.no_vsync", (funkgui::detail::kBgfxResetFlags & BGFX_RESET_VSYNC) == 0, 1);
    {
        const juce::ScopedJuceInitialiser_GUI juceInit;      // the message thread (this one) and the font's bake

        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        const bool metal = device != nil;
        [device release];
        P.note("metal", metal ? "\"present\"" : "\"absent\"");
        if (!metal)
        {
            std::printf("INFO     no Metal device: nothing to present on, every other row skipped\n");
            return P.finish();
        }
        P.eq("init.bgfx_down_before", ctx.valid() ? 1 : 0, 0);

        FgFlipLayer* a = [[FgFlipLayer alloc] init];
        FgFlipLayer* b = [[FgFlipLayer alloc] init];
        a.slot = 0;
        b.slot = 1;
        void* wa = (void*) a;
        void* wb = (void*) b;
        const PrimList f = frameAt(kPw, kPh);

        // ---- init: bgfx started on a window ---------------------------------------------------------------------------
        if (P.eq("init.acquired", ctx.acquire(wa, kPw, kPh), 1))
        {
            P.eq("init.primary", isPrimary(ctx, wa), 1);
            P.eq("init.metal", bgfx::getRendererType() == bgfx::RendererType::Metal, 1);
            P.eq("init.nothing_presented_before_a_frame", gPresented[0].load(), 0);
            drawFrame(ctx, f, wa, kPw, kPh);
            P.eq("init.first_frame_took_a_drawable", gTaken[0].load(), 1);
            const bool first = allPresented(0);
            std::printf("INFO     init: %d of %d drawables presented after the first frame, with no frame after it\n",
                        gPresented[0].load(), gTaken[0].load());
            P.eq("init.first_frame_presented", first, 1);            // 0 of 1 before 0.25.1, until the next frame
            drawFrame(ctx, f, wa, kPw, kPh);
            P.eq("init.next_frame_presented", allPresented(0), 1);
            P.eq("init.one_drawable_a_frame", gTaken[0].load(), 2);

            // ---- reset: the primary resized -----------------------------------------------------------------------------
            ctx.resizeWindow(wa, kResizedPw, kResizedPh);           // bgfx::reset
            const PrimList resized = frameAt(kResizedPw, kResizedPh);
            const int t0 = gTaken[0].load();
            drawFrame(ctx, resized, wa, kResizedPw, kResizedPh);
            const bool afterReset = allPresented(0);
            std::printf("INFO     reset: %d of %d presented after a frame at %d x %d, with no frame after it\n",
                        gPresented[0].load(), gTaken[0].load(), kResizedPw, kResizedPh);
            P.eq("reset.took_a_drawable", gTaken[0].load() - t0, 1);
            P.eq("reset.frame_presented", afterReset, 1);
            P.eq("reset.drawable_size", static_cast<int64_t>(a.drawableSize.width) == kResizedPw
                                            && static_cast<int64_t>(a.drawableSize.height) == kResizedPh, 1);

            // ---- secondary: a window with its own framebuffer ---------------------------------------------------------
            if (P.eq("secondary.acquired", ctx.acquire(wb, kPw, kPh), 1))
            {
                P.eq("secondary.own_framebuffer", bgfx::isValid(ctx.framebufferFor(wb)) && ctx.viewIdFor(wb) != 0, 1);
                const int pa = gPresented[0].load(), ta = gTaken[0].load();
                drawFrame(ctx, f, wb, kPw, kPh);
                P.eq("secondary.took_a_drawable", gTaken[1].load(), 1);
                P.eq("secondary.frame_presented", allPresented(1), 1);
                P.eq("secondary.primary_untouched", gPresented[0].load() - pa + gTaken[0].load() - ta, 0);

                // ---- pump: one frame drawn, then nothing --------------------------------------------------------------
                {
                    auto& pump = funkgui::FramePump::get();
                    OneFrameClient client;
                    client.window = wb;
                    client.list = &f;
                    pump.add(&client);
                    double stamp = 1000.0;
                    const uint64_t frames0 = pump.framesRun();
                    const int tb = gTaken[1].load();
                    client.drawNext = true;
                    for (int i = 0; i < 6; ++i)
                        pump.tick(stamp += 1.0 / 60.0);
                    const bool presented = allPresented(1);
                    std::printf("INFO     pump: %d ticks, %d drawn, %llu bgfx frames, %d of %d drawables presented\n",
                                client.ran, client.drew, static_cast<unsigned long long>(pump.framesRun() - frames0),
                                gPresented[1].load(), gTaken[1].load());
                    P.eq("pump.client_ran_every_tick", client.ran, 6);
                    P.eq("pump.drew_once", client.drew, 1);
                    P.eq("pump.one_bgfx_frame", static_cast<int64_t>(pump.framesRun() - frames0), 1);
                    P.eq("pump.took_a_drawable", gTaken[1].load() - tb, 1);
                    P.eq("pump.frame_presented", presented, 1);      // not before 0.25.1, until something drew again
                    pump.remove(&client);
                }

                // ---- rebuilt: the primary closed, bgfx again on the second window -------------------------------------
                ctx.release(wa);
                P.eq("rebuilt.second_promoted", isPrimary(ctx, wb) && !ctx.owns(wa), 1);
                const int tr = gTaken[1].load();
                drawFrame(ctx, f, wb, kPw, kPh);
                P.eq("rebuilt.took_a_drawable", gTaken[1].load() - tr, 1);
                P.eq("rebuilt.frame_presented", allPresented(1), 1);
                ctx.release(wb);
            }
            else
            {
                ctx.release(wa);
            }
        }
        [a release];
        [b release];
    }
    // ---- teardown -------------------------------------------------------------------------------------------------------
    P.eq("teardown.bgfx_down", ctx.valid() ? 0 : 1, 1);
    return P.finish();
}

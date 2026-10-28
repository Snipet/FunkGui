// FUNKGUI_TEST name=fg.gpu.picture.live timeout=300 gpu=1 labels=live
//
// fg.gpu.picture.live: GPU pictures while an editor is open (v0.23.0; gpu/GpuPicture.h). Labels fg;gpu;live: it opens a
// borderless window on the desktop and initialises bgfx on Metal, so it needs a window server and is never in
// `verify`. Run it serialised:
//   lockf -k -t 900 /tmp/fcmp-gui.lock ctest --test-dir <build-agent-gui> -R '^fg\.gpu\.picture\.live$'
//
// The editor's Panel draws the scene of fg.gpu.picture (a custom shader and a picture among the primitives), so the
// window runs both programs too. Frames come from the pump's own clock; the run loop turns for real time between rows.
//   reference.*  with no window: the picture (bgfx started headless and held), the bytes every row below must match
//   join.*       an editor opened while the picture holds bgfx headless: bgfx starts again on its window (it backs
//                the default swapchain), the hold stays registered; it attaches, draws and presents; a picture between
//                its frames draws on the same bgfx, starting nothing and taking none of the window's drawables
//   reopen.*     the editor closed while the hold lasts: bgfx headless again, held; a new editor takes it over as
//                above and presents
//   handover.*   the hold released while the editor is open: nothing restarts, the editor goes on drawing and
//                presenting; the picture holds nothing from here on
// Presenting: the editor's CAMetalLayer (bgfx's, on the render view) counts the drawables bgfx takes from it, its class
// swapped for a counting subclass with no state of its own (as KVO swaps classes). bgfx takes a new drawable only after
// presenting the last, so a window drawn but never presented takes one and no more. A window whose occlusion state is
// not visible takes none either way (bgfx draws it into a texture of its own): the presenting rows are then skipped
// with a note. macOS 27 reports this process's shown windows so (fg.editorhost.multi's note; state 8192 on the M5,
// even for a window kept on top), so there they always skip; fg.gpu.picture's window.* rows measure presenting with a
// layer of their own as the window, which no occlusion state hides.
//   between.*    a picture on the message thread between the editor's frames: the editor's bgfx, the same bytes
//   indraw.*     a picture asked for inside the Panel's draw (the pump's frame, on the thread that drives bgfx): drawn
//                inline, the same bytes; the editor goes on drawing, with no overflow
//   worker.*     a picture from another thread while the run loop turns: posted, the same bytes
//   teardown.*   the editor closed: bgfx down (the picture held nothing)
// Spec rows only.

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#include <objc/runtime.h>

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/Images.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/core/Col.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/core/Theme.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/gpu/BgfxContext.h>
#include <funkgui/gpu/EditorHost.h>
#include <funkgui/gpu/GpuPicture.h>
#include <funkgui/gpu/NativeSurface.h>
#include <funkgui/gpu/ShaderLibrary.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Panel.h>
#include <funkgui/shaders/fs_custom_test.mtl.h>
#include <funkgui/test/Harness.h>
#include <funkgui/text/FontService.h>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// The drawables the counted editor's layer handed out (see the header): one layer is counted at a time.
static std::atomic<int> gDrawablesTaken{ 0 };

@interface FgPictureLiveCountingLayer : CAMetalLayer
@end

@implementation FgPictureLiveCountingLayer
- (id<CAMetalDrawable>)nextDrawable
{
    gDrawablesTaken.fetch_add(1);
    return [super nextDrawable];
}
@end

namespace T = funkgui::test;
using funkgui::Col;
using funkgui::GpuPicture;
using funkgui::PrimList;
using FRect = funkgui::Rect;                         // macOS has a ::Rect of its own (MacTypes.h)

namespace
{
    constexpr int kW = 160, kH = 100;
    constexpr Col kClear{ 18, 20, 24, 255 };
    constexpr Col kPanel{ 40, 60, 80, 255 };
    constexpr Col kBorder{ 120, 130, 140, 255 };
    constexpr Col kInk{ 230, 232, 236, 255 };
    constexpr Col kAccent{ 200, 120, 40, 255 };
    constexpr Col kOver{ 40, 200, 40, 255 };
    constexpr uint32_t kShaderId = 7, kChecker = 11;

    // fg.gpu.picture's scene.
    void scene(funkgui::Canvas& c)
    {
        c.rrect(4.0f, 4.0f, kW - 8.0f, kH - 8.0f, 6.0f, kPanel, 1.0f, kBorder);
        c.text("GPU PICTURE 0.23", 10.0f, 9.0f, funkgui::type::kLabel, kInk);
        c.segment(10.0f, 28.0f, 60.0f, 34.0f, 1.5f, kInk);
        c.area(66.0f, 120.0f, 30.0f, 22.0f, 36.0f, 36.0f, kAccent, 1.0f, kInk);
        c.disc(138.0f, 22.0f, 8.0f, kBorder, 1.5f, kInk);
        c.image(kChecker, FRect{ 10.0f, 44.0f, 24.0f, 24.0f }, 3.0f);
        const float params[4] = { 0.0f, 0.0f, 0.25f, 1.0f };
        c.shader(kShaderId, FRect{ 70.0f, 40.0f, 40.0f, 30.0f }, params, 4);
        c.rrect(100.0f, 60.0f, 20.0f, 16.0f, 0.0f, kOver);
        c.text("SHADER", 10.0f, 78.0f, funkgui::type::kCaption, kInk);
    }

    // The picture's frame: the scene at dpi 2, whatever the display.
    PrimList record()
    {
        funkgui::Canvas canvas(funkgui::FontService::get().atlas());
        funkgui::FrameInfo f;
        f.logicalW = kW;
        f.logicalH = kH;
        f.dpi = 2.0f;
        f.clear = kClear;
        f.textGamma = 0.714285731f;
        f.fixedClock = true;
        canvas.begin(f);
        scene(canvas);
        return canvas.end();
    }

    void registerChecker()
    {
        const uint8_t rgba[16] = { 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255 };
        funkgui::images::set(kChecker, 2, 2, rgba);
    }

    class NoOpProcessor final : public juce::AudioProcessor
    {
    public:
        NoOpProcessor()
            : juce::AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
        {
        }

        const juce::String getName() const override { return "fg.gpu.picture.live"; }
        void prepareToPlay(double, int) override {}
        void releaseResources() override {}
        void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override { buffer.clear(); }
        double getTailLengthSeconds() const override { return 0.0; }
        bool acceptsMidi() const override { return false; }
        bool producesMidi() const override { return false; }
        bool hasEditor() const override { return false; }
        juce::AudioProcessorEditor* createEditor() override { return nullptr; }
        int getNumPrograms() override { return 1; }
        int getCurrentProgram() override { return 0; }
        void setCurrentProgram(int) override {}
        const juce::String getProgramName(int) override { return {}; }
        void changeProgramName(int, const juce::String&) override {}
        void getStateInformation(juce::MemoryBlock&) override {}
        void setStateInformation(const void*, int) override {}
    };

    // Draws the scene and counts its draws; asked to, takes a picture from inside its draw.
    class ScenePanel final : public funkgui::Panel
    {
    public:
        int draws = 0;
        bool wantPicture = false;
        bool pictureTaken = false;
        GpuPicture inDraw;

        void attach(funkgui::HostServices&) override {}
        int  width() const override { return kW; }
        int  height() const override { return kH; }
        void tick(float) override {}
        void draw(funkgui::Canvas& c, const funkgui::Theme&) override
        {
            ++draws;
            if (wantPicture)
            {
                wantPicture = false;
                inDraw = funkgui::renderGpuPicture(record());   // inside the pump's frame, on bgfx's thread
                pictureTaken = true;
            }
            scene(c);
        }
        bool wantsFullRate() const override { return true; }
        void accessibility(std::vector<funkgui::A11yItem>&) const override {}
        uint32_t a11yRevision() const override { return 0; }
        void a11yAction(uint32_t, funkgui::A11yAction, double) override {}
        void closeGestures() override {}
    };

    struct Window
    {
        juce::Component top;
        std::unique_ptr<funkgui::EditorHost> editor;
        ScenePanel* panel = nullptr;

        Window(juce::AudioProcessor& p, int x, int y)
        {
            auto owned = std::make_unique<ScenePanel>();
            panel = owned.get();
            editor = std::make_unique<funkgui::EditorHost>(p, funkgui::EditorConfig{}, std::move(owned));
            top.setOpaque(true);
            top.setBounds(x, y, kW + 20, kH + 20);
            editor->setTopLeftPosition(10, 10);
            top.addAndMakeVisible(*editor);
            top.setVisible(true);
            top.addToDesktop(0);
        }

        void close()
        {
            if (editor != nullptr)
            {
                top.removeChildComponent(editor.get());
                editor.reset();
            }
            top.removeFromDesktop();
        }

        ~Window() { close(); }
    };

    void spin(double seconds)
    {
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
    }

    int drawsOver(Window& w, double seconds)
    {
        const int before = w.panel->draws;
        spin(seconds);
        return w.panel->draws - before;
    }

    // The render view: the peer NSView's subview of FunkGui's render-view class (as fg.editorhost.multi finds it).
    void* renderViewOf(funkgui::EditorHost& e)
    {
        auto* peer = e.getPeer();
        if (peer == nullptr)
            return nullptr;
        NSView* parent = (NSView*) peer->getNativeHandle();
        const char* cls = funkgui::renderViewClassName();
        for (NSView* v in [parent subviews])
            if (std::strcmp(class_getName([v class]), cls) == 0)
                return (void*) v;
        return nullptr;
    }

    // Registered and backing bgfx's default swapchain: no framebuffer of its own, view 0.
    bool isPrimary(funkgui::BgfxContext& ctx, void* view)
    {
        return view != nullptr && ctx.owns(view) && !bgfx::isValid(ctx.framebufferFor(view))
               && ctx.viewIdFor(view) == 0;
    }

    // From now on, counts the drawables bgfx takes from the view's layer (see the header). False when the view has no
    // CAMetalLayer of bgfx's to count.
    bool countDrawables(void* view)
    {
        CALayer* layer = view != nullptr ? [(NSView*) view layer] : nil;
        if (layer == nil)
            return false;
        if ([layer class] == [FgPictureLiveCountingLayer class])
            return true;
        if ([layer class] != [CAMetalLayer class])
            return false;
        object_setClass(layer, [FgPictureLiveCountingLayer class]);
        return true;
    }

    // Whether the view's window is visible on screen: bgfx takes no drawable for an occluded one.
    bool onScreen(void* view)
    {
        NSWindow* w = view != nullptr ? [(NSView*) view window] : nil;
        return w != nil && ([w occlusionState] & NSWindowOcclusionStateVisible) != 0;
    }

    int drawablesOver(double seconds)
    {
        const int before = gDrawablesTaken.load();
        spin(seconds);
        return gDrawablesTaken.load() - before;
    }

    // The presenting rows of an editor's window (see the header): `key`.presents (bgfx took a new drawable for most
    // frames, as it does only after presenting each), and a picture between its frames taking none of them.
    void presentingRows(T::Probe& P, const std::string& key, Window& w, const PrimList& list,
                        const GpuPicture& reference)
    {
        void* view = renderViewOf(*w.editor);
        if (!countDrawables(view) || !onScreen(view))
        {
            NSWindow* win = view != nullptr ? [(NSView*) view window] : nil;
            std::printf("INFO     %s: the window is %s (occlusion state %lu); its presenting rows are skipped\n",
                        key.c_str(), view == nullptr ? "without a render view" : onScreen(view) ? "not countable"
                                                                                               : "occluded",
                        win != nil ? static_cast<unsigned long>([win occlusionState]) : 0ul);
            P.note(key + "_presenting", "\"skipped\"");
            return;
        }
        const int draws0 = w.panel->draws;
        const int taken = drawablesOver(0.5);
        const int draws = w.panel->draws - draws0;
        std::printf("INFO     %s: %d frames drawn, %d drawables taken in 0.5 s\n", key.c_str(), draws, taken);
        P.ge(key + ".presents", taken, 5);
        P.ge(key + ".presents_most_frames", taken * 10, draws * 8);   // a frame's take can fall either side of a window
        const int before = gDrawablesTaken.load();
        const GpuPicture pic = funkgui::renderGpuPicture(list);
        P.eq(key + ".picture_same_bytes", pic.ok() && pic.image.rgba == reference.image.rgba, 1);
        P.eq(key + ".picture_takes_no_drawable", gDrawablesTaken.load() - before, 0);
    }
}

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    T::Probe P("fg.gpu.picture.live", "", argc, argv);
    auto& ctx = funkgui::BgfxContext::get();

    std::string why;
    P.eq("setup.shader_registered",
         funkgui::shaders::set(kShaderId, fs_custom_test_mtl, sizeof fs_custom_test_mtl, why), 1);
    registerChecker();
    const PrimList list = record();

    // ---- reference: no window ---------------------------------------------------------------------------------------
    const GpuPicture reference = funkgui::renderGpuPicture(list);
    if (!P.eq("reference.ok", reference.ok() && reference.startedContext && funkgui::gpuPictureContextHeld(), 1))
        return P.finish();

    NoOpProcessor processor;
    {
        // ---- join: an editor opened while the picture holds bgfx headless -------------------------------------------
        auto first = std::make_unique<Window>(processor, 160, 160);
        spin(0.5);
        P.eq("join.attached", first->editor->surfaceAttached() ? 1 : 0, 1);
        P.eq("join.primary", isPrimary(ctx, renderViewOf(*first->editor)), 1);
        P.eq("join.hold_kept", funkgui::gpuPictureContextHeld() && ctx.owns(nullptr), 1);
        P.ge("join.draws", drawsOver(*first, 0.5), 5);
        {
            const GpuPicture pic = funkgui::renderGpuPicture(list);
            P.eq("join.ok", pic.ok(), 1);
            P.eq("join.started_nothing", pic.startedContext ? 1 : 0, 0);
            P.eq("join.same_bytes", pic.image.rgba == reference.image.rgba, 1);
        }
        presentingRows(P, "join", *first, list, reference);

        // ---- reopen: the editor closed while the hold lasts, and another opened ------------------------------------
        first.reset();
        spin(0.2);
        P.eq("reopen.headless_again", ctx.valid() && ctx.owns(nullptr) && funkgui::gpuPictureContextHeld(), 1);
        auto second = std::make_unique<Window>(processor, 160, 160);
        Window& a = *second;
        spin(0.5);
        P.eq("reopen.attached", a.editor->surfaceAttached() ? 1 : 0, 1);
        P.eq("reopen.primary", isPrimary(ctx, renderViewOf(*a.editor)), 1);
        P.eq("reopen.hold_kept", funkgui::gpuPictureContextHeld() && ctx.owns(nullptr), 1);
        P.ge("reopen.draws", drawsOver(a, 0.5), 5);
        presentingRows(P, "reopen", a, list, reference);

        // ---- handover: the hold released while the editor is open ---------------------------------------------------
        const uint32_t failures = a.editor->diagnostics().attachFailures;
        P.eq("handover.released", funkgui::releaseGpuPictureContext(), 1);
        P.eq("handover.bgfx_up", ctx.valid() ? 1 : 0, 1);
        P.eq("handover.still_primary", isPrimary(ctx, renderViewOf(*a.editor)) && !ctx.owns(nullptr), 1);
        spin(0.3);
        P.eq("handover.attached", a.editor->surfaceAttached() ? 1 : 0, 1);
        P.eq("handover.no_reattach", static_cast<int64_t>(a.editor->diagnostics().attachFailures - failures), 0);
        presentingRows(P, "handover", a, list, reference);
        P.ge("handover.draws", drawsOver(a, 0.5), 5);

        // ---- between: on the message thread, between frames ---------------------------------------------------------
        {
            const GpuPicture pic = funkgui::renderGpuPicture(list);
            P.eq("between.ok", pic.ok(), 1);
            P.eq("between.started_nothing", !pic.startedContext && !funkgui::gpuPictureContextHeld(), 1);
            P.eq("between.same_bytes", pic.image.rgba == reference.image.rgba, 1);
        }

        // ---- indraw: inside the Panel's draw ------------------------------------------------------------------------
        a.panel->wantPicture = true;
        for (int i = 0; i < 100 && !a.panel->pictureTaken; ++i)
            spin(0.02);
        std::printf("INFO     in the draw: %s, %u draw calls\n", funkgui::toString(a.panel->inDraw.status),
                    a.panel->inDraw.drawCalls);
        P.eq("indraw.taken", a.panel->pictureTaken ? 1 : 0, 1);
        P.eq("indraw.ok", a.panel->inDraw.ok(), 1);
        P.eq("indraw.same_bytes", a.panel->inDraw.image.rgba == reference.image.rgba, 1);
        P.ge("indraw.editor_draws_on", drawsOver(a, 0.5), 5);
        P.eq("indraw.no_overflow", static_cast<int64_t>(a.editor->diagnostics().overflows), 0);
        P.eq("indraw.still_attached", a.editor->surfaceAttached() ? 1 : 0, 1);

        // ---- worker: from another thread while the run loop turns ---------------------------------------------------
        {
            GpuPicture pic;
            std::atomic<bool> done{ false };
            std::thread t([&]
            {
                funkgui::GpuPictureOptions o;
                o.timeoutMs = 10000;
                pic = funkgui::renderGpuPicture(list, o);
                done.store(true);
            });
            for (int i = 0; i < 1000 && !done.load(); ++i)
                spin(0.01);
            t.join();
            P.eq("worker.ok", pic.ok(), 1);
            P.eq("worker.same_bytes", pic.image.rgba == reference.image.rgba, 1);
            P.ge("worker.editor_draws_on", drawsOver(a, 0.3), 3);
        }

        a.close();
        spin(0.2);
    }
    P.eq("teardown.nothing_held", funkgui::gpuPictureContextHeld() ? 1 : 0, 0);
    P.eq("teardown.bgfx_down", ctx.valid() ? 1 : 0, 0);
    funkgui::shaders::remove(kShaderId);
    funkgui::images::remove(kChecker);
    return P.finish();
}

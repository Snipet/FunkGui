// FUNKGUI_TEST name=fg.editorhost.skip timeout=300 gpu=1 labels=live
//
// fg.editorhost.skip: unchanged frames on real windows (0.25.0; gpu/EditorHost.h "Unchanged frames", panel/Panel.h
// needsDraw(), canvas/FrameCompare.h). Labels fg;gpu;live: it opens borderless windows on the desktop and initialises
// bgfx on Metal, so it needs a window server and is never in `verify`. Run it serialised:
//   lockf -k -t 900 /tmp/fcmp-gui.lock ctest --test-dir <build-agent-gui> -R '^fg\.editorhost\.skip$'
//
// The Panel draws one rounded rectangle and answers needsDraw() from flags the rows set. Frames come from the pump's
// own clock while the run loop turns, except in mixed.*, where the rows call FramePump::tick by hand (the run loop does
// not turn, so no other frame runs) and read bgfx's statistics of each frame.
//
// Occlusion is steered: macOS 27 reports this process's shown windows occluded (fg.gpu.picture.live's note), and bgfx
// presents nothing to such a window. So the test swaps NSWindow's occlusionState for its own, which every window of the
// process then reports, EditorHost (NativeSurface.h renderViewOccluded) and bgfx's Metal renderer alike: visible,
// except where uncover.* reports occluded. What macOS itself reports is printed. Presenting is counted: each editor's
// CAMetalLayer (bgfx's, on the render view) counts the drawables bgfx takes from it, its class swapped for a counting
// subclass with no state of its own (fg.gpu.picture.live's way). bgfx takes a drawable from a window's layer only for a
// frame in which a view draws into that window, and presents every drawable it takes at that frame's end
// (renderer_mtl.cpp), so a window whose layer gave none had nothing presented.
//   open.*      the first frame draws; its layer counted, reported visible
//   idle.*      needsDraw() false: asked every frame, nothing recorded or submitted, and the pump runs no
//               bgfx::frame() at all: nothing is drawn, no drawable taken
//   change.*    needsDraw() true once with the same picture: recorded, found unchanged, not submitted; with a new
//               colour: drawn, submitted and presented, then idle again
//   safety.*    needsDraw() true every frame, the same drawing: recorded every frame, found unchanged, never submitted
//   theme.*     the theme preference changed (the Panel not dirty): drawn, then idle; and back
//   resize.*    the window resized (the Panel not dirty): drawn at the new size
//   shown.*     the window hidden, then shown: nothing while hidden. With only the pump's clock, nothing ticks the
//               editor while its window is off screen (the display link stops), and it is not drawn again when shown:
//               its CAMetalLayer kept the picture (printed, not a row). With a frame while hidden (by hand, as a timer
//               clock gives one): nothing drawn then; shown, it draws and presents again, the Panel idle
//   uncover.*   reported occluded: a change is drawn (offscreen, bgfx takes no drawable), then nothing while it stays
//               occluded; reported visible again, the picture is drawn and presented once more (the Panel idle), then
//               nothing
//   mixed.*     a second editor (its layer counted) that changes every frame beside an idle one: each frame holds the
//               busy editor's draw calls only (the idle editor's view gets no draw call or clear) and only the busy
//               window's layer gives up a drawable, one a frame, so nothing is presented to the idle window; one frame
//               of the idle editor (its change nudged, as input nudges it) adds its calls once and takes one drawable
//               from its layer; the same with the
//               editors' roles swapped (the busy one now the primary, bgfx's default swapchain); both idle, no
//               bgfx::frame()
//   closed.*    an editor closed while the others are idle: the pump still runs the frames that let its view go
//               (BgfxContext::framePending), and the idle editors draw nothing
//   rebuilt.*   the primary editor closed: bgfx starts again on the survivor's view, which draws although idle
//   teardown.*  every editor closed: bgfx down, nothing held
// Spec rows only.

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <objc/runtime.h>                             // class_getName, method_setImplementation, object_setClass

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Col.h>
#include <funkgui/core/Theme.h>
#include <funkgui/gpu/BgfxContext.h>
#include <funkgui/gpu/EditorHost.h>
#include <funkgui/gpu/FramePump.h>
#include <funkgui/gpu/NativeSurface.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Panel.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/test/Harness.h>

#include <bgfx/bgfx.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

// The drawables bgfx took from each counted layer (see the header): gTaken[i] for the layer gLayers[i]. Message thread.
static constexpr int kCounted = 2;
static void* gLayers[kCounted] = {};
static int   gTaken[kCounted] = {};

@interface FgSkipCountingLayer : CAMetalLayer
@end

@implementation FgSkipCountingLayer
- (id<CAMetalDrawable>)nextDrawable
{
    id<CAMetalDrawable> drawable = [super nextDrawable];
    for (int i = 0; i < kCounted; ++i)
        if (drawable != nil && gLayers[i] == (void*) self)
            ++gTaken[i];
    return drawable;
}
@end

// What every NSWindow reports as its occlusion state (see the header): 1 visible, 0 occluded, -1 macOS's own answer.
static int gSteer = 1;
static IMP gOcclusionState = nullptr;               // NSWindow's own

static NSWindowOcclusionState steeredOcclusionState(id self, SEL sel)
{
    if (gSteer == 1)
        return NSWindowOcclusionStateVisible;
    if (gSteer == 0)
        return static_cast<NSWindowOcclusionState>(0);
    return reinterpret_cast<NSWindowOcclusionState (*)(id, SEL)>(gOcclusionState)(self, sel);
}

namespace T = funkgui::test;

namespace
{
    class NoOpProcessor final : public juce::AudioProcessor
    {
    public:
        NoOpProcessor()
            : juce::AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
        {
        }

        const juce::String getName() const override { return "fg.editorhost.skip"; }
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

    // A Panel whose needsDraw() the rows steer: `dirty` once (cleared by draw()), `alwaysAsk` every frame with the same
    // drawing, `animate` every frame with a new shade; recolour() is a real change, asked for once. A resize changes
    // nothing it draws on its own account.
    class StillPanel final : public funkgui::Panel
    {
    public:
        int  draws = 0;
        mutable int asked = 0;
        bool dirty = true, alwaysAsk = false, animate = false;
        funkgui::HostServices* host = nullptr;

        void recolour()
        {
            shade_ = static_cast<uint8_t>(shade_ + 7);
            dirty = true;
        }

        void attach(funkgui::HostServices& h) override { host = &h; }
        int  width() const override { return w_; }
        int  height() const override { return h_; }
        bool resizable() const override { return true; }
        int  minWidth() const override { return 200; }
        int  minHeight() const override { return 120; }
        void resize(int w, int h) override
        {
            w_ = w;
            h_ = h;
        }
        void tick(float) override {}
        bool needsDraw() const override
        {
            ++asked;
            return dirty || alwaysAsk || animate;
        }
        void draw(funkgui::Canvas& c, const funkgui::Theme& th) override
        {
            ++draws;
            if (animate)
                ++shade_;
            c.rrect(12.0f, 12.0f, static_cast<float>(w_) - 24.0f, static_cast<float>(h_) - 24.0f, 6.0f,
                    funkgui::Col{ shade_, 60, 90, 255 }, 1.0f, th.ink52);
            dirty = false;
        }
        bool wantsFullRate() const override { return animate; }
        void accessibility(std::vector<funkgui::A11yItem>&) const override {}
        uint32_t a11yRevision() const override { return 0; }
        void a11yAction(uint32_t, funkgui::A11yAction, double) override {}
        void closeGestures() override {}

    private:
        int w_ = 260, h_ = 170;
        uint8_t shade_ = 40;
    };

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

    struct Window
    {
        juce::Component top;
        std::unique_ptr<funkgui::EditorHost> editor;
        StillPanel* panel = nullptr;
        int slot = -1;                               // gLayers' slot once counted

        Window(juce::AudioProcessor& p, int x, int y)
        {
            auto owned = std::make_unique<StillPanel>();
            panel = owned.get();
            editor = std::make_unique<funkgui::EditorHost>(p, funkgui::EditorConfig{}, std::move(owned));
            top.setOpaque(true);
            top.setBounds(x, y, 340, 240);
            editor->setTopLeftPosition(10, 10);
            top.addAndMakeVisible(*editor);
            top.setVisible(true);
            top.addToDesktop(0);
        }

        void close()
        {
            if (slot >= 0)
                gLayers[slot] = nullptr;
            slot = -1;
            if (editor != nullptr)
            {
                top.removeChildComponent(editor.get());
                editor.reset();
            }
            top.removeFromDesktop();
        }

        ~Window() { close(); }

        // From now on, counts the drawables bgfx takes from this window's layer in gLayers[`i`]. False when the render
        // view has no CAMetalLayer of bgfx's.
        bool countDrawables(int i)
        {
            void* view = editor != nullptr ? renderViewOf(*editor) : nullptr;
            CALayer* layer = view != nullptr ? [(NSView*) view layer] : nil;
            if (layer == nil || ([layer class] != [CAMetalLayer class] && [layer class] != [FgSkipCountingLayer class]))
                return false;
            object_setClass(layer, [FgSkipCountingLayer class]);
            gLayers[i] = (void*) layer;
            gTaken[i] = 0;
            slot = i;
            return true;
        }

        int taken() const { return slot >= 0 ? gTaken[slot] : 0; }
    };

    void spin(double seconds)
    {
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
    }

    // What a window's editor and the pump did between two snapshots.
    struct Snap
    {
        int draws = 0, asked = 0, taken = 0;          // taken: drawables bgfx took from the window's layer
        uint32_t frames = 0, skipped = 0, unchanged = 0;
        uint64_t pumpFrames = 0;
    };

    Snap snap(const Window& w)
    {
        Snap s;
        s.draws = w.panel->draws;
        s.asked = w.panel->asked;
        s.taken = w.taken();
        const auto d = w.editor->diagnostics();
        s.frames = d.frames;
        s.skipped = d.skipped;
        s.unchanged = d.unchanged;
        s.pumpFrames = funkgui::FramePump::get().framesRun();
        return s;
    }

    Snap since(const Window& w, const Snap& before)
    {
        const Snap now = snap(w);
        Snap d;
        d.draws = now.draws - before.draws;
        d.asked = now.asked - before.asked;
        d.taken = now.taken - before.taken;
        d.frames = now.frames - before.frames;
        d.skipped = now.skipped - before.skipped;
        d.unchanged = now.unchanged - before.unchanged;
        d.pumpFrames = now.pumpFrames - before.pumpFrames;
        return d;
    }

    // What macOS itself reports for the window of <view>, past the steer (see the header).
    const char* macOSReport(void* view)
    {
        const int was = gSteer;
        gSteer = -1;
        const bool occluded = funkgui::renderViewOccluded(view);
        gSteer = was;
        return occluded ? "occluded" : "visible";
    }

    double stamp = 0.0;

    // One frame by hand: the pump's tick, with no turn of the run loop (no other frame runs). Returns the draw calls
    // bgfx reports then, which are the PREVIOUS frame's: its frames are double-buffered, and getStats() reads the one
    // rendered before the last swap.
    uint32_t tickByHand()
    {
        stamp += 1.0 / 60.0;
        funkgui::FramePump::get().tick(stamp);
        const bgfx::Stats* st = bgfx::getStats();
        return st != nullptr ? st->numDraw : 0u;
    }
}

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    T::Probe P("fg.editorhost.skip", "", argc, argv);
    {
        Method m = class_getInstanceMethod([NSWindow class], @selector(occlusionState));
        if (m == nullptr)
        {
            P.harnessError("NSWindow has no occlusionState");
            return P.finish();
        }
        gOcclusionState = method_setImplementation(m, reinterpret_cast<IMP>(steeredOcclusionState));
    }
    auto& ctx = funkgui::BgfxContext::get();
    auto& prefs = funkgui::UiPreferences::get();
    stamp = juce::Time::getMillisecondCounterHiRes() * 0.001;

    NoOpProcessor processor;
    {
        // ---- open ----------------------------------------------------------------------------------------------------
        Window a(processor, 140, 140);
        spin(0.5);
        P.eq("open.attached", a.editor->surfaceAttached() ? 1 : 0, 1);
        P.ge("open.drew", a.panel->draws, 1);
        P.ge("open.submitted", a.editor->diagnostics().frames, 1);
        P.eq("open.counted", a.countDrawables(0) ? 1 : 0, 1);
        P.eq("open.reported_visible", funkgui::renderViewOccluded(renderViewOf(*a.editor)) ? 0 : 1, 1);
        std::printf("INFO     open: macOS itself reports the window %s\n", macOSReport(renderViewOf(*a.editor)));

        // ---- idle ----------------------------------------------------------------------------------------------------
        {
            const Snap s0 = snap(a);
            spin(1.0);
            const Snap d = since(a, s0);
            std::printf("INFO     idle: %d asks, %u skipped in a second\n", d.asked, d.skipped);
            P.ge("idle.asked", d.asked, 5);
            P.eq("idle.no_draws", d.draws, 0);
            P.eq("idle.no_frames", d.frames, 0);
            P.eq("idle.skipped_every_ask", d.skipped, d.asked);
            P.eq("idle.no_bgfx_frames", static_cast<int64_t>(d.pumpFrames), 0);
            P.eq("idle.nothing_presented", d.taken, 0);
        }

        // ---- change --------------------------------------------------------------------------------------------------
        {
            const Snap s0 = snap(a);
            a.panel->dirty = true;                   // asked for, but the same picture: the safety net
            spin(0.4);
            const Snap same = since(a, s0);
            P.ge("change.same_picture_recorded", same.draws, 1);
            P.eq("change.same_picture_unchanged", same.unchanged, same.draws);
            P.eq("change.same_picture_not_submitted", same.frames, 0);
            P.eq("change.same_picture_not_presented", same.taken, 0);
        }
        {
            const Snap s0 = snap(a);
            a.panel->recolour();
            spin(0.4);
            const Snap d = since(a, s0);
            P.ge("change.drew", d.draws, 1);
            P.ge("change.submitted", d.frames, 1);
            P.ge("change.bgfx_frame", static_cast<int64_t>(d.pumpFrames), 1);
            P.ge("change.presented", d.taken, 1);
            const Snap s1 = snap(a);
            spin(0.5);
            const Snap after = since(a, s1);
            P.eq("change.then_idle", after.draws, 0);
            P.eq("change.then_no_bgfx_frames", static_cast<int64_t>(after.pumpFrames), 0);
            P.eq("change.then_nothing_presented", after.taken, 0);
        }

        // ---- safety --------------------------------------------------------------------------------------------------
        {
            a.panel->alwaysAsk = true;
            const Snap s0 = snap(a);
            spin(1.0);
            const Snap d = since(a, s0);
            a.panel->alwaysAsk = false;
            P.ge("safety.recorded", d.draws, 5);
            P.eq("safety.all_unchanged", d.unchanged, d.draws);
            P.eq("safety.no_frames", d.frames, 0);
            P.eq("safety.no_bgfx_frames", static_cast<int64_t>(d.pumpFrames), 0);
            P.eq("safety.nothing_presented", d.taken, 0);
        }

        // ---- theme ---------------------------------------------------------------------------------------------------
        {
            const int was = prefs.theme();
            const Snap s0 = snap(a);
            prefs.setTheme(was == 0 ? 1 : 0);
            spin(0.4);
            const Snap d = since(a, s0);
            P.ge("theme.drew", d.draws, 1);
            P.ge("theme.submitted", d.frames, 1);
            const Snap s1 = snap(a);
            spin(0.4);
            P.eq("theme.then_idle", since(a, s1).draws, 0);
            const Snap s2 = snap(a);
            prefs.setTheme(was);
            spin(0.4);
            P.ge("theme.back_drew", since(a, s2).draws, 1);
        }

        // ---- resize --------------------------------------------------------------------------------------------------
        {
            const Snap s0 = snap(a);
            a.editor->setSize(300, 200);
            spin(0.4);
            const Snap d = since(a, s0);
            P.eq("resize.panel_size", a.panel->width() == 300 && a.panel->height() == 200 ? 1 : 0, 1);
            P.ge("resize.drew", d.draws, 1);
            P.ge("resize.submitted", d.frames, 1);
            const Snap s1 = snap(a);
            spin(0.4);
            P.eq("resize.then_idle", since(a, s1).draws, 0);
        }

        // ---- shown ---------------------------------------------------------------------------------------------------
        {
            // Hidden and shown with only the pump's own clock: the display link stops while the window is off screen.
            a.top.setVisible(false);
            spin(0.1);
            const Snap s0 = snap(a);
            spin(0.4);
            const Snap h = since(a, s0);
            P.eq("shown.nothing_while_hidden", h.draws, 0);
            a.top.setVisible(true);
            spin(0.5);
            const Snap u = since(a, s0);
            std::printf("INFO     shown, no frame while hidden: %d asks while hidden, then %d draws, %d drawables "
                        "taken\n", h.asked, u.draws, u.taken);

            // A frame while hidden (the pump's tick by hand, as a timer clock gives one): the editor learns that the
            // picture may be gone, and draws it again once shown.
            a.top.setVisible(false);
            spin(0.1);
            const Snap s1 = snap(a);
            stamp = juce::Time::getMillisecondCounterHiRes() * 0.001;
            tickByHand();
            P.eq("shown.hidden_tick_draws_nothing", since(a, s1).draws, 0);
            a.top.setVisible(true);
            spin(0.5);
            const Snap d = since(a, s1);
            std::printf("INFO     shown: %d draws, %d drawables taken; macOS itself reports the window %s\n", d.draws,
                        d.taken, macOSReport(renderViewOf(*a.editor)));
            P.ge("shown.drew", d.draws, 1);
            P.ge("shown.presented", d.taken, 1);
            const Snap s2 = snap(a);
            spin(0.4);
            P.eq("shown.then_idle", since(a, s2).draws, 0);
        }

        // ---- uncover: occluded, then visible again ------------------------------------------------------------------
        {
            gSteer = 0;
            const Snap s0 = snap(a);
            a.panel->recolour();
            spin(0.4);
            const Snap d = since(a, s0);
            P.ge("uncover.drew_while_occluded", d.draws, 1);
            P.ge("uncover.submitted_while_occluded", d.frames, 1);
            P.eq("uncover.not_presented_while_occluded", d.taken, 0);
            const Snap s1 = snap(a);
            spin(0.4);
            const Snap wait = since(a, s1);
            P.eq("uncover.waits_while_occluded", wait.draws, 0);
            P.eq("uncover.no_bgfx_frames_while_occluded", static_cast<int64_t>(wait.pumpFrames), 0);
            gSteer = 1;
            const Snap s2 = snap(a);
            spin(0.4);
            const Snap back = since(a, s2);
            std::printf("INFO     uncover: %d draws, %d drawables taken once reported visible\n", back.draws,
                        back.taken);
            P.ge("uncover.drew", back.draws, 1);
            P.ge("uncover.presented", back.taken, 1);
            const Snap s3 = snap(a);
            spin(0.4);
            const Snap after = since(a, s3);
            P.eq("uncover.then_idle", after.draws, 0);
            P.eq("uncover.then_nothing_presented", after.taken, 0);
        }

        // ---- mixed: frames by hand -------------------------------------------------------------------------------------
        Window b(processor, 520, 140);
        spin(0.5);
        P.eq("mixed.second_attached", b.editor->surfaceAttached() ? 1 : 0, 1);
        P.eq("mixed.second_counted", b.countDrawables(1) ? 1 : 0, 1);
        {
            b.panel->animate = true;
            const Snap sa = snap(a), sb = snap(b);
            uint32_t busy = 0;                       // frames 1-3: B alone (read after frames 2-4)
            for (int i = 0; i < 3; ++i)
                busy = tickByHand();
            P.eq("mixed.busy_drew_each", since(b, sb).draws, 3);
            P.eq("mixed.idle_drew_none", since(a, sa).draws, 0);
            P.eq("mixed.bgfx_frames", static_cast<int64_t>(since(b, sb).pumpFrames), 3);
            P.ge("mixed.busy_calls", busy, 1);
            P.eq("mixed.busy_presented_each", since(b, sb).taken, 3);
            P.eq("mixed.idle_not_presented", since(a, sa).taken, 0);

            a.panel->recolour();
            a.panel->host->nudgeFullRate();          // as input does: A's next frame at the next tick (0.25.0: a calm
                                                     // editor otherwise runs at its own twelfth of a second)
            tickByHand();                            // frame 4: A and B
            P.eq("mixed.idle_drew_once", since(a, sa).draws, 1);
            P.eq("mixed.idle_presented_once", since(a, sa).taken, 1);
            const uint32_t both = tickByHand();      // frame 5: B alone; reports frame 4
            const uint32_t again = tickByHand();     // frame 6: B alone; reports frame 5
            std::printf("INFO     mixed: %u draw calls a frame with one editor drawing, %u with both, then %u\n", busy,
                        both, again);
            P.ge("mixed.both_more_calls", both, busy + 1);
            P.eq("mixed.idle_view_not_drawn", again, busy);
            P.eq("mixed.idle_drew_once_only", since(a, sa).draws, 1);
            P.eq("mixed.idle_presented_once_only", since(a, sa).taken, 1);

            b.panel->animate = false;
            tickByHand();                            // B's last frame at rest (not dirty: nothing drawn)

            // The roles swapped: A (the primary, bgfx's default swapchain) busy, B idle.
            a.panel->animate = true;
            const Snap ta = snap(a), tb = snap(b);
            for (int i = 0; i < 3; ++i)
                tickByHand();
            P.eq("mixed.primary_busy_presented_each", since(a, ta).taken, 3);
            P.eq("mixed.secondary_idle_drew_none", since(b, tb).draws, 0);
            P.eq("mixed.secondary_idle_not_presented", since(b, tb).taken, 0);
            a.panel->animate = false;
            tickByHand();                            // A's last frame at rest

            const Snap s1 = snap(b);
            for (int i = 0; i < 3; ++i)
                tickByHand();
            P.eq("mixed.all_idle_no_bgfx_frames", static_cast<int64_t>(since(b, s1).pumpFrames), 0);
        }

        // ---- closed: an editor closed while the others are idle ------------------------------------------------------
        {
            {
                Window c(processor, 140, 420);
                spin(0.4);
                P.ge("closed.third_drew", c.panel->draws, 1);
                const Snap sa = snap(a), sb = snap(b);
                c.close();
                P.ge("closed.view_held", ctx.retiringViews(), 1);
                spin(0.6);
                P.eq("closed.view_let_go", ctx.retiringViews(), 0);
                P.ge("closed.frames_ran", static_cast<int64_t>(since(a, sa).pumpFrames), 2);
                P.eq("closed.others_idle", since(a, sa).draws + since(b, sb).draws, 0);
            }
        }

        // ---- rebuilt: the primary closed, bgfx starts again on the survivor ------------------------------------------
        {
            const Snap sb = snap(b);
            a.close();
            spin(0.5);
            const Snap d = since(b, sb);
            P.eq("rebuilt.survivor_attached", b.editor->surfaceAttached() ? 1 : 0, 1);
            P.ge("rebuilt.survivor_drew", d.draws, 1);   // a new surface: drawn whatever macOS reports
            P.ge("rebuilt.survivor_submitted", d.frames, 1);
        }
        b.close();
        spin(0.2);
    }
    P.eq("teardown.bgfx_down", ctx.valid() ? 0 : 1, 1);
    P.eq("teardown.nothing_held", ctx.retiringViews(), 0);
    return P.finish();
}

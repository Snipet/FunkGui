#pragma once

#include <juce_events/juce_events.h>
#include <memory>
#include <vector>

namespace funkgui
{
    // One redraw clock for every open editor in the process.
    //
    // bgfx is a process-wide singleton and bgfx::frame() presents *all* views
    // at once, so it must be called exactly once per frame no matter how many
    // editors are open. Previously each editor ran its own 60 Hz juce::Timer
    // and called frame() from it: eight instances meant eight unsynchronised
    // timers and eight presents per frame, each able to block the message
    // thread inside nextDrawable once Metal's in-flight drawable limit was
    // reached.
    //
    // The pump drives every client from a single CADisplayLink and calls
    // bgfx::frame() once at the end. When no link can be created yet (no
    // window, or the GPU path is unavailable) it falls back to a plain timer,
    // so the editor still runs — just without refresh-rate sync.
    //
    // v0.21.0: the link is on one client's view, and a link stops while its
    // view is off screen (its window ordered out or minimised: a host that
    // shows one plug-in window at a time hides the first editor's to show the
    // second's). Every editor then froze, the second never drew a frame, and
    // its framebuffer's creation waited in bgfx's queue for a view the host
    // later freed. The link now goes on a client whose view is on screen
    // (viewOnScreen), and a watchdog (four times a second) moves it to one
    // when it has not fired for a quarter of a second.
    //
    // Each editor its own cadence (0.25.0): the pump ticks at full rate
    // (kFullHz) while any client asks for it and at kIdleHz otherwise, as
    // before, but a tick runs only the clients that want it. A client that
    // asked for full rate at its last frame (Result::wantsFullRate), or was
    // nudged since (nudgeFullRate(c): input), runs at every tick; a calm one
    // runs at the tick nearest a twelfth of a second after its last frame, so
    // it keeps the idle rate while another editor runs at 60 Hz, and its dt
    // is the time since its own last frame. While the pump itself idles, or
    // runs on its pinned clock (forceFallbackClock: captures), every client
    // runs at every tick, so one editor alone runs exactly as before. Before
    // 0.25.0 one moving editor put every open editor on 60 Hz, each recording
    // a frame per tick. The rules are src/gpu/ClientCadence.h.
    //
    // v0.25.1: a nudge also holds its client at full rate for a quarter of
    // a second of the pump's clock (ClientCadence::kHoldSec), and the pump
    // with it, so the frames of an interaction (a hover, a click) come at
    // the display's rate rather than at the idle rate between its events.
    // wantedFullRate(c) still reports the client's own request. And bgfx
    // presents every frame before bgfx::frame() returns, so the last frame
    // drawn before the pump runs none is on screen (src/gpu/BgfxReset.h).
    namespace detail { class ClientCadence; }

    class FrameClient
    {
    public:
        virtual ~FrameClient() = default;

        struct Result
        {
            bool submitted = false;      // drew anything this frame (0.25.0: false when it left its last picture up)
            bool wantsFullRate = false;  // something is moving or being touched
        };

        // Advance animation and submit draw calls. Must NOT call bgfx::frame().
        virtual Result submitFrame(float dtSeconds) = 0;

        // The NSView the display link should follow, or nullptr if this client
        // is not yet attached to a window.
        virtual void* frameClockView() const = 0;
    };

    class FramePump : private juce::Timer
    {
    public:
        static FramePump& get();

        void add(FrameClient* c);
        void remove(FrameClient* c);

        // Entry point for the display link. Public because the C callback in
        // DisplayLink.mm has to reach it.
        void tick(double timestampSec);

        // Go to the full refresh rate immediately rather than at the end of
        // the next frame. Called from input handlers: coming out of the idle
        // cadence one frame late would put up to 80 ms between the pointer
        // arriving and the hover animation starting to move. This form
        // nudges every client: each runs at the next tick.
        void nudgeFullRate();

        // 0.25.0: the same for one client (EditorHost's own nudgeFullRate()):
        // it runs at the next tick, and the pump goes to full rate; the other
        // clients keep their own cadence. A nudge during the client's own
        // frame holds: it runs at the next tick too. A client the pump does
        // not hold nudges only the pump's rate.
        void nudgeFullRate(const FrameClient* c);

        // Drop any display link bound to this client's view, and rebind to a
        // surviving one. Must be called before the client destroys the view it
        // handed out — the link is attached to that view, and outliving it is
        // a use-after-free on a path (host re-parenting the editor) that hosts
        // genuinely take.
        void releaseClockFor(const FrameClient* c);

        // Pin the pump to its fallback timer. A CADisplayLink correctly stops
        // delivering callbacks while its window is occluded, which is what we
        // want in production and useless for the offscreen capture harness —
        // it made a canvas dump depend on whether the plugin window happened
        // to be in front. Diagnostics only.
        void forceFallbackClock();

        // True when frames are coming from a CADisplayLink rather than the
        // fallback timer. Diagnostics only.
        bool isDisplayLinked() const { return link_ != nullptr; }

        // Measured over the last second, and which cadence was last asked
        // for. Diagnostics only — but the only way to tell whether a rate
        // request is actually being honoured by the display.
        float measuredFps()  const { return fps_; }
        bool  wantedFullRate() const { return ratePref_ == 1; }

        // 0.25.0: the same for one client, on its own cadence: its frames
        // over windows of at least a second of the pump's clock, the first
        // opening at its first frame; and whether it asks for full rate (its
        // last frame's Result::wantsFullRate, or a nudge since; during its
        // frame, the request before that frame). 0 and false for a client the
        // pump does not hold. EditorHost reports these (Diagnostics::fps,
        // FrameInfo::fps and ::fullRate).
        float measuredFps(const FrameClient* c) const;
        bool  wantedFullRate(const FrameClient* c) const;

        // How often the watchdog moved the link off a view that had gone off
        // screen (v0.21.0). Diagnostics and fg.editorhost.multi.
        int   linkRebinds() const { return rebinds_; }

        // 0.25.0: the bgfx::frame() calls the pump has made. A tick runs one
        // when any client submitted, or when bgfx has work waiting for a frame
        // (BgfxContext::framePending: a closed editor's view to let go); none
        // while every editor leaves its last picture up, so nothing is drawn
        // or presented. Diagnostics and fg.editorhost.skip.
        uint64_t framesRun() const { return framesRun_; }

        ~FramePump() override;                   // never runs (get()); out of line, where ClientCadence is complete

    private:
        FramePump();

        void timerCallback() override;   // fallback clock
        void updateClock();
        void requestRate(bool fullRate);
        void watch();                    // the watchdog's: a link that stopped firing is moved (v0.21.0)
        bool bindLink(bool onScreenOnly);
        detail::ClientCadence* cadenceOf(const FrameClient* c) const;

        struct Watchdog final : juce::Timer
        {
            explicit Watchdog(FramePump& p) : pump(p) {}
            void timerCallback() override { pump.watch(); }
            FramePump& pump;
        };
        Watchdog watchdog_{ *this };
        double lastTickWall_ = 0.0;      // when tick() last ran, wall clock (seconds)
        int    rebinds_ = 0;
        uint64_t framesRun_ = 0;         // 0.25.0: bgfx::frame() calls (framesRun)

        // The clients, in the order they were added, each with its own
        // cadence (0.25.0).
        struct Slot
        {
            FrameClient* client = nullptr;
            std::unique_ptr<detail::ClientCadence> cadence;
        };
        std::vector<Slot> clients_;
        std::vector<FrameClient*> tickClients_;  // tick()'s snapshot of the clients, capacity kept
        void*  link_ = nullptr;
        const FrameClient* linkOwner_ = nullptr;
        double lastStampSec_ = 0.0;
        bool   haveStamp_ = false;
        int    ratePref_ = -1;           // -1 unset, 0 idle, 1 full
        bool   inTick_ = false;
        bool   forceTimer_ = false;
        int    fpsCount_ = 0;
        double fpsWindowStart_ = 0.0;
        float  fps_ = 0.0f;

        static constexpr float kIdleHz = 12.0f;
        static constexpr float kFullHz = 60.0f;
    };
}

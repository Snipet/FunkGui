#pragma once

#include <juce_events/juce_events.h>
#include <vector>

namespace hrvbgui
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
    class FrameClient
    {
    public:
        virtual ~FrameClient() = default;

        struct Result
        {
            bool submitted = false;      // drew anything this frame
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
        // arriving and the hover animation starting to move.
        void nudgeFullRate();

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

    private:
        FramePump() = default;

        void timerCallback() override;   // fallback clock
        void updateClock();
        void requestRate(bool fullRate);

        std::vector<FrameClient*> clients_;
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

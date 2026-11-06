#pragma once

// One editor's cadence in the FramePump (0.25.0; gpu/FramePump.h "Each editor its own cadence"): whether a tick of
// the pump runs that editor's frame, the dt the frame gets, the editor's own rate request and its measured rate. Plain
// C++ with no JUCE and no bgfx, so fg.framepump.cadence drives it with made-up ticks in every configuration. Message
// thread, like FramePump.
//
// - Every editor runs at every tick while the pump is not at full rate (no editor asked for it at the last tick and
//   none was nudged since: the pump's ticks are the idle cadence already) or runs on its pinned clock
//   (FramePump::forceFallbackClock, a capture: its frame counts stand). One editor alone therefore runs exactly as
//   before 0.25.0.
// - At full rate an editor runs at every tick when it has not run yet, asked for full rate at its last frame
//   (Panel::wantsFullRate()), was nudged since (input: FramePump::nudgeFullRate), or is held (below). A calm one (none
//   of these) runs at the tick nearest a twelfth of a second (kIdleHz, the pump's idle rate) after its last frame: the
//   first tick at which the time since that frame plus half the tick's own dt reaches 1/12 s. That is every fifth tick
//   at 60 Hz, every tenth at 120 Hz, every sixth at 72 Hz and every fifth of a 16 ms timer, so a calm editor keeps
//   12 Hz while another runs at 60. Before 0.25.0 every editor ran at every tick of the pump, which ran at full rate
//   while any one editor asked for it.
// - dt: the pump's dts summed since the editor's last frame, clamped to 1 ms .. 100 ms as the pump clamps its own.
// - The rate request (fullRate()): between frames, the last frame's wantsFullRate() or a nudge since; during a frame,
//   the request before it or a nudge since (FrameInfo::fullRate: the request before the frame being recorded). A
//   nudge during the editor's own frame holds: the next tick runs it too.
// - The hold (v0.25.1, held()): a nudge also keeps the editor at full rate for kHoldSec of the pump's clock from the
//   frame that takes it (the frame it runs at the next tick, or the frame it came during): every tick whose dts since
//   that frame sum to less than kHoldSec runs the editor, and the pump stays at full rate (FramePump::tick asks for
//   it while any editor is held). Each nudge starts the hold again. Without it a hover, which changes at once and
//   animates nothing, asked for full rate only until the frame that drew it, so the pump went back to the idle rate
//   after each input frame and the frame for the next pointer event waited for an idle tick (up to a twelfth of a
//   second). The hold is the pump's, not a request: fullRate() and FrameInfo::fullRate do not report it, so a capture's
//   frames (UI_KEYS nudges as it replays) record what they did before. At 60 Hz the hold is the nudged frame and the
//   14 ticks after it; then the editor waits for its twelfth of a second as any calm one.
// - fps: the editor's frames over windows of at least one second of the pump's clock, the first window opening at its
//   first frame (WebClock's FrameCadence measures the same way).

namespace funkgui::detail
{
    class ClientCadence
    {
    public:
        static constexpr double kIdleHz = 12.0;      // FramePump's idle rate: a calm editor's own
        static constexpr double kHoldSec = 0.25;     // v0.25.1: how long a nudge holds the editor at full rate

        // A tick of the pump, `dt` seconds after the one before (the pump's dt, clamped). `everyTick`: the pump is not
        // at full rate, or runs on its pinned clock. Whether this editor's frame runs at this tick.
        constexpr bool tick(double dt, bool everyTick) noexcept
        {
            owedSec_ += dt;
            holdSec_ = holdSec_ > dt ? holdSec_ - dt : 0.0;
            if (everyTick || !started_ || fullRate() || held())
                return true;
            return owedSec_ + 0.5 * dt >= 1.0 / kIdleHz;
        }

        // The editor's frame runs now, at the pump's clock `nowSec`: the dt to give it. Counts the frame for fps().
        constexpr float begin(double nowSec) noexcept
        {
            float dt = static_cast<float>(owedSec_);
            dt = !(dt >= 0.001f) ? 0.001f : (dt > 0.1f ? 0.1f : dt);
            owedSec_ = 0.0;
            if (nudged_)
                holdSec_ = kHoldSec;                 // the hold runs from this frame
            full_ = full_ || nudged_;                // the request before this frame: fullRate() during it
            nudged_ = false;
            if (!started_)
            {
                started_ = true;
                windowStartSec_ = nowSec;            // the first frame opens the first window
            }
            else
            {
                ++windowFrames_;
            }
            if (nowSec - windowStartSec_ >= 1.0)
            {
                fps_ = static_cast<float>(windowFrames_ / (nowSec - windowStartSec_));
                windowFrames_ = 0;
                windowStartSec_ = nowSec;
            }
            return dt;
        }

        // The editor's frame ran and asked for `wantsFullRate`. A nudge since begin() holds for the next tick, and
        // starts the hold from this frame.
        constexpr void end(bool wantsFullRate) noexcept
        {
            if (nudged_)
                holdSec_ = kHoldSec;
            full_ = wantsFullRate || nudged_;
            nudged_ = false;
        }

        // Input arrived, or the editor has something to show soon: it runs at the next tick, and the pump goes to full
        // rate; the frame then starts the hold (held()).
        constexpr void nudge() noexcept { nudged_ = true; }

        constexpr bool  fullRate() const noexcept { return full_ || nudged_; }
        constexpr bool  held() const noexcept { return holdSec_ > 0.0; }   // v0.25.1: the hold after a nudge
        constexpr float fps() const noexcept { return fps_; }
        constexpr bool  started() const noexcept { return started_; }

    private:
        double owedSec_ = 0.0;                       // the pump's dts since the last frame
        double windowStartSec_ = 0.0;
        int    windowFrames_ = 0;
        float  fps_ = 0.0f;
        bool   started_ = false;                     // a frame has run
        bool   full_ = false;                        // the last frame asked for full rate (or a nudge held)
        bool   nudged_ = false;                      // nudged since the last frame began
        double holdSec_ = 0.0;                       // v0.25.1: what is left of the hold after the last nudge
    };
}

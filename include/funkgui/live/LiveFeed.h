#pragma once

// Is the telemetry stream alive (02 §5.10; HR BgfxEditor.cpp:966-980, isLive() in BgfxEditor.h:176)? The audio thread
// publishes a frame per block while an editor is attached, and every publish increments the frame's publishCount. The
// editor polls once per tick: a new count means the stream is live and fresh; a count that stops moving, or reads that
// keep failing (a torn seqlock read), age it, and after `staleAfter` seconds (0.5 s) it is stale, so the display falls
// back to what the parameters alone say instead of freezing on the last frame (02 §6.5, §9.1).
//
// G6 (the design gives the interface; the rules are HR's, made exact):
// - poll(read, dt) calls read(frame) once. read returns true when it produced a consistent frame. A consistent frame
//   whose publishCount differs from the last new one is new: the age drops to 0 and the feed is live. Anything else (a
//   failed read, the same count again) adds dt to the age (dt <= 0 or NaN adds nothing). Counts are compared for
//   inequality only, so a wrapped counter is still new.
// - publishCount 0 is "nothing published yet" (HR's lastPublish_ starts at 0): a frame that still carries 0 is never
//   new, so an attached-but-silent processor is never shown as live.
// - frame() is the last consistent frame read (also when its count was not new, as HR copied it); a failed read leaves
//   it unchanged, so a torn read never reaches the display. Before the first consistent read it is a value-initialised
//   Frame.
// - live() = a new count has been seen and age < staleAfter; fresh() = the last poll saw a new count; staleSeconds() =
//   the age (0 until something ages it).
// - No clock is read: time is the dt the Panel ticks with (02 §3.7 rule 1). Message thread; the frame is a copy.
//
// Frame must be default-constructible and copyable and have a member `publishCount` convertible to uint32_t
// (fcdsp::UiFrame, 01 §6.2).

#include <cstdint>

namespace funkgui
{
    // The staleness rule without the frame (G6 addition): what LiveFeed<Frame> uses, testable on its own.
    class LiveState
    {
    public:
        explicit LiveState(float staleAfter = 0.5f) noexcept;

        void  observe(bool readOk, uint32_t publishCount, float dt) noexcept;   // one poll's outcome
        bool  live() const noexcept;
        bool  fresh() const noexcept;
        float staleSeconds() const noexcept;
        float staleAfter() const noexcept { return staleAfter_; }

    private:
        float    staleAfter_;
        float    age_ = 0.0f;
        uint32_t last_ = 0;                      // the last new publishCount (0: none yet)
        bool     seen_ = false;                  // a new count has been seen
        bool     fresh_ = false;
    };

    template <class Frame>
    class LiveFeed
    {
    public:
        explicit LiveFeed(float staleAfter = 0.5f) : state_(staleAfter) {}

        // read(Frame&) -> bool (the seqlock reader).
        template <class ReadFn>
        void poll(ReadFn&& read, float dt)
        {
            const bool ok = static_cast<bool>(read(scratch_));
            if (ok)
                frame_ = scratch_;
            state_.observe(ok, ok ? static_cast<uint32_t>(frame_.publishCount) : 0u, dt);
        }

        bool  live() const { return state_.live(); }
        bool  fresh() const { return state_.fresh(); }
        float staleSeconds() const { return state_.staleSeconds(); }
        const Frame& frame() const { return frame_; }

        const LiveState& state() const noexcept { return state_; }   // G6 addition

    private:
        LiveState state_;
        Frame     frame_{};
        Frame     scratch_{};                    // a failed read may have written into it: never shown
    };
}

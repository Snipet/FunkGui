#pragma once

// A web host's frame clock and UI zoom arithmetic (v0.13.0; FCompressor ADR-93, web Sprint C): the pure parts of
// WebHost, the counterparts of FramePump (src/gpu/FramePump.cpp) and of EditorHost's zoom (src/gpu/EditorHost.cpp
// "Zoom"). Plain C++, header-only, no Emscripten and no DOM, so both are tested natively and under node
// (fg.web.clock); WebHost (src/web/WebHost.cpp) does the requestAnimationFrame calls and the canvas sizing.
//
// FrameCadence: a browser's only vsync clock is requestAnimationFrame, which runs at the display's rate (60, 120,
// 144 Hz), has no rate request and does not run in a hidden tab. FramePump's cadence over it:
// - dt: the first frame's is 1/60 s; after that the time since the frame before, clamped to 1 ms .. 100 ms (a hidden
//   tab, a breakpoint or a slow frame must not make every ease jump to its target).
// - fps: frames over windows of at least one second, the first window opening at the first frame.
// - Full rate (something moves, or input arrived: nudge()): the first vsync at least kFullMinMs after the frame
//   before. That is every vsync up to 75 Hz and an even divisor above: 60 on a 60, 120 or 240 Hz display, 72 on
//   144 Hz, 55 on 165 Hz (CADisplayLink's preferred 60 gives the native host the same near-60 rates).
// - Idle (nothing moves): one frame per 1/12 s, FramePump's kIdleHz, so hover and host automation are still polled.
//   A host does not ask for every vsync in between: waitMs() is how long it sleeps before it requests the animation
//   frame that will be drawn.
// - A nudge is full rate until the next frame's own request: the frame it causes is drawn at the next vsync.
// - Hidden: nothing is due.
// A host calls, per animation frame: due(now, hidden); if so advance(now) for the dt, its frame, then
// requestRate(what the Panel wants); then waitMs(now) before the next requestAnimationFrame.
//
// Zoom: steps cleaned as EditorHost cleans them (25 .. 400 %, ascending, each once), the default as the nearest
// listed step (the smaller on a tie), a zoomed size as round(logical * percent / 100) with halves away from zero, and
// the fit: the largest step at or below the chosen one whose zoomed size fits an area, the smallest step when none
// does, the chosen one as it is when the area is unknown.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <span>
#include <vector>

namespace funkgui::web
{
    class FrameCadence
    {
    public:
        static constexpr double kFullHz = 60.0, kIdleHz = 12.0;      // FramePump's
        // Full rate draws at the first vsync this long after the last frame: below 75 Hz's 13.3 ms and above the
        // 12.5 ms that three vsyncs of a 240 Hz display take, so neither display sits on the threshold.
        static constexpr double kFullMinMs = 12.8;
        // Idle draws at the first vsync within 4 ms of a twelfth of a second (five vsyncs at 60 Hz are 83.3 ms).
        static constexpr double kIdleMinMs = 1000.0 / kIdleHz - 4.0;
        // An idle host sleeps until one full-rate frame before its next frame is due, then asks for a vsync.
        static constexpr double kIdleWaitMs = 1000.0 / kIdleHz - 1000.0 / kFullHz;

        // Whether the animation frame at `nowMs` (requestAnimationFrame's timestamp) is one to draw.
        constexpr bool due(double nowMs, bool hidden) const noexcept
        {
            if (hidden)
                return false;
            if (!stamped_)
                return true;
            const double elapsed = nowMs - lastMs_;
            return elapsed < 0.0 || elapsed >= (rate_ == 1 ? kFullMinMs : kIdleMinMs);
        }

        // The frame at `nowMs` is being drawn: its dt in seconds. Counts the frame for fps().
        constexpr float advance(double nowMs) noexcept
        {
            float dt = 1.0f / 60.0f;
            if (stamped_)
                dt = static_cast<float>((nowMs - lastMs_) * 0.001);
            dt = !(dt >= 0.001f) ? 0.001f : (dt > 0.1f ? 0.1f : dt);   // a clock gone back, or NaN, is 1 ms too
            if (!stamped_)
                windowStartMs_ = nowMs;                              // the first frame opens the first window
            else
                ++windowFrames_;
            if (nowMs - windowStartMs_ >= 1000.0)
            {
                fps_ = static_cast<float>(windowFrames_ * 1000.0 / (nowMs - windowStartMs_));
                windowFrames_ = 0;
                windowStartMs_ = nowMs;
            }
            lastMs_ = nowMs;
            stamped_ = true;
            return dt;
        }

        // What the frame just drawn asks of the clock (Panel::wantsFullRate()). It ends a nudge.
        constexpr void requestRate(bool fullRate) noexcept { rate_ = fullRate ? 1 : 0; }

        // Input arrived: full rate from the next vsync, until the next frame's own request.
        constexpr void nudge() noexcept { rate_ = 1; }

        // The rate last asked for (FrameInfo::fullRate: the request before the frame being recorded).
        constexpr bool wantedFullRate() const noexcept { return rate_ == 1; }

        // Frames per second over the last whole window; 0 until one has passed.
        constexpr float fps() const noexcept { return fps_; }

        // How long to wait at `nowMs` before requesting the next animation frame, in ms; 0 = request it now. `nowMs`
        // is on the clock of the timestamps given to due() and advance(): performance.now() for a browser's frames.
        constexpr double waitMs(double nowMs) const noexcept
        {
            if (rate_ == 1 || !stamped_)
                return 0.0;
            const double wait = lastMs_ + kIdleWaitMs - nowMs;
            return wait > 0.0 ? wait : 0.0;
        }

        // The clock stopped: the next frame is a first frame again (dt 1/60 s) and no rate is asked for.
        constexpr void reset() noexcept { *this = FrameCadence{}; }

    private:
        double lastMs_ = 0.0;                        // the last drawn frame
        bool   stamped_ = false;
        int    rate_ = -1;                           // -1 unset, 0 idle, 1 full (FramePump's ratePref_)
        double windowStartMs_ = 0.0;
        int    windowFrames_ = 0;
        float  fps_ = 0.0f;
    };

    // ---- zoom -------------------------------------------------------------------------------------------------------

    inline constexpr int kMinZoomPercent = 25, kMaxZoomPercent = 400;       // EditorHost's, and UI_ZOOM's

    // A config's zoom steps as a host offers them: those in 25 .. 400, ascending, each once.
    inline std::vector<int> cleanZoomSteps(std::span<const int> steps)
    {
        std::vector<int> out;
        for (const int s : steps)
            if (s >= kMinZoomPercent && s <= kMaxZoomPercent)
                out.push_back(s);
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    // The step of `steps` (ascending) nearest to `want`, the smaller on a tie; 100 when there are no steps.
    inline int nearestZoomStep(std::span<const int> steps, int want) noexcept
    {
        if (steps.empty())
            return 100;
        int best = steps.front();
        for (const int s : steps)
            if (std::abs(s - want) < std::abs(best - want))
                best = s;
        return best;
    }

    // round(logical * percent / 100), halves away from zero (EditorHost::zoomedSize).
    inline int zoomedSize(int logical, int percent) noexcept
    {
        return percent == 100 ? logical : static_cast<int>(std::lround(static_cast<double>(logical) * percent / 100.0));
    }

    // The zoom a W x H Panel is drawn at when `chosen` is asked for and areaW x areaH px are there for it: the
    // largest step (ascending `steps`) at or below `chosen` whose zoomed size fits; the smallest step when none fits;
    // `chosen` itself when the area is not known (a side <= 0) or there are no steps (EditorHost::fitZoom).
    inline int fitZoom(std::span<const int> steps, int chosen, int logicalW, int logicalH, int areaW,
                       int areaH) noexcept
    {
        if (areaW <= 0 || areaH <= 0 || steps.empty())
            return chosen;
        int best = 0;
        for (const int s : steps)
        {
            if (s > chosen)
                break;
            if (zoomedSize(logicalW, s) <= areaW && zoomedSize(logicalH, s) <= areaH)
                best = s;
        }
        return best > 0 ? best : steps.front();
    }
}

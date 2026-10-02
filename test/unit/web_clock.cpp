// FUNKGUI_TEST name=fg.web.clock timeout=120 gpu=0 links=harness
//
// fg.web.clock (v0.13.0; FCompressor ADR-93, web Sprint C): web/WebClock.h, the pure parts of WebHost's frame clock
// and UI zoom. Header-only and free of Emscripten, so the same rows run natively with and without JUCE and as wasm32
// under node. The clock is driven as a browser drives a host: vsyncs at a display's rate, an animation frame
// delivered at the first vsync after it was requested, the host's callback running a little after the vsync's
// timestamp, and a timer for the waits FrameCadence asks for.
//   dt.*      the first frame's dt is 1/60 s; after that the time since the last frame, clamped to 1 ms .. 100 ms;
//             reset() makes the next frame a first frame
//   fps.*     measured over one-second windows, 0 before the first has passed
//   full.*    full rate on 60, 75, 90, 120, 144, 165 and 240 Hz displays: every vsync up to 75 Hz, an even divisor
//             near 60 Hz above, evenly spaced
//   idle.*    12 frames a second on every display, without asking for every vsync in between
//   nudge.*   a nudge during an idle wait is drawn at the next vsync and lasts until the frame's own request
//   hidden.*  nothing is due while the page is hidden
//   wait.*    how long a host sleeps before its next requestAnimationFrame
//   rate.*    wantedFullRate(): the request before the frame, FramePump's rule
//   zoom.*    cleanZoomSteps, nearestZoomStep, zoomedSize and fitZoom against the cases of fg.editorhost.zoom
//             (config.*, fit.*), which needs a GPU build and a display
// Spec rows only.

#include <funkgui/test/Harness.h>
#include <funkgui/web/WebClock.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace T = funkgui::test;
namespace web = funkgui::web;
using web::FrameCadence;

namespace
{
    bool sameBits(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }

    constexpr double kCallbackLagMs = 2.0;           // a callback runs this long after its vsync's timestamp

    // A display and a host's clock loop over it (WebHost's: src/web/WebHost.cpp).
    struct Browser
    {
        explicit Browser(double hz) : periodMs(1000.0 / hz) {}

        double periodMs;
        FrameCadence cadence;
        std::vector<double> drawn;                   // the vsync time of every frame drawn
        int    callbacks = 0;                        // animation frames delivered
        bool   hidden = false;
        bool   panelWantsFullRate = false;
        double nudgeAtMs = -1.0;                     // input arrives at this time (once)

        // Runs the loop from `fromMs` (the time the host first asks for an animation frame) until `toMs`.
        void run(double fromMs, double toMs)
        {
            double request = fromMs;
            for (;;)
            {
                const double vsync = (std::floor(request / periodMs) + 1.0) * periodMs;     // the first one after
                if (vsync > toMs)
                    return;
                ++callbacks;
                if (cadence.due(vsync, hidden))
                {
                    cadence.advance(vsync);
                    drawn.push_back(vsync);
                    cadence.requestRate(panelWantsFullRate);
                }
                const double now = vsync + kCallbackLagMs;
                request = now + cadence.waitMs(vsync);
                if (nudgeAtMs >= now && nudgeAtMs < request)
                {
                    cadence.nudge();                 // the host drops its timer and asks for a frame at once
                    request = nudgeAtMs;
                    nudgeAtMs = -1.0;
                }
            }
        }

        int drawnBetween(double fromMs, double toMs) const
        {
            int n = 0;
            for (const double t : drawn)
                n += t >= fromMs && t < toMs ? 1 : 0;
            return n;
        }

        // The largest and smallest gap between consecutive frames drawn in [fromMs, toMs), in vsyncs.
        void gaps(double fromMs, double toMs, int& smallest, int& largest) const
        {
            smallest = 1 << 30;
            largest = 0;
            for (size_t i = 1; i < drawn.size(); ++i)
                if (drawn[i - 1] >= fromMs && drawn[i] < toMs)
                {
                    const int g = static_cast<int>(std::lround((drawn[i] - drawn[i - 1]) / periodMs));
                    smallest = g < smallest ? g : smallest;
                    largest = g > largest ? g : largest;
                }
        }
    };

    // The rules are usable in constant expressions.
    constexpr float firstDt()
    {
        FrameCadence c;
        return c.advance(1234.5);
    }
    static_assert(firstDt() == 1.0f / 60.0f);
}

int main(int argc, char** argv)
{
    T::Probe P("fg.web.clock", "", argc, argv);

    // ---- dt ---------------------------------------------------------------------------------------------------------
    {
        FrameCadence c;
        P.eq("dt.first_is_a_sixtieth", sameBits(c.advance(5000.0), 1.0f / 60.0f), 1);
        P.near("dt.follows_the_stamps", c.advance(5016.5), 0.0165, 1.0e-6);
        P.eq("dt.clamped_at_1ms", sameBits(c.advance(5016.75), 0.001f), 1);
        P.eq("dt.clamped_at_100ms", sameBits(c.advance(5016.75 + 250.0), 0.1f), 1);
        P.near("dt.just_inside", c.advance(5266.75 + 99.0), 0.099, 1.0e-6);
        P.eq("dt.clock_gone_back_is_1ms", sameBits(c.advance(5000.0), 0.001f), 1);
        c.reset();
        P.eq("dt.first_again_after_reset", sameBits(c.advance(9000.0), 1.0f / 60.0f), 1);
        P.eq("dt.reset_forgets_the_rate", c.wantedFullRate(), 0);
    }

    // ---- fps --------------------------------------------------------------------------------------------------------
    {
        Browser b(60.0);
        b.panelWantsFullRate = true;
        b.run(100.0, 600.0);
        P.near("fps.zero_before_a_second", b.cadence.fps(), 0.0, 0.0);
        b.run(600.0, 3200.0);
        P.near("fps.sixty", b.cadence.fps(), 60.0, 0.5);
        Browser idle(60.0);
        idle.run(100.0, 3200.0);
        P.near("fps.twelve_when_idle", idle.cadence.fps(), 12.0, 0.2);
    }

    // ---- full rate: capped near 60 Hz, evenly spaced ----------------------------------------------------------------
    {
        const struct { int hz; int frames; int everyNth; } cases[] = {
            { 60, 60, 1 }, { 75, 75, 1 }, { 90, 45, 2 }, { 120, 60, 2 }, { 144, 72, 2 }, { 165, 55, 3 }, { 240, 60, 4 },
        };
        for (const auto& c : cases)
        {
            Browser b(c.hz);
            b.panelWantsFullRate = true;
            b.run(3.0, 3100.0);
            const std::string k = "full." + std::to_string(c.hz) + "hz";
            P.eq(k + ".frames_per_second", b.drawnBetween(1000.0 - 0.01, 2000.0 - 0.01), c.frames);
            int smallest = 0, largest = 0;
            b.gaps(500.0, 3000.0, smallest, largest);
            P.eq(k + ".every_nth_vsync", smallest == c.everyNth && largest == c.everyNth, 1);
        }
    }

    // ---- idle: 12 Hz, and no animation frame asked for in between ---------------------------------------------------
    for (const int hz : { 60, 120, 144 })
    {
        Browser b(hz);
        b.run(3.0, 3100.0);
        const std::string k = "idle." + std::to_string(hz) + "hz";
        P.eq(k + ".frames_per_second", b.drawnBetween(1000.0 - 0.01, 2000.0 - 0.01), 12);
        // At most a few vsyncs are looked at per frame drawn (the wait ends one 60 Hz frame early), never all of them.
        P.le(k + ".callbacks_per_frame", static_cast<double>(b.callbacks) / static_cast<double>(b.drawn.size()), 3.0);
        P.eq(k + ".first_frame_at_once", b.drawn.front() <= 1000.0 / hz + 0.01, 1);
    }

    // ---- a nudge ----------------------------------------------------------------------------------------------------
    {
        // Idle at 60 Hz: frames at 16.67, 100, 183.3, 266.7 ms. Input at 120 ms, during the wait for 183.3 ms.
        Browser b(60.0);
        b.nudgeAtMs = 120.0;
        b.run(3.0, 400.0);
        const double vsyncAfter = 8.0 * 1000.0 / 60.0;       // 133.3 ms
        bool atNextVsync = false;
        for (const double t : b.drawn)
            atNextVsync = atNextVsync || std::fabs(t - vsyncAfter) < 0.01;
        P.eq("nudge.drawn_at_the_next_vsync", atNextVsync, 1);
        // The Panel wanted nothing after it: the frame's own request ends the nudge, and the 12 Hz spacing resumes.
        P.eq("nudge.ended_by_the_frame", b.cadence.wantedFullRate(), 0);
        double after = 0.0;
        for (const double t : b.drawn)
            if (t > vsyncAfter + 0.01 && after == 0.0)
                after = t;
        P.near("nudge.then_idle_again", after - vsyncAfter, 1000.0 / 12.0, 0.01);

        // A nudge right after a frame still waits for the full-rate spacing: never two frames in one vsync.
        FrameCadence c;
        c.advance(1000.0);
        c.requestRate(false);
        c.nudge();
        P.eq("nudge.not_before_the_next_vsync", c.due(1004.0, false), 0);
        P.eq("nudge.due_a_vsync_later", c.due(1000.0 + 1000.0 / 60.0, false), 1);
        P.near("nudge.no_wait", c.waitMs(1002.0), 0.0, 0.0);
    }

    // ---- hidden -----------------------------------------------------------------------------------------------------
    {
        Browser b(60.0);
        b.hidden = true;
        b.panelWantsFullRate = true;
        b.run(3.0, 1000.0);
        P.eq("hidden.nothing_drawn", static_cast<int64_t>(b.drawn.size()), 0);
        FrameCadence c;
        c.advance(1000.0);
        c.nudge();
        P.eq("hidden.not_due_even_nudged", c.due(5000.0, true), 0);
        P.eq("hidden.due_when_shown_again", c.due(5000.0, false), 1);
        P.eq("hidden.dt_after_the_gap_is_100ms", sameBits(c.advance(5000.0), 0.1f), 1);
    }

    // ---- wait and rate ----------------------------------------------------------------------------------------------
    {
        FrameCadence c;
        P.near("wait.none_before_the_first_frame", c.waitMs(50.0), 0.0, 0.0);
        P.eq("rate.unset_is_not_full", c.wantedFullRate(), 0);
        P.eq("rate.first_frame_is_due", c.due(50.0, false), 1);
        c.advance(1000.0);
        c.requestRate(false);
        P.near("wait.idle_after_a_frame", c.waitMs(1000.0), 1000.0 / 12.0 - 1000.0 / 60.0, 1.0e-9);
        P.near("wait.idle_later", c.waitMs(1030.0), 1000.0 / 12.0 - 1000.0 / 60.0 - 30.0, 1.0e-9);
        P.near("wait.idle_overdue_is_zero", c.waitMs(1200.0), 0.0, 0.0);
        P.eq("rate.idle_not_due_at_60hz", c.due(1000.0 + 4.0 * 1000.0 / 60.0, false), 0);
        P.eq("rate.idle_due_a_twelfth_later", c.due(1000.0 + 5.0 * 1000.0 / 60.0, false), 1);
        c.requestRate(true);
        P.near("wait.none_at_full_rate", c.waitMs(1000.0), 0.0, 0.0);
        P.eq("rate.full_wanted", c.wantedFullRate(), 1);
        P.eq("rate.full_not_due_at_120hz", c.due(1000.0 + 1000.0 / 120.0, false), 0);
        P.eq("rate.full_due_at_60hz", c.due(1000.0 + 1000.0 / 60.0, false), 1);
        c.requestRate(false);
        c.nudge();
        P.eq("rate.nudge_is_full", c.wantedFullRate(), 1);
        c.requestRate(false);
        P.eq("rate.request_ends_the_nudge", c.wantedFullRate(), 0);
    }

    // ---- zoom -------------------------------------------------------------------------------------------------------
    {
        const std::vector<int> raw{ 150, 100, 100, 500, 20, 125 };
        const std::vector<int> cleaned = web::cleanZoomSteps(raw);
        P.eq("zoom.steps_cleaned", cleaned == std::vector<int>{ 100, 125, 150 }, 1);
        P.eq("zoom.steps_bounds_kept", web::cleanZoomSteps(std::vector<int>{ 400, 25, 24, 401 })
                                           == std::vector<int>{ 25, 400 }, 1);
        P.eq("zoom.steps_none", web::cleanZoomSteps(std::vector<int>{}).empty(), 1);
        P.eq("zoom.default_nearest", web::nearestZoomStep(cleaned, 130), 125);
        P.eq("zoom.default_listed", web::nearestZoomStep(cleaned, 150), 150);
        const std::vector<int> tie{ 100, 120 };
        P.eq("zoom.default_tie_smaller", web::nearestZoomStep(tie, 110), 100);
        P.eq("zoom.default_without_steps", web::nearestZoomStep(std::vector<int>{}, 150), 100);

        P.eq("zoom.size_100", web::zoomedSize(641, 100), 641);
        P.eq("zoom.size_125", web::zoomedSize(240, 125), 300);
        P.eq("zoom.size_175", web::zoomedSize(160, 175), 280);
        P.eq("zoom.size_half_rounds_up", web::zoomedSize(1050, 125), 1313);        // 1312.5
        P.eq("zoom.size_rounds_down", web::zoomedSize(241, 125), 301);              // 301.25
        P.eq("zoom.size_rounds_up", web::zoomedSize(3, 125), 4);                    // 3.75

        const std::vector<int> steps{ 100, 125, 150, 175 };
        const int areaW = 1512, areaH = 944;                                        // a 14" MacBook's user area
        // 125 % fits the width (0.89 of it), 150 % does not (1.07): fg.editorhost.zoom's fit.open, fit.set_150 ...
        const int w = static_cast<int>(std::floor(areaW / 1.4)), h = 100;
        P.eq("zoom.fit.width_bound", web::fitZoom(steps, 175, w, h, areaW, areaH), 125);
        P.eq("zoom.fit.chosen_150", web::fitZoom(steps, 150, w, h, areaW, areaH), 125);
        P.eq("zoom.fit.below_the_fit_as_chosen", web::fitZoom(steps, 100, w, h, areaW, areaH), 100);
        // 150 % fits the height (0.94 of it), 175 % does not (1.09): fit.height.
        const int h2 = static_cast<int>(std::floor(areaH / 1.6));
        P.eq("zoom.fit.height_bound", web::fitZoom(steps, 175, 200, h2, areaW, areaH), 150);
        P.eq("zoom.fit.none_fits_is_smallest", web::fitZoom(steps, 175, areaW + 10, 100, areaW, areaH), 100);
        P.eq("zoom.fit.exactly_fits", web::fitZoom(steps, 175, 800, 400, 1400, 700), 175);
        P.eq("zoom.fit.one_px_short", web::fitZoom(steps, 175, 800, 400, 1399, 700), 150);
        P.eq("zoom.fit.unknown_area_as_chosen", web::fitZoom(steps, 175, 4000, 4000, 0, 0), 175);
        P.eq("zoom.fit.no_height_as_chosen", web::fitZoom(steps, 150, 4000, 4000, 1000, -5), 150);
        P.eq("zoom.fit.no_steps_as_chosen", web::fitZoom(std::vector<int>{}, 100, 4000, 4000, 100, 100), 100);
    }

    return P.finish();
}

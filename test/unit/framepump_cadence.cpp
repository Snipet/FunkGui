// FUNKGUI_TEST name=fg.framepump.cadence timeout=60 gpu=0 links=harness
//
// fg.framepump.cadence (0.25.0; gpu/FramePump.h "Each editor its own cadence", src/gpu/ClientCadence.h): one editor's
// cadence in the FramePump, driven with made-up ticks (the rules are plain C++, so this runs in every configuration).
// fg.editorhost.cadence runs them in the FramePump with several editors, fg.editorhost.cadence.live on real windows.
//   start.*     a new editor runs at its first tick, also while the pump is at full rate; a calm first frame leaves
//               it calm
//   idle.*      the pump idling (its ticks are the idle cadence) or on its pinned clock (a capture): a calm editor
//               runs at every tick, its dt the tick's; ticks a twelfth of a second apart run it at every tick at full
//               rate too
//   calm60.*    the pump at 60 Hz for another editor: a calm editor runs at every fifth tick, its dt 5/60 s, 12 fps
//   calm120.*   at 120 Hz: every tenth tick; calm72.*: at 72 Hz, every sixth; timer16.*: a 16 ms timer, every fifth
//   jitter.*    60 Hz ticks each up to 3 ms early or late: the tick nearest a twelfth of a second, about 12 Hz
//   full.*      an editor that asks for full rate runs at every tick; calm again, it waits for its twelfth of a second
//   nudge.*     a nudge runs it at the next tick and asks for full rate until that frame's own request; then the
//               hold runs it (hold.*), then its own cadence; two nudges are one; a nudge during its own frame holds
//               for one more tick, and the hold follows
//   hold.*      (v0.25.1) a nudge holds the editor at full rate for kHoldSec (0.25 s) of the pump's clock from the
//               frame that takes it: it runs at every tick whose dts since that frame sum to less than a quarter of a
//               second (at 60 Hz the nudged frame and the 14 ticks after it; 120 Hz: 29; 72 Hz: 17; a 16 ms timer: 15),
//               then waits for its own twelfth of a second; held() says so, fullRate() (the request) does not; from
//               the frame after an idle tick too; each nudge starts it again (nudges every fifth tick: every tick
//               runs, and the hold ends a quarter of a second after the last); a nudge during its own frame holds
//               from that frame; ticks a twelfth of a second apart end it after the same quarter of a second
//   request.*   fullRate() during a frame: the request before it (a nudge between frames included), or a nudge since
//   dt.*        clamped to 1 ms .. 100 ms
//   fps.*       0 until its frames span a second; then its own rate
// Spec rows only.

#include "../../src/gpu/ClientCadence.h"

#include <funkgui/test/Harness.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace T = funkgui::test;
using funkgui::detail::ClientCadence;

namespace
{
    // The pump's dt as it hands it on: a float.
    double pumpDt(double seconds) { return static_cast<double>(static_cast<float>(seconds)); }

    struct Runs
    {
        std::vector<int>   ticks;                    // the ticks the editor ran at
        std::vector<float> dts;                      // the dt each frame got
    };

    // `n` ticks of the pump, `dtAt(i)` seconds apart, the pump's clock at `now`; a frame run at tick i asks for
    // `wants(i)`.
    Runs drive(ClientCadence& c, int n, const std::function<double(int)>& dtAt, bool everyTick,
               const std::function<bool(int)>& wants, double& now)
    {
        Runs r;
        for (int i = 0; i < n; ++i)
        {
            const double dt = dtAt(i);
            now += dt;
            if (!c.tick(dt, everyTick))
                continue;
            r.ticks.push_back(i);
            r.dts.push_back(c.begin(now));
            c.end(wants(i));
        }
        return r;
    }

    bool never(int) { return false; }

    // How many runs, from the first, are at consecutive ticks starting at tick 0.
    int leadingRun(const Runs& r)
    {
        int n = 0;
        while (n < static_cast<int>(r.ticks.size()) && r.ticks[static_cast<std::size_t>(n)] == n)
            ++n;
        return n;
    }

    // The gap between the run after the leading run and the leading run's last; -1 when there is none.
    int gapAfter(const Runs& r, int lead)
    {
        if (lead < 1 || lead >= static_cast<int>(r.ticks.size()))
            return -1;
        return r.ticks[static_cast<std::size_t>(lead)] - r.ticks[static_cast<std::size_t>(lead - 1)];
    }

    // Gaps between consecutive runs other than `gap`.
    int64_t otherGaps(const Runs& r, int gap)
    {
        int64_t n = 0;
        for (std::size_t i = 1; i < r.ticks.size(); ++i)
            n += (r.ticks[i] - r.ticks[i - 1]) != gap ? 1 : 0;
        return n;
    }

    double maxDtError(const Runs& r, double want)
    {
        double e = 0.0;
        for (float d : r.dts)
            e = std::max(e, std::fabs(static_cast<double>(d) - want));
        return e;
    }

    // A started, calm cadence: one frame run and asked for nothing.
    ClientCadence calmCadence(double& now)
    {
        ClientCadence c;
        now += pumpDt(1.0 / 60.0);
        c.tick(pumpDt(1.0 / 60.0), false);
        c.begin(now);
        c.end(false);
        return c;
    }

    // A calm editor beside a pump at full rate: ten seconds of ticks `tickSec` apart run it at every `gap`th tick, with
    // dt gap * tickSec, at kIdleHz.
    void rateRows(T::Probe& P, const std::string& prefix, double tickSec, int gap)
    {
        double now = 0.0;
        ClientCadence c = calmCadence(now);
        const int n = static_cast<int>(std::lround(10.0 / tickSec));
        const Runs r = drive(c, n, [&](int) { return pumpDt(tickSec); }, false, never, now);
        P.eq(prefix + ".runs", static_cast<int64_t>(r.ticks.size()), n / gap);
        P.eq(prefix + ".first_at", r.ticks.empty() ? -1 : r.ticks.front(), gap - 1);
        P.eq(prefix + ".other_gaps", otherGaps(r, gap), 0);
        P.le(prefix + ".dt_error", maxDtError(r, gap * pumpDt(tickSec)), 1e-6);
        P.near(prefix + ".fps", c.fps(), 1.0 / (gap * tickSec), 0.05);
        P.eq(prefix + ".calm", c.fullRate(), 0);
    }

    // v0.25.1: a calm editor nudged between frames, then ticks `tickSec` apart at full rate: the nudged frame at the
    // first tick and `held` ticks after it (the ticks less than kHoldSec after the nudged frame), then its own cadence,
    // `gap` ticks after its last held frame. The hold is held() and never fullRate().
    void holdRows(T::Probe& P, const std::string& prefix, double tickSec, int held, int gap)
    {
        double now = 0.0;
        ClientCadence c = calmCadence(now);
        c.nudge();
        const double dt = pumpDt(tickSec);
        Runs r;
        int lastHeld = -1, requests = 0;
        for (int i = 0; i < 3 * (held + gap); ++i)
        {
            now += dt;
            if (c.tick(dt, false))
            {
                r.ticks.push_back(i);
                c.begin(now);
                c.end(false);
            }
            lastHeld = c.held() ? i : lastHeld;
            requests += i > 0 && c.fullRate() ? 1 : 0;
        }
        const int lead = leadingRun(r);
        P.eq(prefix + ".held_frames", lead, held + 1);
        P.eq(prefix + ".last_held_tick", lastHeld, held);
        // The rule, not only the count: the last held frame is less than kHoldSec after the nudged one, the next tick
        // would have been kHoldSec or more.
        P.eq(prefix + ".span_under_hold", held * dt < ClientCadence::kHoldSec ? 1 : 0, 1);
        P.ge(prefix + ".next_tick_past_hold", (held + 1) * dt, ClientCadence::kHoldSec);
        P.eq(prefix + ".then_calm", gapAfter(r, lead), gap);
        P.eq(prefix + ".not_a_request", requests, 0);
    }
}

int main(int argc, char** argv)
{
    T::Probe P("fg.framepump.cadence", "", argc, argv);
    const double t60 = pumpDt(1.0 / 60.0);

    // ---- start ------------------------------------------------------------------------------------------------------
    {
        ClientCadence c;
        P.eq("start.not_started", c.started(), 0);
        P.eq("start.calm_before", c.fullRate(), 0);
        P.eq("start.runs_at_full_rate", c.tick(t60, false), 1);
        P.near("start.dt", c.begin(1.0), t60, 1e-7);
        c.end(false);
        P.eq("start.started", c.started(), 1);
        P.eq("start.calm_after", c.fullRate(), 0);
        P.eq("start.next_tick_waits", c.tick(t60, false), 0);
    }

    // ---- idle -------------------------------------------------------------------------------------------------------
    {
        double now = 0.0;
        ClientCadence c = calmCadence(now);
        const Runs idle = drive(c, 120, [](int) { return pumpDt(1.0 / 12.0); }, true, never, now);
        P.eq("idle.every_tick", static_cast<int64_t>(idle.ticks.size()), 120);
        P.le("idle.dt_error", maxDtError(idle, pumpDt(1.0 / 12.0)), 1e-6);
        const Runs pinned = drive(c, 600, [&](int) { return t60; }, true, never, now);
        P.eq("idle.pinned_every_tick", static_cast<int64_t>(pinned.ticks.size()), 600);
        P.le("idle.pinned_dt_error", maxDtError(pinned, t60), 1e-6);
        // At full rate, ticks that come a twelfth of a second apart (the first after a rate change) run it each time.
        const Runs slow = drive(c, 24, [](int) { return pumpDt(1.0 / 12.0); }, false, never, now);
        P.eq("idle.slow_ticks_every_tick", static_cast<int64_t>(slow.ticks.size()), 24);
    }

    // ---- calm at full rate ------------------------------------------------------------------------------------------
    rateRows(P, "calm60", 1.0 / 60.0, 5);
    rateRows(P, "calm120", 1.0 / 120.0, 10);
    rateRows(P, "calm72", 1.0 / 72.0, 6);
    rateRows(P, "timer16", 0.016, 5);
    {
        double now = 0.0;
        ClientCadence c = calmCadence(now);
        uint32_t seed = 12345u;
        const Runs r = drive(c, 600, [&](int)
        {
            seed = seed * 1664525u + 1013904223u;
            const double jitter = (static_cast<double>(seed >> 8) / 16777216.0 - 0.5) * 0.006;   // -3 .. +3 ms
            return pumpDt(1.0 / 60.0 + jitter);
        }, false, never, now);
        int minGap = 1000, maxGap = 0;
        for (std::size_t i = 1; i < r.ticks.size(); ++i)
        {
            minGap = std::min(minGap, r.ticks[i] - r.ticks[i - 1]);
            maxGap = std::max(maxGap, r.ticks[i] - r.ticks[i - 1]);
        }
        P.in("jitter.min_gap", minGap, 4, 5);
        P.in("jitter.max_gap", maxGap, 5, 6);
        P.in("jitter.fps", c.fps(), 11.0, 13.0);
    }

    // ---- full -------------------------------------------------------------------------------------------------------
    {
        double now = 0.0;
        ClientCadence c;                             // new: runs at tick 0, which asks for full rate
        const Runs r = drive(c, 120, [&](int) { return t60; }, false, [](int i) { return i < 60; }, now);
        int consecutive = 0;
        while (consecutive < static_cast<int>(r.ticks.size())
               && r.ticks[static_cast<std::size_t>(consecutive)] == consecutive)
            ++consecutive;
        P.eq("full.every_tick_while_asked", consecutive, 61);   // ticks 0-59 asked; tick 60 runs on that request
        P.eq("full.calm_waits", r.ticks.size() > 61 ? r.ticks[61] - r.ticks[60] : -1, 5);
        Runs calmPart;
        if (r.ticks.size() > 60)
            calmPart.ticks.assign(r.ticks.begin() + 60, r.ticks.end());
        P.eq("full.then_fifth_ticks", otherGaps(calmPart, 5), 0);
        P.eq("full.then_runs", static_cast<int64_t>(calmPart.ticks.size()), 12);   // ticks 60, 65 .. 115
    }

    // ---- nudge ------------------------------------------------------------------------------------------------------
    {
        double now = 0.0;
        ClientCadence c = calmCadence(now);
        now += t60;
        P.eq("nudge.calm_waits", c.tick(t60, false), 0);
        c.nudge();
        P.eq("nudge.asks_full_rate", c.fullRate(), 1);
        now += t60;
        P.eq("nudge.runs_next_tick", c.tick(t60, false), 1);
        P.near("nudge.dt_since_last_frame", c.begin(now), 2.0 * t60, 1e-6);
        c.end(false);
        P.eq("nudge.ends_with_own_request", c.fullRate(), 0);
        P.eq("nudge.then_held", c.held(), 1);                     // v0.25.1 (hold.*)
        const Runs after = drive(c, 40, [&](int) { return t60; }, false, never, now);
        const int lead = leadingRun(after);
        P.eq("nudge.held_ticks", lead, 14);                      // before 0.25.1: none, calm at once
        P.eq("nudge.then_calm", gapAfter(after, lead), 5);
    }
    {
        // Two nudges before a tick are one: the same frames as one nudge.
        double nowOnce = 0.0, nowTwice = 0.0;
        ClientCadence once = calmCadence(nowOnce), twice = calmCadence(nowTwice);
        once.nudge();
        twice.nudge();
        twice.nudge();
        const Runs r1 = drive(once, 40, [&](int) { return t60; }, false, never, nowOnce);
        const Runs r2 = drive(twice, 40, [&](int) { return t60; }, false, never, nowTwice);
        P.eq("nudge.twice_at_next_tick", r2.ticks.empty() ? -1 : r2.ticks.front(), 0);
        P.eq("nudge.twice_as_once", r1.ticks == r2.ticks ? 1 : 0, 1);
    }
    {
        // A nudge during its own frame (a Panel's tick asking for a frame soon) holds for one more tick.
        double now = 0.0;
        ClientCadence c = calmCadence(now);
        do
            now += t60;
        while (!c.tick(t60, false));                 // its next own frame
        c.begin(now);
        c.nudge();
        P.eq("nudge.in_frame_asks", c.fullRate(), 1);
        c.end(false);
        P.eq("nudge.in_frame_holds", c.fullRate(), 1);
        now += t60;
        P.eq("nudge.in_frame_next_tick", c.tick(t60, false), 1);
        c.begin(now);
        c.end(false);
        P.eq("nudge.in_frame_then_calm", c.fullRate(), 0);
        now += t60;
        P.eq("nudge.in_frame_then_held", c.tick(t60, false), 1);  // v0.25.1: the hold; before, it waited
    }

    // ---- hold (v0.25.1) ---------------------------------------------------------------------------------------------
    P.near("hold.seconds", ClientCadence::kHoldSec, 0.25, 0.0);
    holdRows(P, "hold.60hz", 1.0 / 60.0, 14, 5);
    holdRows(P, "hold.120hz", 1.0 / 120.0, 29, 10);
    holdRows(P, "hold.72hz", 1.0 / 72.0, 17, 6);
    holdRows(P, "hold.timer16", 0.016, 15, 5);
    {
        // The nudge reaches an idling pump: its next tick comes a twelfth of a second later (and runs every editor),
        // then the ticks come at 60 Hz. The hold runs from the nudged frame: it and 14 ticks after it.
        double now = 0.0;
        ClientCadence c = calmCadence(now);
        c.nudge();
        now += pumpDt(1.0 / 12.0);
        P.eq("hold.after_idle_tick_runs", c.tick(pumpDt(1.0 / 12.0), true), 1);
        c.begin(now);
        c.end(false);
        const Runs r = drive(c, 40, [&](int) { return t60; }, false, never, now);
        const int lead = leadingRun(r);
        P.eq("hold.after_idle_tick_held", lead, 14);
        P.eq("hold.after_idle_tick_then_calm", gapAfter(r, lead), 5);
    }
    {
        // Nudged before every fifth tick for a second, as pointer moves at 12 Hz: it runs at every tick, held, and
        // the hold ends a quarter of a second after the frame of the last nudge (tick 55): ticks 0 .. 69.
        double now = 0.0;
        ClientCadence c = calmCadence(now);
        Runs r;
        int heldTicks = 0;
        for (int i = 0; i < 100; ++i)
        {
            if (i < 60 && i % 5 == 0)
                c.nudge();
            now += t60;
            if (c.tick(t60, false))
            {
                r.ticks.push_back(i);
                c.begin(now);
                c.end(false);
            }
            heldTicks += c.held() ? 1 : 0;
        }
        const int lead = leadingRun(r);
        P.eq("hold.refreshed_every_tick", lead, 70);
        P.eq("hold.refreshed_held_ticks", heldTicks, 70);
        P.eq("hold.refreshed_then_calm", gapAfter(r, lead), 5);
    }
    {
        // A nudge during its own frame: the hold runs from that frame.
        double now = 0.0;
        ClientCadence c = calmCadence(now);
        do
            now += t60;
        while (!c.tick(t60, false));
        c.begin(now);
        c.nudge();
        c.end(false);
        P.eq("hold.in_frame_held", c.held(), 1);
        const Runs r = drive(c, 40, [&](int) { return t60; }, false, never, now);
        const int lead = leadingRun(r);
        P.eq("hold.in_frame_ticks", lead, 14);
        P.eq("hold.in_frame_then_calm", gapAfter(r, lead), 5);
    }
    {
        // Ticks a twelfth of a second apart (at full rate, the first after a rate change): the hold ends after the
        // same quarter of a second, two ticks after the nudged frame.
        double now = 0.0;
        ClientCadence c = calmCadence(now);
        c.nudge();
        const double slow = pumpDt(1.0 / 12.0);
        int heldTicks = 0;
        for (int i = 0; i < 6; ++i)
        {
            now += slow;
            if (c.tick(slow, false))
            {
                c.begin(now);
                c.end(false);
            }
            heldTicks += c.held() ? 1 : 0;
        }
        P.eq("hold.slow_ticks_held", heldTicks, 3);              // the nudged frame's tick and the two after it
    }

    // ---- request ----------------------------------------------------------------------------------------------------
    {
        double now = 0.0;
        ClientCadence c = calmCadence(now);
        c.nudge();
        now += t60;
        c.tick(t60, false);
        c.begin(now);
        P.eq("request.nudged_before_frame", c.fullRate(), 1);
        c.end(false);
        P.eq("request.after_calm_frame", c.fullRate(), 0);

        now += t60;
        c.tick(t60, true);
        c.begin(now);
        P.eq("request.calm_before_frame", c.fullRate(), 0);
        c.end(true);
        P.eq("request.after_full_frame", c.fullRate(), 1);
        now += t60;
        c.tick(t60, false);
        c.begin(now);
        P.eq("request.full_before_frame", c.fullRate(), 1);
        c.end(false);
        P.eq("request.after_full_then_calm", c.fullRate(), 0);
    }

    // ---- dt ---------------------------------------------------------------------------------------------------------
    {
        double now = 0.0;
        ClientCadence c = calmCadence(now);
        P.eq("dt.tiny_runs", c.tick(0.0002, true), 1);
        P.near("dt.clamped_low", c.begin(now), 0.001, 1e-9);
        c.end(false);
        for (int i = 0; i < 7; ++i)                  // 70 ms: not yet due at full rate
            P.eq("dt.waits_" + std::to_string(i), c.tick(0.01, false), 0);
        P.eq("dt.gap_runs", c.tick(0.1, false), 1);  // 170 ms owed
        P.near("dt.clamped_high", c.begin(now), 0.1, 1e-7);
        c.end(false);
    }

    // ---- fps --------------------------------------------------------------------------------------------------------
    {
        double now = 0.0;
        ClientCadence calm = calmCadence(now);
        drive(calm, 59, [&](int) { return t60; }, false, never, now);
        P.eq("fps.zero_before_a_second", calm.fps() == 0.0f ? 1 : 0, 1);
        drive(calm, 600, [&](int) { return t60; }, false, never, now);
        P.near("fps.calm", calm.fps(), 12.0, 0.05);

        double now2 = 0.0;
        ClientCadence busy;
        drive(busy, 600, [&](int) { return t60; }, false, [](int) { return true; }, now2);
        P.near("fps.busy", busy.fps(), 60.0, 0.05);
        P.eq("fps.busy_asks", busy.fullRate(), 1);
    }
    return P.finish();
}

// FUNKGUI_TEST name=fg.livefeed timeout=120 gpu=0
//
// fg.livefeed: LiveFeed<Frame> and LiveState (live/LiveFeed.h; 02 §5.10, §6.5, §9.1; HR BgfxEditor.cpp:966-980) against
// a scripted seqlock reader: live only after a new publishCount, stale 0.5 s after the count stops moving (or reads keep
// failing), fresh on the poll that saw a new count, count 0 never live, a wrapped counter still new, a torn read never
// reaching frame(), and time only from dt. Exact binary time steps (1/8 s), so every age is exact. Spec rows only.

#include <funkgui/core/Ease.h>
#include <funkgui/live/LiveFeed.h>
#include <funkgui/test/Harness.h>

#include <cstdint>
#include <limits>

namespace T = funkgui::test;
namespace ease = funkgui::ease;

namespace
{
    constexpr float kDt = 0.125f;                        // exact: 4 polls = 0.5 s

    struct Frame                                         // the shape of fcdsp::UiFrame that LiveFeed needs
    {
        uint32_t publishCount = 0;
        float    grDb = 0.0f;
    };

    // The seqlock reader: `ok` false is a torn read, which (like a real reader) scribbles over the frame it was given.
    struct Reader
    {
        bool     ok = true;
        uint32_t count = 0;
        float    grDb = 0.0f;
        int      calls = 0;

        bool operator()(Frame& f)
        {
            ++calls;
            f.publishCount = ok ? count : 0xDEADBEEFu;
            f.grDb = ok ? grDb : -999.0f;
            return ok;
        }
    };
}

int main(int argc, char** argv)
{
    T::Probe P("fg.livefeed", "", argc, argv);
    const float nan = std::numeric_limits<float>::quiet_NaN();

    // ---- A fresh feed, then the first publish ----------------------------------------------------------------------
    {
        funkgui::LiveFeed<Frame> feed;
        Reader r;
        P.eq("feed.initial", !feed.live() && !feed.fresh() && feed.staleSeconds() == 0.0f
                                 && feed.frame().publishCount == 0 && ease::sameBits(feed.state().staleAfter(), 0.5f), 1);

        r.count = 0;                                     // attached, nothing published yet
        feed.poll(r, kDt);
        P.eq("feed.count0_never_live", !feed.live() && !feed.fresh() && ease::sameBits(feed.staleSeconds(), kDt), 1);
        P.eq("feed.one_read_per_poll", r.calls, 1);

        r.count = 1;
        r.grDb = 3.5f;
        feed.poll(r, kDt);
        P.eq("feed.new_count_is_live", feed.live() && feed.fresh() && feed.staleSeconds() == 0.0f, 1);
        P.eq("feed.frame_copied", feed.frame().publishCount == 1 && ease::sameBits(feed.frame().grDb, 3.5f), 1);

        // The count stops: fresh drops at once, live lasts until the age reaches 0.5 s.
        r.grDb = 4.0f;
        feed.poll(r, kDt);
        P.eq("feed.same_count_not_fresh", !feed.fresh() && feed.live(), 1);
        P.eq("feed.same_count_frame_copied", ease::sameBits(feed.frame().grDb, 4.0f), 1);   // HR copied it
        feed.poll(r, kDt);
        feed.poll(r, kDt);
        P.eq("feed.live_below_stale_after", feed.live() && ease::sameBits(feed.staleSeconds(), 0.375f), 1);
        feed.poll(r, kDt);
        P.eq("feed.stale_at_0.5", !feed.live() && ease::sameBits(feed.staleSeconds(), 0.5f), 1);
        feed.poll(r, kDt);
        P.eq("feed.age_keeps_growing", ease::sameBits(feed.staleSeconds(), 0.625f), 1);

        // It resumes.
        r.count = 2;
        feed.poll(r, kDt);
        P.eq("feed.resumes", feed.live() && feed.fresh() && feed.staleSeconds() == 0.0f, 1);

        // Torn reads: the frame is never touched, and they age the stream like a stopped count.
        r.ok = false;
        feed.poll(r, kDt);
        P.eq("feed.torn_read_keeps_frame", feed.frame().publishCount == 2 && ease::sameBits(feed.frame().grDb, 4.0f), 1);
        P.eq("feed.torn_read_ages", !feed.fresh() && ease::sameBits(feed.staleSeconds(), kDt), 1);
        for (int i = 0; i < 3; ++i)
            feed.poll(r, kDt);
        P.eq("feed.torn_reads_go_stale", feed.live(), 0);
        r.ok = true;
        feed.poll(r, kDt);
        P.eq("feed.same_count_after_torn_not_new", !feed.fresh() && !feed.live(), 1);   // count 2 again: still old
        r.count = 3;
        feed.poll(r, kDt);
        P.eq("feed.new_count_after_torn", feed.live() && feed.fresh(), 1);
    }

    // ---- The counter wraps; time only from dt ----------------------------------------------------------------------
    {
        funkgui::LiveFeed<Frame> feed;
        Reader r;
        r.count = 0xFFFFFFFFu;
        feed.poll(r, kDt);
        r.count = 0u;                                    // wrapped: different, so new, even though it is 0
        feed.poll(r, kDt);
        P.eq("feed.wrapped_count_is_new", feed.fresh() && feed.live(), 1);
        feed.poll(r, 0.0f);
        feed.poll(r, -1.0f);
        feed.poll(r, nan);
        P.eq("feed.nonpositive_or_nan_dt_no_age", feed.staleSeconds() == 0.0f && feed.live(), 1);
    }

    // ---- LiveState alone, and a custom staleAfter -------------------------------------------------------------------
    {
        funkgui::LiveState s(0.25f);
        P.eq("state.initial", !s.live() && !s.fresh() && s.staleSeconds() == 0.0f, 1);
        s.observe(true, 7, kDt);
        P.eq("state.new_count", s.live() && s.fresh(), 1);
        s.observe(true, 7, kDt);
        P.eq("state.below_custom", s.live(), 1);
        s.observe(false, 8, kDt);                        // a failed read's count is ignored
        P.eq("state.failed_read_ignores_count", !s.live() && !s.fresh() && ease::sameBits(s.staleSeconds(), 0.25f), 1);
        s.observe(true, 8, kDt);
        P.eq("state.recovers", s.live() && s.fresh(), 1);
    }

    return P.finish();
}

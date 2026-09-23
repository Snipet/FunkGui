// FUNKGUI_TEST name=fg.dwell timeout=120 gpu=0
//
// fg.dwell: the fixed-dt state machines a Panel switches and fades with (02 §3.7 rules 1–2, §5.7, §5.8, §7.1):
// DwellSelector (pin / hold / touch, the dwell, the crossfade, reversal, cuts), ScreenFader (tau 0.12 s), and HintLine
// (the first-run countdown, the first move's cut to 0.4 s, skip, always-chrome, and a draw whose geometry does not move
// while the hint fades). Every run uses exact binary time steps where a count matters, so the rows are exact. Spec rows
// only.

#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/Fingerprint.h>
#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Ease.h>
#include <funkgui/core/Theme.h>
#include <funkgui/test/Harness.h>
#include <funkgui/text/FontService.h>
#include <funkgui/widgets/DwellSelector.h>
#include <funkgui/widgets/HintLine.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace T = funkgui::test;
namespace ease = funkgui::ease;
using funkgui::DwellSelector;
using funkgui::HintLine;

namespace
{
    constexpr float kDt60 = 1.0f / 60.0f;
    constexpr float kDt16 = 1.0f / 16.0f;                // exact in binary: dwell counts are exact

    // Ticks until settled(), or -1 after maxTicks; records amount() after every tick.
    template <class V>
    int settle(DwellSelector<V>& d, float dt, int maxTicks, std::vector<float>* trace = nullptr)
    {
        for (int i = 1; i <= maxTicks; ++i)
        {
            d.tick(dt);
            if (trace != nullptr)
                trace->push_back(d.amount());
            if (d.settled())
                return i;
        }
        return -1;
    }

    bool rising(const std::vector<float>& v)
    {
        for (size_t i = 1; i < v.size(); ++i)
            if (v[i] < v[i - 1] || v[i] > 1.0f)
                return false;
        return true;
    }

    // A frame with the hint line drawn at (10, 10), 400 px wide.
    funkgui::PrimList drawHint(const HintLine& h, const char* spec, float maxW = 400.0f)
    {
        funkgui::Canvas c(funkgui::FontService::get().atlas());
        funkgui::FrameInfo info;
        info.logicalW = 480;
        info.logicalH = 40;
        info.dpi = 2.0f;
        c.begin(info);
        h.draw(c, funkgui::Theme::graphite(), 10.0f, 10.0f, maxW, spec);
        return c.end();
    }

    int tagged(const funkgui::PrimList& l, funkgui::Tag t)
    {
        int n = 0;
        for (const funkgui::Prim& p : l.prims)
            n += p.tag == t ? 1 : 0;
        return n;
    }
}

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;      // FontService bakes the atlas through JUCE's font stack
    T::Probe P("fg.dwell", "", argc, argv);
    const float nan = std::numeric_limits<float>::quiet_NaN();

    // ---- DwellSelector: rest and pin --------------------------------------------------------------------------------
    {
        DwellSelector<int> d(0);
        P.eq("dwell.initial", d.incoming() == 0 && d.outgoing() == 0 && ease::sameBits(d.amount(), 1.0f)
                                  && d.settled() && d.pinned() == 0 && !d.held() && d.dwellLeft() == 0.0f, 1);
        d.pin(2);
        P.eq("dwell.pin_targets_at_once", d.incoming() == 2 && d.outgoing() == 0 && d.amount() == 0.0f, 1);
        P.eq("dwell.pin_unsettles", d.settled(), 0);
        d.tick(kDt60);
        P.eq("dwell.pin_step_is_toward", ease::sameBits(d.amount(), ease::toward(0.0f, 1.0f, kDt60, 0.18f)), 1);
        std::vector<float> trace{ d.amount() };
        const int n = settle(d, kDt60, 1000, &trace);
        P.in("dwell.pin_lands_ticks_60hz", n, 1, 120);
        P.eq("dwell.pin_lands_exactly", ease::sameBits(d.amount(), 1.0f), 1);
        P.eq("dwell.pin_monotone", rising(trace), 1);
        d.pin(2);
        P.eq("dwell.pin_same_is_noop", ease::sameBits(d.amount(), 1.0f) && d.settled() && d.incoming() == 2, 1);
        d.pin(1, true);
        P.eq("dwell.pin_instant_cuts", d.incoming() == 1 && d.outgoing() == 1 && ease::sameBits(d.amount(), 1.0f)
                                           && d.settled(), 1);

        // The same run twice gives the same bits.
        DwellSelector<int> a(0), b(0);
        std::vector<float> ta, tb;
        a.pin(3);
        b.pin(3);
        settle(a, kDt60, 1000, &ta);
        settle(b, kDt60, 1000, &tb);
        P.eq("dwell.repeatable", ta.size() == tb.size() && T::hashFloats(ta) == T::hashFloats(tb), 1);
    }

    // ---- touch: the dwell ---------------------------------------------------------------------------------------------
    {
        DwellSelector<int> d(0, 0.18f, 0.5f);            // 0.5 s = 8 ticks of 1/16 s, exactly
        d.touch(3);
        P.eq("touch.targets_at_once", d.incoming() == 3 && d.outgoing() == 0 && d.amount() == 0.0f, 1);
        P.eq("touch.dwell_pending", ease::sameBits(d.dwellLeft(), 0.5f) && !d.settled(), 1);
        for (int i = 0; i < 7; ++i)
            d.tick(kDt16);
        P.eq("touch.holds_through_dwell", d.incoming() == 3 && ease::sameBits(d.dwellLeft(), kDt16), 1);
        d.tick(kDt16);
        P.eq("touch.returns_after_dwell", d.incoming() == 0 && d.outgoing() == 3 && d.dwellLeft() == 0.0f, 1);
        const int n = settle(d, kDt60, 1000);
        P.in("touch.settles_on_pinned", n, 1, 120);
        P.eq("touch.settled_state", d.incoming() == 0 && ease::sameBits(d.amount(), 1.0f), 1);

        d.touch(2);
        for (int i = 0; i < 4; ++i)
            d.tick(kDt16);
        d.touch(2);
        P.eq("touch.again_restarts_dwell", ease::sameBits(d.dwellLeft(), 0.5f) && d.incoming() == 2, 1);
        d.pin(1);
        P.eq("touch.pin_cancels_dwell", d.incoming() == 1 && d.dwellLeft() == 0.0f, 1);
    }

    // ---- hold: while dragging, then the dwell after release -----------------------------------------------------------
    {
        DwellSelector<int> d(0, 0.18f, 0.5f);
        d.touch(3);
        d.hold(2);
        P.eq("hold.targets_at_once", d.incoming() == 2 && d.held() == 2, 1);
        P.eq("hold.cancels_touch", d.dwellLeft() == 0.0f, 1);
        const int n = settle(d, kDt60, 1000);
        P.in("hold.settles_while_held", n, 1, 120);
        for (int i = 0; i < 100; ++i)
            d.tick(kDt16);
        P.eq("hold.lasts", d.incoming() == 2 && d.settled(), 1);
        d.touch(3);
        P.eq("hold.beats_touch", d.incoming(), 2);
        d.pin(1);
        P.eq("hold.beats_pin", d.incoming() == 2 && d.pinned() == 1, 1);
        d.hold(std::nullopt);
        P.eq("hold.release_dwells", d.incoming() == 2 && ease::sameBits(d.dwellLeft(), 0.5f) && !d.held(), 1);
        for (int i = 0; i < 8; ++i)
            d.tick(kDt16);
        P.eq("hold.release_then_pinned", d.incoming(), 1);
        d.hold(std::nullopt);
        P.eq("hold.release_without_hold_is_noop", d.incoming() == 1 && d.dwellLeft() == 0.0f, 1);
    }

    // ---- the crossfade: reversal and a third view ---------------------------------------------------------------------
    {
        DwellSelector<int> d(0);
        d.pin(1);
        for (int i = 0; i < 5; ++i)
            d.tick(kDt60);
        const float a = d.amount();
        d.pin(0);
        P.eq("fade.reverse_swaps", d.incoming() == 0 && d.outgoing() == 1, 1);
        P.eq("fade.reverse_continuous", ease::sameBits(d.amount(), 1.0f - a), 1);

        DwellSelector<int> e(0);
        e.pin(1);
        e.tick(kDt60);                                   // amount < 0.5: view 0 still dominates
        e.pin(2);
        P.eq("fade.third_from_dominant_out", e.incoming() == 2 && e.outgoing() == 0 && e.amount() == 0.0f, 1);
        e.pin(1);
        for (int i = 0; i < 60; ++i)
            e.tick(kDt60);                               // amount >= 0.5: view 1 dominates
        e.pin(2);
        P.eq("fade.third_from_dominant_in", e.incoming() == 2 && e.outgoing() == 1 && e.amount() == 0.0f, 1);
    }

    // ---- degenerate time ----------------------------------------------------------------------------------------------
    {
        DwellSelector<int> d(0, 0.18f, 0.5f);
        d.touch(1);
        d.tick(kDt60);
        const float a = d.amount(), left = d.dwellLeft();
        d.tick(0.0f);
        d.tick(-1.0f);
        d.tick(nan);
        P.eq("time.nonpositive_or_nan_dt_moves_nothing", ease::sameBits(d.amount(), a) && ease::sameBits(d.dwellLeft(), left), 1);
    }

    // ---- ScreenFader: tau 0.12 s ----------------------------------------------------------------------------------------
    {
        P.eq("fader.tau_bits", ease::sameBits(funkgui::kScreenFadeTau, 0.12f), 1);
        funkgui::ScreenFader f(0, funkgui::kScreenFadeTau);
        DwellSelector<int> slow(0);
        f.pin(1);
        slow.pin(1);
        f.tick(kDt60);
        P.eq("fader.step_is_toward", ease::sameBits(f.amount(), ease::toward(0.0f, 1.0f, kDt60, 0.12f)), 1);
        const int nf = settle(f, kDt60, 1000) + 1;
        const int ns = settle(slow, kDt60, 1000);
        P.in("fader.lands_ticks_60hz", nf, 1, 90);
        P.eq("fader.faster_than_default", nf < ns, 1);
        P.eq("fader.lands_on_screen", f.incoming() == 1 && ease::sameBits(f.amount(), 1.0f), 1);
    }

    // ---- HintLine -------------------------------------------------------------------------------------------------------
    {
        HintLine h("DRAG A VALUE. DOUBLE-CLICK TO RESET.");
        P.eq("hint.initial", h.active() && h.wantsFullRate() && ease::sameBits(h.remaining(), 6.0f)
                                 && ease::sameBits(h.alpha(), 1.0f) && !h.alwaysChrome(), 1);
        for (int i = 0; i < 5; ++i)
            h.tick(1.0f);
        P.eq("hint.counts_down_without_pointer", ease::sameBits(h.remaining(), 1.0f) && h.active(), 1);
        h.tick(0.0f);
        h.tick(-1.0f);
        h.tick(nan);
        P.eq("hint.nonpositive_or_nan_dt", ease::sameBits(h.remaining(), 1.0f), 1);
        h.tick(2.0f);
        P.eq("hint.expires", !h.active() && !h.wantsFullRate() && h.remaining() == 0.0f && h.alpha() == 0.0f, 1);

        HintLine m("HINT");
        m.pointerMoved();
        P.eq("hint.move_cuts_to_0.4", ease::sameBits(m.remaining(), HintLine::kCut) && ease::sameBits(m.alpha(), 1.0f), 1);
        m.tick(0.25f);
        P.near("hint.fades_over_last_0.4", m.alpha(), 0.375, 1e-6);
        m.pointerMoved();
        P.eq("hint.second_move_no_extend", ease::sameBits(m.remaining(), HintLine::kCut - 0.25f), 1);

        HintLine s("HINT");
        s.skip();
        P.eq("hint.skip", !s.active() && !s.wantsFullRate(), 1);
        P.eq("hint.zero_seconds_inactive", HintLine("HINT", 0.0f).active(), 0);

        HintLine c("HINT");
        c.noteDown();
        P.eq("hint.down_without_move_is_always_chrome", c.alwaysChrome(), 1);
        c.noteMove();
        P.eq("hint.move_after_down_ends_it", c.alwaysChrome(), 0);
        HintLine c2("HINT");
        c2.pointerMoved();
        c2.noteDown();
        P.eq("hint.pointer_moved_notes_move", c2.alwaysChrome(), 0);
        HintLine c3("HINT");
        c3.noteMove();
        P.eq("hint.note_move_keeps_hint", ease::sameBits(c3.remaining(), 6.0f), 1);
    }

    // HintLine::draw: the hint while active (its fade moves colour only), the fitted spec line after.
    {
        auto& fonts = funkgui::FontService::get();
        fonts.atlas();
        if (!P.eq("hint.font_ok", fonts.ok(), 1))
            return P.finish();
        const char* spec = "RATIO   STEPS 2 · 4 · 10   DRAG / WHEEL / ARROWS STEP   CLICK A STEP   DBL-CLICK RESET";
        HintLine h("DRAG A VALUE OR THE CURVE. DOUBLE-CLICK TO RESET.");
        const funkgui::PrimList full = drawHint(h, spec);
        h.pointerMoved();
        h.tick(0.3f);                                    // alpha 0.25
        const funkgui::PrimList faded = drawHint(h, spec);
        const auto ff = funkgui::fingerprint(full), fd = funkgui::fingerprint(faded);
        P.eq("hint.draw_tagged", tagged(full, funkgui::tags::hint) == static_cast<int>(full.prims.size())
                                     && !full.prims.empty(), 1);
        P.eq("hint.draw_fade_moves_colour_only", ff.text == fd.text && ff.texts == fd.texts
                                                     && full.prims.front().c0 != faded.prims.front().c0, 1);
        h.tick(1.0f);
        const funkgui::PrimList line = drawHint(h, spec, 200.0f);
        float right = 0.0f;
        for (const funkgui::Prim& p : line.prims)
            right = p.x1 > right ? p.x1 : right;
        P.eq("hint.spec_after_expiry", !line.prims.empty() && fingerprint(line).text != ff.text, 1);
        P.le("hint.spec_fitted_right_px", right, 10.0f + 200.0f + 6.0f);   // glyph quads carry the SDF spread
        P.eq("hint.spec_null_draws_nothing", static_cast<int64_t>(drawHint(h, nullptr).prims.size()), 0);
        P.eq("hint.spec_empty_draws_nothing", static_cast<int64_t>(drawHint(h, "").prims.size()), 0);
    }

    return P.finish();
}

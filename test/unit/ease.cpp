// FUNKGUI_TEST name=fg.ease timeout=120 gpu=0 links=harness
//
// fg.ease: the motion and blend utilities a Panel animates with (02 §3.7 rule 2, §5.7, §2.2). ease::toward / hover /
// shown / sameBits (core/Ease.h): HR's one-pole step bit for bit, no overshoot, the snap that makes a fixed-dt run land
// exactly on its target in a finite number of ticks, the same run twice giving the same bits, and every degenerate
// input defined. Col::fade / premix (core/Col.h), which crossfades and bead-free curves use, and the theme's `signal`
// token (renamed from HR's `ice`, 02 §2.3) with HR's values unchanged. Header-only: links FunkGui::harness alone.
// v0.10.0: the animation speed (ease::setTimeScale): 0 lands in one step, 2 takes twice the ticks, 1 is bit for bit the
// unscaled run, hover / shown / DwellSelector follow it, and anything outside 0 … 8 is 1.

#include <funkgui/core/Col.h>
#include <funkgui/core/Ease.h>
#include <funkgui/core/Theme.h>
#include <funkgui/test/Harness.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace T = funkgui::test;
namespace ease = funkgui::ease;
using funkgui::Col;

namespace
{
    constexpr float kDt60 = 1.0f / 60.0f;

    // Eases x towards target until it lands, recording every value; the tick count, or -1 after maxTicks.
    template <class Step>
    int run(float x, float target, int maxTicks, std::vector<float>& trace, Step step)
    {
        trace.clear();
        for (int i = 1; i <= maxTicks; ++i)
        {
            x = step(x);
            trace.push_back(x);
            if (ease::sameBits(x, target))
                return i;
        }
        return -1;
    }

    // Every step moved towards the target and never past it.
    bool monotoneNoOvershoot(float start, float target, const std::vector<float>& trace)
    {
        float prev = start;
        for (const float v : trace)
        {
            const bool rising = target >= start;
            if (rising ? (v < prev || v > target) : (v > prev || v < target))
                return false;
            prev = v;
        }
        return true;
    }

    int64_t rgba(Col c) { return (int64_t{ c.r } << 24) | (int64_t{ c.g } << 16) | (int64_t{ c.b } << 8) | c.a; }
}

int main(int argc, char** argv)
{
    T::Probe P("fg.ease", "", argc, argv);
    const float nan = std::numeric_limits<float>::quiet_NaN();

    // ---- sameBits -------------------------------------------------------------------------------------------------
    static_assert(ease::sameBits(0.5f, 0.5f) && !ease::sameBits(0.0f, -0.0f), "sameBits is a constexpr bit compare");
    P.eq("same_bits.equal", ease::sameBits(0.25f, 0.25f), 1);
    P.eq("same_bits.next_float_differs", ease::sameBits(0.25f, std::nextafter(0.25f, 1.0f)), 0);
    P.eq("same_bits.signed_zeros_differ", ease::sameBits(0.0f, -0.0f), 0);
    P.eq("same_bits.nan_equals_itself", ease::sameBits(nan, nan), 1);

    // ---- toward: HR's step, then the snap ---------------------------------------------------------------------------
    {
        const float x = 0.2f, t = 0.9f, dt = kDt60, tau = 0.09f;
        const float hr = x + (t - x) * std::fmin(1.0f, dt / tau);      // the HR idiom, BgfxEditor.cpp:944-948
        P.eq("toward.hr_step_bits", ease::sameBits(ease::toward(x, t, dt, tau), hr), 1);
    }
    P.eq("toward.dt_ge_tau_lands", ease::sameBits(ease::toward(0.0f, 1.0f, 0.2f, 0.09f), 1.0f), 1);
    P.eq("toward.snaps_within_eps", ease::sameBits(ease::toward(0.9995f, 1.0f, 0.0f, 0.09f), 1.0f), 1);
    P.eq("toward.custom_snap", ease::sameBits(ease::toward(0.95f, 1.0f, 0.0f, 0.09f, 0.1f), 1.0f), 1);
    P.eq("toward.no_move_without_dt", ease::sameBits(ease::toward(0.5f, 1.0f, 0.0f, 0.09f), 0.5f), 1);
    P.eq("toward.negative_dt_no_move", ease::sameBits(ease::toward(0.5f, 1.0f, -1.0f, 0.09f), 0.5f), 1);
    P.eq("toward.nan_dt_no_move", ease::sameBits(ease::toward(0.5f, 1.0f, nan, 0.09f), 0.5f), 1);
    P.eq("toward.zero_tau_jumps", ease::sameBits(ease::toward(0.5f, 1.0f, kDt60, 0.0f), 1.0f), 1);
    P.eq("toward.nan_tau_jumps", ease::sameBits(ease::toward(0.5f, 1.0f, kDt60, nan), 1.0f), 1);
    P.eq("toward.nan_x_jumps", ease::sameBits(ease::toward(nan, 0.3f, kDt60, 0.09f), 0.3f), 1);
    P.eq("toward.nan_target_holds", ease::sameBits(ease::toward(0.3f, nan, kDt60, 0.09f), 0.3f), 1);
    P.eq("toward.at_target_stays", ease::sameBits(ease::toward(0.7f, 0.7f, kDt60, 0.09f), 0.7f), 1);

    // A fixed-dt run lands exactly, in a finite number of ticks, monotonically, and the same way every time.
    {
        std::vector<float> a, b;
        const auto step = [](float v) { return ease::toward(v, 1.0f, kDt60, 0.09f); };
        const int n = run(0.0f, 1.0f, 1000, a, step);
        const int n2 = run(0.0f, 1.0f, 1000, b, step);
        P.in("toward.ticks_to_land_60hz", n, 1, 60);
        P.eq("toward.run_monotone_no_overshoot", monotoneNoOvershoot(0.0f, 1.0f, a), 1);
        P.eq("toward.run_repeatable", n == n2 && T::hashFloats(a) == T::hashFloats(b), 1);
        const auto stepDown = [](float v) { return ease::toward(v, -0.5f, 1.0f / 120.0f, 0.16f); };
        const int down = run(1.0f, -0.5f, 1000, a, stepDown);
        P.in("toward.ticks_to_land_down_120hz", down, 1, 240);
        P.eq("toward.down_monotone_no_overshoot", monotoneNoOvershoot(1.0f, -0.5f, a), 1);
    }

    // ---- hover: 90 ms in, 160 ms out --------------------------------------------------------------------------------
    P.eq("hover.in_is_toward_90ms",
         ease::sameBits(ease::hover(0.25f, true, kDt60), ease::toward(0.25f, 1.0f, kDt60, 0.09f)), 1);
    P.eq("hover.out_is_toward_160ms",
         ease::sameBits(ease::hover(0.75f, false, kDt60), ease::toward(0.75f, 0.0f, kDt60, 0.16f)), 1);
    P.near("hover.first_tick_in", ease::hover(0.0f, true, kDt60), kDt60 / 0.09, 1e-6);
    P.near("hover.first_tick_out", ease::hover(1.0f, false, kDt60), 1.0 - kDt60 / 0.16, 1e-6);
    {
        std::vector<float> trace;
        const int in = run(0.0f, 1.0f, 1000, trace, [](float h) { return ease::hover(h, true, kDt60); });
        P.in("hover.in_lands_ticks", in, 1, 60);
        P.eq("hover.in_monotone", monotoneNoOvershoot(0.0f, 1.0f, trace), 1);
        const int out = run(1.0f, 0.0f, 1000, trace, [](float h) { return ease::hover(h, false, kDt60); });
        P.in("hover.out_lands_ticks", out, 1, 90);
        P.eq("hover.out_monotone", monotoneNoOvershoot(1.0f, 0.0f, trace), 1);
        P.eq("hover.out_slower_than_in", out > in, 1);
    }

    // ---- shown: a big jump snaps, a small one eases and lands within 1e-4 -------------------------------------------
    P.eq("shown.big_jump_snaps", ease::sameBits(ease::shown(0.1f, 0.5f, kDt60), 0.5f), 1);
    P.eq("shown.custom_jump", ease::sameBits(ease::shown(0.1f, 0.2f, kDt60, 0.09f, 0.05f), 0.2f), 1);
    P.eq("shown.small_move_eases",
         ease::sameBits(ease::shown(0.4f, 0.5f, kDt60), 0.4f + (0.5f - 0.4f) * (kDt60 / 0.09f)), 1);
    P.eq("shown.snaps_within_1e-4", ease::sameBits(ease::shown(0.49995f, 0.5f, 0.0f), 0.5f), 1);
    P.eq("shown.holds_outside_snap_without_dt", ease::sameBits(ease::shown(0.4998f, 0.5f, 0.0f), 0.4998f), 1);
    P.eq("shown.nan_target_holds", ease::sameBits(ease::shown(0.4f, nan, kDt60), 0.4f), 1);
    {
        std::vector<float> trace;
        const int n = run(0.36f, 0.5f, 1000, trace, [](float s) { return ease::shown(s, 0.5f, kDt60); });
        P.in("shown.lands_ticks", n, 1, 90);
        P.eq("shown.monotone", monotoneNoOvershoot(0.36f, 0.5f, trace), 1);
    }

    // ---- v0.10.0: the animation speed -------------------------------------------------------------------------------
    {
        std::vector<float> trace, base;
        P.eq("scale.default_is_1", ease::timeScale() == 1.0f ? 1 : 0, 1);
        const auto step = [](float x) { return ease::toward(x, 1.0f, kDt60, 0.12f); };
        const int n1 = run(0.0f, 1.0f, 1000, base, step);
        ease::setTimeScale(1.0f);
        const int n1b = run(0.0f, 1.0f, 1000, trace, step);
        P.eq("scale.one_is_unscaled", n1 == n1b && trace == base ? 1 : 0, 1);
        ease::setTimeScale(0.0f);
        P.eq("scale.zero.toward_one_step", run(0.0f, 1.0f, 1000, trace, step), 1);
        P.eq("scale.zero.hover_one_step", ease::hover(0.0f, true, kDt60) == 1.0f ? 1 : 0, 1);
        P.eq("scale.zero.shown_one_step", ease::shown(0.40f, 0.5f, kDt60) == 0.5f ? 1 : 0, 1);
        P.eq("scale.zero.dt0_still_jumps", ease::toward(0.0f, 1.0f, 0.0f, 0.12f) == 1.0f ? 1 : 0, 1);
        ease::setTimeScale(2.0f);
        const int n2 = run(0.0f, 1.0f, 1000, trace, step);
        P.in("scale.two.ticks_ratio", static_cast<double>(n2) / static_cast<double>(n1), 1.8, 2.2);
        P.eq("scale.two.monotone", monotoneNoOvershoot(0.0f, 1.0f, trace), 1);
        ease::setTimeScale(std::numeric_limits<float>::quiet_NaN());
        P.eq("scale.nan_is_1", ease::timeScale() == 1.0f ? 1 : 0, 1);
        ease::setTimeScale(-1.0f);
        P.eq("scale.negative_is_1", ease::timeScale() == 1.0f ? 1 : 0, 1);
        ease::setTimeScale(9.0f);
        P.eq("scale.over_8_is_1", ease::timeScale() == 1.0f ? 1 : 0, 1);
        ease::setTimeScale(8.0f);
        P.eq("scale.eight_kept", ease::timeScale() == 8.0f ? 1 : 0, 1);
        ease::setTimeScale(1.0f);
    }

    // ---- Col::fade: HR's arithmetic (round half away from zero, clamp) ----------------------------------------------
    const Col ink { 0x8A, 0x8E, 0x95, 255 };
    P.eq("fade.half", fade(ink, 0.5f).a, 128);                           // 127.5 rounds away from zero
    P.eq("fade.keeps_rgb", rgba(fade(ink, 0.5f)) >> 8, rgba(ink) >> 8);
    P.eq("fade.zero", fade(ink, 0.0f).a, 0);
    P.eq("fade.one", fade(ink, 1.0f).a, 255);
    P.eq("fade.clamps_high", fade(ink, 2.0f).a, 255);
    P.eq("fade.clamps_negative", fade(ink, -1.0f).a, 0);
    P.eq("fade.nan_is_zero", fade(ink, nan).a, 0);
    P.eq("fade.scales_own_alpha", fade(Col{ 1, 2, 3, 100 }, 0.5f).a, 50);
    P.eq("fade.round_half_up_odd", fade(Col{ 1, 2, 3, 101 }, 0.5f).a, 51);

    // ---- Col::premix: the opaque colour of c at alpha a over the ground ---------------------------------------------
    const Col ground = funkgui::Theme::graphite().ground, accent = funkgui::Theme::graphite().accent;
    P.eq("premix.zero_is_ground", rgba(premix(ground, accent, 0.0f)), rgba(ground));
    P.eq("premix.one_is_colour", rgba(premix(ground, accent, 1.0f)), rgba(accent));
    P.eq("premix.half_is_mix", rgba(premix(ground, accent, 0.5f)), rgba(funkgui::mix(ground, accent, 0.5f)));
    P.eq("premix.clamps", rgba(premix(ground, accent, 3.0f)), rgba(accent));
    P.eq("premix.always_opaque", premix(ground, Col{ 10, 20, 30, 0 }, 0.7f).a, 255);
    P.eq("premix.colour_alpha_is_coverage", rgba(premix(ground, Col{ 255, 255, 255, 0 }, 1.0f)), rgba(ground));
    P.eq("premix.constexpr", [] { constexpr Col c = premix(Col{ 0, 0, 0, 255 }, Col{ 200, 100, 50, 255 }, 0.5f);
                                  return c.r == 100 && c.g == 50 && c.b == 25 && c.a == 255; }(), 1);

    // ---- Theme: `signal` carries HR's `ice` values, bit for bit -----------------------------------------------------
    P.eq("theme.graphite.signal", rgba(funkgui::Theme::graphite().signal), 0x7FD4E8FF);
    P.eq("theme.paper.signal", rgba(funkgui::Theme::paper().signal), 0x1E7E96FF);
    P.eq("theme.graphite.text_gamma_bits",
         ease::sameBits(funkgui::Theme::graphite().textGamma, 1.0f / 1.4f), 1);
    P.eq("theme.paper.text_gamma_bits", ease::sameBits(funkgui::Theme::paper().textGamma, 1.4f), 1);
    P.eq("theme.graphite.accent", rgba(accent), 0xFF5A1FFF);
    P.eq("theme.token_count_unchanged", static_cast<int64_t>(sizeof(funkgui::Theme)), 9 * 4 + 4);

    return P.finish();
}

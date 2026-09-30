#pragma once

// Every eased value in a Panel goes through these (02 §3.7 rule 2, §5.7). They take the frame's dt in seconds and read
// no clock, and each one SNAPS onto its target once it is within a small epsilon, so with a fixed dt a value lands
// exactly on its target after a finite number of ticks and HeadlessHost::settle() is exact. Motion is HR's: a one-pole
// step x += (target - x) * min(1, dt / tau), which never overshoots (A §5.2 rule 13), 90 ms in and 160 ms out for
// hover, and big jumps snap instead of gliding (BgfxEditor.cpp:940-958).
//
// Degenerate inputs are defined, never NaN-propagating: dt <= 0 (or NaN) moves nothing (the snap still applies),
// tau <= 0 (or NaN) jumps to the target, a NaN current value jumps to the target, and a NaN target leaves x as it is.
//
// v0.10.0, the animation speed: every tau these functions take is multiplied by timeScale(), 1 by default. 0 is no
// animation: every ease lands on its target in one call, as tau <= 0 does. 2 is twice as slow. It is one value for the
// whole process, a machine-wide preference that the product sets on the message thread (FCompressor's settings
// screen), and only these eases read it: meters, clocks and dwells keep their own time. setTimeScale takes 0 … 8 and
// makes anything else (a NaN, a negative) 1.

#include <bit>
#include <cmath>
#include <cstdint>

namespace funkgui::ease
{
    namespace detail
    {
        inline float timeScale = 1.0f;                   // constant-initialised; message thread (see the top)
    }

    inline float timeScale() noexcept { return detail::timeScale; }

    inline void setTimeScale(float s) noexcept { detail::timeScale = s >= 0.0f && s <= 8.0f ? s : 1.0f; }

    // Exact comparison stated as a bit compare (HR BgfxEditor.cpp:118-122): "has this value moved by any amount at
    // all". +0 and -0 differ; a NaN equals the same NaN. Used for write-only-on-change and view-cache keys.
    constexpr bool sameBits(float a, float b) noexcept
    {
        return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b);
    }

    // One step of the one-pole ease towards target, then the snap: once |target - x| <= snap, x is target exactly.
    inline float toward(float x, float target, float dt, float tau, float snap = 1e-3f) noexcept
    {
        if (std::isnan(target))
            return x;
        tau *= detail::timeScale;                        // v0.10.0: the animation speed (0: none)
        if (std::isnan(x) || !(tau > 0.0f))
            return target;
        if (dt > 0.0f)
        {
            const float k = dt >= tau ? 1.0f : dt / tau;
            x += (target - x) * k;
        }
        return std::fabs(target - x) <= snap ? target : x;
    }

    // Hover amount 0..1: 90 ms in, 160 ms out (HR BgfxEditor.cpp:944-948); snaps within 1e-3, which is also where HR
    // stopped calling it easing (BgfxEditor.cpp:1081).
    inline float hover(float h, bool on, float dt) noexcept
    {
        return toward(h, on ? 1.0f : 0.0f, dt, on ? 0.09f : 0.16f);
    }

    // A displayed value (a caret, a fill) following its target: a jump larger than `jump` snaps at once, so a preset
    // recall never sweeps across the track (HR BgfxEditor.cpp:950-957); smaller moves ease with tau and snap within
    // 1e-4 of the target (HR's settle test, BgfxEditor.cpp:1082).
    inline float shown(float s, float target, float dt, float tau = 0.09f, float jump = 0.15f) noexcept
    {
        if (!std::isnan(target) && !std::isnan(s) && std::fabs(target - s) > jump)
            return target;
        return toward(s, target, dt, tau, 1e-4f);
    }
}

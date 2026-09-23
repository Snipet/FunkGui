#pragma once

#include <cmath>
#include <cstdint>

namespace funkgui
{
    // Plain 8-bit RGBA. Lives in its own header so that the palette (Theme.h)
    // carries no dependency on bgfx — anything that only needs colours, such
    // as the preferences store or an offscreen tool, should not have to pull
    // in the GPU headers to get them.
    struct Col
    {
        uint8_t r = 0, g = 0, b = 0, a = 255;

        constexpr Col withAlpha(float f) const
        {
            return { r, g, b, static_cast<uint8_t>(
                (f <= 0.0f ? 0.0f : f >= 1.0f ? 255.0f : f * 255.0f)) };
        }
    };

    // Linear blend in the stored (gamma-encoded) space. Good enough for
    // interpolating between two tokens of the same palette, which is all the
    // UI ever asks for.
    constexpr uint8_t mixChannel(uint8_t x, uint8_t y, float u)
    {
        return static_cast<uint8_t>(static_cast<float>(x)
             + (static_cast<float>(y) - static_cast<float>(x)) * u);
    }

    constexpr Col mix(Col a, Col b, float t)
    {
        const float u = t <= 0.0f ? 0.0f : (t >= 1.0f ? 1.0f : t);
        return { mixChannel(a.r, b.r, u), mixChannel(a.g, b.g, u),
                 mixChannel(a.b, b.b, u), mixChannel(a.a, b.a, u) };
    }

    // A colour at a fraction of its own alpha, for crossfades (02 §2.2). HR had two copies (BgfxEditor.cpp:83,
    // PresetPanel.cpp:56); this is the same arithmetic. It scales rather than sets, so an ink level that is itself an
    // alpha keeps its ratio. The alpha is c.a * a rounded half away from zero and clamped to 0..255, as HR's
    // jlimit(0, 255, std::round(c.a * a)); a NaN factor gives alpha 0.
    inline Col fade(Col c, float a) noexcept
    {
        const float v = std::round(static_cast<float>(c.a) * a);
        c.a = static_cast<uint8_t>(!(v > 0.0f) ? 0.0f : (v >= 255.0f ? 255.0f : v));
        return c;
    }

    // The opaque colour that c, drawn at alpha a over the flat ground, looks like (A §2.6). A curve is a run of capsule
    // segments whose ends overlap, so a translucent curve double-blends at every join and shows beads; on the flat
    // ground this pre-mixed colour is exact and bead-free. c's own alpha counts as coverage (every theme token is
    // opaque, so for the palette this is mix(ground, c, a) with the rounding of mix()). The result is always opaque.
    constexpr Col premix(Col ground, Col c, float a)
    {
        const float u = (a <= 0.0f ? 0.0f : (a >= 1.0f ? 1.0f : a)) * (static_cast<float>(c.a) / 255.0f);
        return { mixChannel(ground.r, c.r, u), mixChannel(ground.g, c.g, u), mixChannel(ground.b, c.b, u), 255 };
    }
}

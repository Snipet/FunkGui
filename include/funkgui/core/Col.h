#pragma once

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
}

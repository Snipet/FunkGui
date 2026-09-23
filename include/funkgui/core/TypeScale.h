#pragma once

// The type scale, shared by every part of the panel that sets text. Hierarchy
// comes from size, case, tracking, colour and the SDF weight axis — never from
// a second face. 10px is the hard floor: below that the field is more than a
// 4.8x downsample of a 48px bake and thin stems disappear.

#include "SdfCanvas.h"

namespace hrvbgui::type
{
    using TS = SdfCanvas::TextStyle;

    inline constexpr TS kDisplay { 44.0f, 0.0f,  0.03f, true  };
    inline constexpr TS kValueP  { 24.0f, 0.0f,  0.00f, true  };
    inline constexpr TS kValueS  { 18.0f, 0.0f,  0.00f, true  };
    inline constexpr TS kNumeral { 18.0f, 0.0f,  0.00f, true  };
    inline constexpr TS kWordmark{ 13.0f, 2.6f,  0.05f, false };
    inline constexpr TS kLatch   { 13.0f, 2.0f,  0.05f, false };
    inline constexpr TS kLabel   { 11.0f, 1.6f,  0.03f, false };
    inline constexpr TS kCaption { 10.0f, 2.0f,  0.03f, false };
    inline constexpr TS kMicro   { 10.0f, 1.4f,  0.03f, false };
}

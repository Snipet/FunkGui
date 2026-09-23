#pragma once

// One recorded primitive (02 §3.2): exactly one dump line, expanded by BgfxSink into one axis-aligned quad of HR's
// 64-byte vertices (canvas/Expand.h). The layout is lossless against HR's vertex stream: HR's four corners carry the
// local coordinates (d0.x, d0.y), (e0.x, d0.y), (e0.x, e0.y), (d0.x, e0.y) for every kind (SdfCanvas.cpp rrect
// :108-111, segment :143-146, text :252-255), so storing the top-left and bottom-right corners loses nothing.

#include <funkgui/canvas/Tags.h>

#include <cstdint>
#include <type_traits>

namespace funkgui
{
    // Not `Kind`: fcdsp::Kind exists (K1 #18). The value is stored in d2[2] as a float (the shader's kind).
    enum class PrimKind : uint8_t { rrect = 0, text = 1, segment = 2, area = 3 };

    // Flags, stored in d2[3] as a small integer-valued float (0..7); decoded as int(d2[3] + 0.5) on the CPU.
    namespace pflag
    {
        inline constexpr uint32_t live         = 1u << 0;   // animated/live data: excluded from geometry fingerprints
        inline constexpr uint32_t strokeBottom = 1u << 1;   // AREA: stroke the bottom edge instead of the top
        inline constexpr uint32_t strokeBoth   = 1u << 2;   // AREA: stroke both edges
    }

    // Which edge of an AREA column is stroked (02 §4.1): top = no flag, bottom = strokeBottom, both = strokeBoth.
    enum class AreaEdge : uint8_t { top, bottom, both };

    struct Prim                               // 84 bytes
    {
        float    x0, y0, x1, y1;              // quad TL / BR in logical px, AA apron included
        uint32_t c0, c1;                      // RGBA8, r | g<<8 | b<<16 | a<<24 (HR pack())
        float    d0[4];                       // TL varyings: local x,y (text: u0,v0), hw, hh
        float    e0[2];                       // BR local x,y (text: u1,v1)
        float    d1[4];                       // per kind (A §2.2 table; AREA 02 §4.1)
        float    d2[4];                       // [0] border/stroke half-width, [1] softness, [2] PrimKind, [3] flags
        Tag      tag;                         // CPU-only, never uploaded
        uint16_t reserved = 0;
    };
    static_assert(sizeof(Prim) == 84 && std::is_trivially_copyable_v<Prim>, "Prim is 84 bytes (02 §3.2)");
}

#pragma once

// The vertex expansion (02 §3.2, §3.11 fg.canvas.expansion): a Prim becomes HR's six non-indexed 64-byte vertices
// a, b, c, a, c, d (SdfCanvas::quad). a = (x0, y0) with local (d0.x, d0.y), b = (x1, y0) with (e0.x, d0.y),
// c = (x1, y1) with (e0.x, e0.y), d = (x0, y1) with (d0.x, e0.y); every vertex carries the Prim's c0, c1, d0[2..3],
// d1 and d2. The tag stays on the CPU. A pure function with no bgfx, so it is testable headless: BgfxSink (Gpu)
// uploads exactly this stream, whose layout matches BgfxContext's vertex layout (A §2.2).
//
// Declared in G2 (v0.2.0, frozen at FZ1); implemented by G3 with BgfxSink (the recorder-parity spike).

#include <funkgui/canvas/Prim.h>
#include <funkgui/canvas/PrimList.h>

#include <cstdint>
#include <type_traits>
#include <vector>

namespace funkgui
{
    struct Vtx                                   // HR SdfCanvas.h:91-98; offsets 0, 8, 12, 16, 32, 48
    {
        float    x, y;                           // logical px
        uint32_t c0, c1;                         // Color0 / Color1, RGBA8 normalised
        float    d0[4];                          // TexCoord0
        float    d1[4];                          // TexCoord1
        float    d2[4];                          // TexCoord2
    };
    static_assert(sizeof(Vtx) == 64 && std::is_trivially_copyable_v<Vtx>, "Vtx is HR's 64-byte vertex (A §2.2)");

    inline constexpr int kVerticesPerPrim = 6;

    // The six vertices of one primitive into out[0..5].
    void expand(const Prim&, Vtx* out) noexcept;

    // Every primitive of the list, in order, into out: resized to kVerticesPerPrim * prims.size(), capacity kept.
    void expand(const PrimList&, std::vector<Vtx>& out);
}

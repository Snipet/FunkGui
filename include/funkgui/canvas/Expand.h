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
    // Shader draws are not in it (v0.19.0: expandShader below).
    void expand(const PrimList&, std::vector<Vtx>& out);

    // ---- v0.19.0 additions: shader draws (Canvas::shader, PrimList::shaders) ----------------------------------------
    // What BgfxSink does with a frame that has shader draws, as pure functions with no bgfx, so that the split, the
    // vertices and the scissor are testable headless (fg.canvas.shader) and SoftRaster can follow the same order.

    // The six vertices of one shader draw's quad into out[0..5], in expand()'s corner order a, b, c, a, c, d over the
    // rect's corners: x, y are the corner in logical px (a_position) and d0 = (u, v, px, py) there (a_texcoord0: u, v
    // run 0..1 across the rect, y down; px, py are logical px from its top-left corner, 0..w and 0..h). Everything
    // else is 0. FunkGui's custom vertex shader (shaders/vs_custom.sc) passes d0.xy on as v_uv and d0.zw as v_px. The
    // clip is not applied here (the scissor is shaderPixels).
    void expandShader(const ShaderDraw&, Vtx* out) noexcept;

    // One draw call of a frame: a run of primitives with the UI program, or one shader draw with its own program.
    struct DrawStep
    {
        enum class Kind : uint8_t { prims, shader };
        Kind     kind = Kind::prims;
        uint32_t first = 0;                      // prims: the run's first primitive; shader: its index in list.shaders
        uint32_t count = 0;                      // prims: how many (>= 1); shader: 1
    };

    // The frame's draw calls in drawing order, into out (cleared, capacity kept): the primitives split into runs at
    // every shader draw, each shader draw between the runs at its place, and no empty run. Without shader draws it is
    // one run of every primitive (nothing for an empty list): the one draw call a frame always had. A draw whose `at`
    // is past the end draws after every primitive; one whose `at` is below an earlier draw's (out of order: Canvas never
    // records that) draws right after that earlier one.
    void drawSteps(const PrimList&, std::vector<DrawStep>& out);

    // A rectangle of physical pixels (BgfxSink's scissor): x, y from the top-left, w x h of them.
    struct PixelRect
    {
        int x = 0, y = 0, w = 0, h = 0;
    };

    // The pixels a shader draw may write in a physW x physH frame of `info`: those whose centres lie in the
    // half-open rect ∩ clip (SoftRaster's and the rasterisers' rule, so the scissor follows the quad's own edges and a
    // clipped primitive's cut ones), clamped to the frame. Logical px map to physical by physW / logicalW in x and
    // physH / logicalH in y, as the vertex shaders map them. Empty (w or h 0) when no pixel qualifies or the frame
    // has no size.
    PixelRect shaderPixels(const ShaderDraw&, const FrameInfo&, int physW, int physH) noexcept;
}

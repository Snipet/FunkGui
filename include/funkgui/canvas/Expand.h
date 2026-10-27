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
    // Shader and image draws are not in it (v0.19.0: expandShader below; v0.20.0: expandImage).
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

    // One draw call of a frame: a run of primitives with the UI program, or one shader draw with its own program, or
    // (v0.20.0) one image draw with the image program.
    struct DrawStep
    {
        enum class Kind : uint8_t { prims, shader, image };
        Kind     kind = Kind::prims;
        uint32_t first = 0;                      // prims: the run's first primitive; shader: its index in list.shaders;
                                                 // image: its index in list.images
        uint32_t count = 0;                      // prims: how many (>= 1); shader, image: 1
    };

    // The frame's draw calls in drawing order, into out (cleared, capacity kept): the primitives split into runs at
    // every shader and image draw, each of those between the runs at its place, and no empty run. Without shader and
    // image draws it is one run of every primitive (nothing for an empty list): the one draw call a frame always had.
    // Shader and image draws follow each other in recording order (an image draw comes after the list.shaders[i] with
    // i < its shaderAt and before the others). A draw whose `at` is past the end draws after every primitive; one whose
    // `at` is below an earlier draw's (out of order: Canvas never records that) draws right after that earlier one.
    // The dump and SoftRaster follow these steps too.
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

    // ---- v0.20.0 additions: image draws (Canvas::image, PrimList::images) -------------------------------------------
    // The image program's half of the work as pure functions, testable headless (fg.canvas.image) and shared by
    // SoftRaster, BgfxSink and WebGlSink, so all three cover, sample and scissor alike.

    // The apron around an image's dst, in logical px on every side: the rrect's (Canvas::rrect pads 1.5), so the
    // antialiased edge is drawn whole at dpi 2 / 3 and above.
    inline constexpr float kImageApron = 1.5f;

    // The six vertices of one image draw's quad into out[0..5], in expand()'s corner order a, b, c, a, c, d over the
    // corners of dst grown by kImageApron: x, y are the corner in logical px and d0 = (u, v, px, py) there, where px,
    // py are logical px from dst's top-left corner (-kImageApron .. w + kImageApron) and u = px / w, v = py / h (0..1
    // across dst itself). Everything else is 0. The vertex shader is the custom programs' (shaders/vs_custom.sc: d0.xy
    // becomes v_uv, d0.zw v_px). The clip is not applied here (the scissor is imagePixels).
    void expandImage(const ImageDraw&, Vtx* out) noexcept;

    // The pixels an image draw may write in a physW x physH frame of `info`: those whose centres lie in dst grown by
    // kImageApron, ∩ the clip when clipped, clamped to the frame (shaderPixels' rule and mapping). Empty when none.
    PixelRect imagePixels(const ImageDraw&, const FrameInfo&, int physW, int physH) noexcept;

    // The level of a picW x picH picture (images::levelCount levels) that an image draw samples at dpi: the largest l
    // with 2^l <= the texels of uv per device px along the axis that has more of them (uv.w * picW / (dst.w * dpi) and
    // uv.h * picH / (dst.h * dpi)), at most the last level; 0 when the picture is shown at its size or larger, or the
    // numbers are not finite. One level for the whole draw, so the level never depends on a pixel's derivatives and
    // every sink samples the same texels.
    int imageLevel(const ImageDraw&, float dpi, int picW, int picH) noexcept;

    // Where an image draw samples level `level` of a picW x picH picture, in the texture's 0..1 coordinates: a point
    // of dst at (u, v) (0..1 across dst) samples (uv.x + u * uv.w, uv.y + v * uv.h), clamped to [lo, hi] per axis,
    // where lo and hi are the centres of the outermost texels of that level that uv covers (half a texel inside uv's
    // edges); when uv is narrower than one texel of the level, lo = hi = its middle. The sinks pass these as uniforms.
    struct ImageSampling
    {
        float u0 = 0.0f, v0 = 0.0f, du = 1.0f, dv = 1.0f;   // uv as given
        float loU = 0.0f, loV = 0.0f, hiU = 1.0f, hiV = 1.0f;
    };
    ImageSampling imageSampling(const ImageDraw&, int level, int picW, int picH) noexcept;
}

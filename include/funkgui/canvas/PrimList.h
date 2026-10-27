#pragma once

// A recorded frame (02 §3.2): what Canvas::end() returns, what BgfxSink submits, what the dump (§3.8) writes and parses
// and what the fingerprint (§3.9) hashes. Declared in G2 (v0.2.0, frozen at FZ1); clear(), writeText() and parseText()
// are implemented with the recorder (G3).

#include <funkgui/canvas/Axis.h>
#include <funkgui/canvas/Prim.h>
#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Col.h>
#include <funkgui/core/Geometry.h>

#include <cstdint>
#include <cstdio>
#include <string_view>
#include <type_traits>
#include <vector>

namespace funkgui
{
    struct FrameInfo
    {
        int      logicalW = 0, logicalH = 0;
        float    dpi = 1.0f;                  // physical / logical height (HR)
        Col      clear{};                     // the theme's ground
        float    textGamma = 1.0f;
        int      theme = 0;
        float    seconds = 0.0f;              // u_viewSize.w
        uint32_t frame = 0;
        float    dt = 0.0f;
        bool     fixedClock = false;          // dump "clock fixed": a HeadlessHost or a FIXED_DT capture
        // G2 additions for the dump's view line and overflow count (02 §3.8, §4.5), which FrameInfo did not carry:
        bool     displayLinked = false;       // "clock displaylink" (else "timer") when not fixedClock
        float    fps = 0.0f;                  // measured frame rate ("fps 0.0" for a fixed clock)
        bool     fullRate = false;            // "rate full" / "rate idle"
        uint32_t overflows = 0;               // BgfxSink drops so far ("overflow N"; 0 = no line)
    };

    // ---- v0.19.0 addition: a shader draw (Canvas::shader) -----------------------------------------------------------
    // A rectangle that a host's own fragment shader fills, in order with the primitives: it is drawn after
    // prims[at - 1] and before prims[at] (at == prims.size(): after every primitive), and several at one position draw
    // in the order they were recorded. Kept beside the prims, never among them: the Prim layout, funkgui::expand's vertex stream and
    // the UI shaders are untouched, and a frame without shader draws is exactly what it was before v0.19.0.
    //
    // Who draws what (each a stated behaviour):
    // - BgfxSink (gpu/BgfxSink.h): the program registered under `id` (gpu/ShaderLibrary.h) over `rect`, scissored to
    //   rect ∩ clip; nothing when no program is registered under it or bgfx refused it.
    // - SoftRaster (canvas/SoftRaster.h; HeadlessHost::writePng and every probe PNG): an opaque placeholder over
    //   rect ∩ clip at its place in the order (a dark fill, a lighter 1 device px border, diagonal stripes).
    // - WebGlSink (web/WebGlSink.h): nothing (a browser has no shaderc); the primitives around it draw as ever.
    // - The dump (PrimList::writeText): a "shader" line at its place among the "p" lines. Fingerprints: canvas/
    //   Fingerprint.h.
    struct ShaderDraw
    {
        static constexpr int kMaxParams = 16;

        uint32_t id = 0;                      // the host's program id (shaders::set)
        uint32_t at = 0;                      // the primitives recorded before it (counted after a clip cut them)
        Rect     rect{};                      // logical px, as recorded: u_rect; v_uv runs 0..1 across it
        Rect     clip{};                      // the clip in force when recorded (the open clips intersected), or {}
        bool     clipped = false;             // whether a clip was in force
        bool     live = false;                // the canvas's live flag when recorded (Canvas::setLive)
        Tag      tag = 0;                     // the canvas's tag when recorded
        float    params[kMaxParams] = {};     // u_params[0..3], in order; what the call did not give is 0
    };
    static_assert(std::is_trivially_copyable_v<ShaderDraw>, "a ShaderDraw is plain data");

    // ---- v0.20.0 addition: an image draw (Canvas::image) ------------------------------------------------------------
    // A picture of the registry (canvas/Images.h) drawn into `dst`, in order with the primitives and the shader draws:
    // after prims[at - 1] and before prims[at], and after shaders[shaderAt - 1] and before shaders[shaderAt] (both
    // counts are what had been recorded before it), so a frame draws its shader and image draws in the order they
    // were recorded, each at its place among the primitives. Kept beside the prims, as shader draws are: the Prim
    // layout, funkgui::expand's vertex stream and the UI shaders are untouched, and a frame without image draws is
    // exactly what it was before v0.20.0.
    //
    // What every sink draws (SoftRaster, BgfxSink, WebGlSink; the rule is SoftRaster's and shaders/fs_image.sc's):
    // - the samples whose centres lie in the half-open quad `dst` grown by kImageApron (canvas/Expand.h) on every side,
    //   ∩ the clip when clipped (a primitive's rule; the clip is the GPU sinks' scissor, never a cut of the quad);
    // - each with coverage = (1 - smoothstep(-aa, aa, d)) * opacity, aa = 1 / dpi (one device px, the rrects' edge)
    //   and d the signed distance to the rounded rectangle dst with corner radius min(radius, w / 2, h / 2): the edge
    //   of rrect(dst, radius, ...) exactly, soft over two device px;
    // - and the colour of the picture's level imageLevel() (canvas/Expand.h) sampled bilinearly, premultiplied, at
    //   the point of `uv` (0..1 of the picture: the sub-rectangle stretched over dst) that the sample's place in dst
    //   maps to, clamped to the centres of the outermost texels inside uv, so nothing outside uv bleeds in and the
    //   apron repeats the edge; unpremultiplied and blended source-over as the primitives are (BGFX_STATE_BLEND_ALPHA).
    // A picture that is not in the registry when the frame is drawn draws nothing. The dump (PrimList::writeText)
    // writes an "image" line at its place; fingerprints: canvas/Fingerprint.h.
    struct ImageDraw
    {
        uint32_t id = 0;                      // the registry's id (images::set)
        uint32_t at = 0;                      // the primitives recorded before it (counted after a clip cut them)
        uint32_t shaderAt = 0;                // the shader draws recorded before it
        Rect     dst{};                       // logical px, as recorded
        Rect     uv{ 0.0f, 0.0f, 1.0f, 1.0f };   // the sub-rectangle of the picture, inside 0..1, w and h > 0
        float    radius = 0.0f;               // logical px, >= 0
        float    opacity = 1.0f;              // 0..1
        Rect     clip{};                      // the clip in force when recorded (the open clips intersected), or {}
        bool     clipped = false;             // whether a clip was in force
        bool     live = false;                // the canvas's live flag when recorded (Canvas::setLive)
        Tag      tag = 0;                     // the canvas's tag when recorded
    };
    static_assert(std::is_trivially_copyable_v<ImageDraw>, "an ImageDraw is plain data");

    struct PrimList
    {
        FrameInfo            info{};
        std::vector<Prim>    prims;
        std::vector<AxisRec> axes;
        uint32_t missingGlyphs = 0;           // codepoints not in the atlas (HR drops them silently, A §2.5)
        uint32_t missingFirst[8] = {};        // the first distinct missing codepoints, for the dump
        // v0.19.0: the frame's shader draws in recording order, so `at` never decreases along the vector (Canvas keeps
        // it so; a sink, SoftRaster or the dump given one that does draws or writes that draw as early as it can).
        std::vector<ShaderDraw> shaders;
        // v0.20.0: the frame's image draws in recording order (`at` and `shaderAt` never decrease along the vector),
        // and the draw statistics a probe reads off HeadlessHost::draw(): images.size() pictures drawn, and
        // missingImages calls of Canvas::image that named an id the registry did not hold (each recorded nothing).
        std::vector<ImageDraw> images;
        uint32_t missingImages = 0;

        void clear();                         // empties everything, keeps the vectors' capacity
        // Dump v2 (§3.8); false on a write error. v0.19.0: each shader draw is a line before the "p" line of prims[at]
        // (after the last "p" line when at == prims.size()):
        //   shader 7 at 12  rect 10 20 200 100  clip 0 0 400 300  params 0.5 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0  live 0  t X
        // "clip -" when no clip was in force, all 16 params always, "t NAME" only when tagged (as on a "p" line).
        // v0.20.0: each image draw is an "image" line in drawing order among the "p" and "shader" lines, and
        // missingImages, when not 0, an "images missing N" line after the "glyphs" line:
        //   image 3 at 12 shader 1  dst 10 20 64 48  uv 0 0.125 1 0.75  radius 6  opacity 1  clip -  live 0  t X
        bool writeText(std::FILE*) const;
        // Accepts HR's v1 and v2; false on malformed input. A "shader" line (v2 only) must have at == the number of "p"
        // lines before it; an "image" line (v2 only) at == the "p" lines and shader == the "shader" lines before it.
        static bool parseText(std::string_view, PrimList&);
    };
}

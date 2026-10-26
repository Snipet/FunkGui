#pragma once

// The CPU recorder (02 §3.3): every draw call appends Prims to a PrimList; nothing here touches a GPU. The HR
// primitives keep SdfCanvas.cpp's maths byte for byte, so BgfxSink's expansion of the list is bit-identical to what HR
// submitted for the same calls (the recorder-parity spike, G3). The canvas is a member of its host, never constructed
// per frame, so the list keeps its capacity (A §2.1). Text needs only atlas.baked() (C §3.1), no texture.
//
// Declared in G2 (v0.2.0, frozen at FZ1). Implemented by G3 (src/canvas/Canvas.cpp: the HR primitives, begin/end,
// tags, text) and G4 (src/canvas/CanvasShapes.cpp: area, areaStrip, polyline, disc, dotted, axis); both append through
// emit(), which stamps the current tag and live flag.

#include <funkgui/canvas/Axis.h>
#include <funkgui/canvas/Prim.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/core/Col.h>
#include <funkgui/core/Flatten.h>                    // FSynth G1: the curves' default tolerances
#include <funkgui/core/Geometry.h>
#include <funkgui/text/FontAtlasSdf.h>
#include <funkgui/text/TextStyle.h>

#include <array>
#include <cstddef>

namespace funkgui
{
    class Canvas
    {
    public:
        explicit Canvas(const FontAtlasSdf& atlas);

        void begin(const FrameInfo&);                  // clears the list (capacity kept) and takes the frame's info
        const PrimList& end();                         // valid until the next begin()

        // HR primitives; maths byte-identical to SdfCanvas.cpp
        void  rrect (float x, float y, float w, float h, float radius, Col fill,
                     float borderW = 0, Col border = {}, float softness = 0);
        void  rrect4(float x, float y, float w, float h, float rTL, float rTR, float rBR, float rBL,
                     Col fill, float borderW = 0, Col border = {}, float softness = 0);
        void  hairlineH(float x, float y, float w, Col);   // 1 device px tall; floors y to a device px (HR)
        void  hairlineV(float x, float y, float h, Col);   // 1 device px wide; floors x
        void  segment(float x0, float y0, float x1, float y1, float width, Col, float softness = 0);
        float snapY(float y) const;                    // round to a device px
        float snapX(float x) const;
        void  text(const char* utf8, float x, float yTop, const TextStyle&, Col, Align = Align::left);
        float textWidth(const char* utf8, const TextStyle&) const;         // == text::width (text/TextFit.h)
        float capCentreTop(float centreY, const TextStyle&) const;
        float sharedBaselineTop(float otherTop, const TextStyle& other, const TextStyle& mine) const;

        // new primitives (02 §4)
        void area(float x0, float x1, float yTop0, float yTop1, float yBot0, float yBot1,
                  Col fill, float strokeW = 0, Col stroke = {}, AreaEdge edge = AreaEdge::top);
        void areaStrip(const float* xs, int n, const float* yTop, const float* yBot /*nullable*/, float yBase,
                       Col fill, float strokeW = 0, Col stroke = {}, AreaEdge edge = AreaEdge::top);
        void polyline(const float* xs, const float* ys, int n, float width, Col opaque);
        void disc(float cx, float cy, float r, Col fill, float ringW = 0, Col ring = {});
        void dotted(float x, float y, float w, float step, Col);             // 1-device-px dots
        void axis(Tag, const AxisMap* x, const AxisMap* y);                  // recorded for probes, not drawn

        // Stamped onto every primitive until changed.
        void setTag(Tag);
        Tag  tag() const;
        void setLive(bool);
        bool live() const;

        // Sets a tag and the live flag for a scope and restores the previous ones on exit.
        struct Scope
        {
            Scope(Canvas&, Tag, bool live);
            ~Scope();
            Scope(const Scope&) = delete;
            Scope& operator=(const Scope&) = delete;

            Canvas& c;
            Tag     t;                                 // the tag to restore
            bool    l;                                 // the live flag to restore
        };

        // ---- v0.9.0 addition: clipping (FCompressor's smooth-scrolling lists) ---------------------------------------
        // Every primitive recorded from pushClip() to its popClip() is cropped to the rectangle, intersected with the
        // clips around it. A primitive's quad is cut at the rectangle's edges and its local coordinates (a glyph's
        // atlas uvs) are interpolated to the new corners, so each sample inside draws as the uncropped primitive's did;
        // a primitive wholly outside is dropped, and one wholly inside is left bit for bit. It happens on the CPU, at
        // popClip(): the PrimList, the dump, the fingerprints, SoftRaster and BgfxSink see ordinary primitives, and no
        // shader or vertex changes. The cut is a hard edge: put it on a device px with snapX/snapY (whole logical px
        // are on one only at dpi 1 and 2, and a web host's dpi is physical height / logical height). An edge through
        // device pixel centres in y is filled one row further down by WebGlSink than by SoftRaster and the native
        // sinks (web/WebGlSink.h).
        // Clips nest kMaxClips deep; a push beyond that is ignored, and so is its pop. begin() drops the clips left
        // open and end() closes them. Axis records are never clipped.
        static constexpr int kMaxClips = 8;
        void pushClip(const Rect&);
        void popClip();
        int  clipDepth() const noexcept;               // pushes not yet popped (those beyond kMaxClips included)

        // pushClip for a scope, popClip on exit.
        struct ClipScope
        {
            ClipScope(Canvas&, const Rect&);
            ~ClipScope();
            ClipScope(const ClipScope&) = delete;
            ClipScope& operator=(const ClipScope&) = delete;

            Canvas& c;
        };

        float dpi() const;
        int   missingGlyphs() const;                   // this frame's codepoints not in the atlas

        // ---- FSynth G1 additions: curves as runs of capsule segments (src/canvas/CanvasCurves.cpp) ------------------
        // Flattened with geom:: (core/Flatten.h) at the frame's dpi() and recorded through polyline(), so there is no
        // new primitive kind, shader, dump field or SoftRaster branch, and BgfxSink and WebGlSink draw them as they
        // draw any segment. The colour must be opaque (premix() it over the ground): translucent capsules bead where
        // their ends overlap, as polyline() says. Angles are radians, 0 = +x, increasing clockwise on screen.
        // The last argument of arc and cubic, tolDevicePx, is the largest chord error in device px (how far a chord may
        // stray from its curve), turned into logical px by dpi(): rings default to geom::kArcDevicePx (0.1), cables to
        // geom::kFlattenDevicePx (0.6). A finer tolerance records more, shorter pieces, at most
        // geom::kMaxCurvePoints - 1 per call. A tolerance that is not > 0 (or is NaN) records nothing. arcDotted
        // records no chords and takes no tolerance.
        // On Linux, without JUCE and on a web host: the same source and the same primitives (platform-neutral).
        //
        // arc: geom::arcSegments(r, a0, a1, dpi(), tolDevicePx) capsules of `width` along the circle of radius r, from
        // a0 to a1. A radius or a width that is not > 0 records nothing; no sweep records one zero-length capsule, a
        // round dot.
        void arc(Point centre, float r, float a0, float a1, float width, Col opaque,
                 float tolDevicePx = geom::kArcDevicePx);
        // arcDotted: one round dot (a zero-length capsule of diameter dotW) every stepPx of arc length, from a0
        // towards a1: at the lengths k * stepPx for every k with k * stepPx < r * |a1 - a0| (the half-open arc, as
        // dotted()'s track), so a0 always carries a dot when the arc has any length and a1 never does. A radius, a
        // step or a dot width that is not > 0 records nothing; at most 4096 dots. Each dot's centre is placed on the
        // circle itself (sine and cosine in double), so the dots do not depend on dpi().
        void arcDotted(Point centre, float r, float a0, float a1, float stepPx, float dotW, Col opaque);
        // cubic: the Bezier p[0..3] as geom::flattenCubic's pieces at dpi() and tolDevicePx, capsules of `width`. A
        // null p or a width that is not > 0 records nothing.
        void cubic(const Point p[4], float width, Col opaque, float tolDevicePx = geom::kFlattenDevicePx);

        // ---- v0.19.0 addition: custom shaders (src/canvas/Canvas.cpp) -----------------------------------------------
        // Records a shader draw (canvas/PrimList.h ShaderDraw): the rectangle r (logical px) filled by the fragment
        // shader the host registered under `id` (gpu/ShaderLibrary.h), at this point in the order: what was recorded
        // before it draws under it, what is recorded after it over it. Kept with it: the clip in force (the open clips
        // intersected, or none), the current tag and live flag, and params[0..nParams) as u_params, the rest 0
        // (nParams is clamped to 0..ShaderDraw::kMaxParams; a null params gives none).
        // Records nothing when r is not finite or not > 0 wide and high, or when the clip in force leaves none of it.
        // The rectangle itself is never cut: v_uv runs 0..1 across all of r, and the clip becomes the GPU's scissor
        // when the frame is drawn. A clip popped later moves `at` to the primitives that clip kept, so the order holds.
        // Drawn by BgfxSink with the registered program; by SoftRaster as a placeholder; by WebGlSink not at all.
        // Plain C++: the same on Linux, without JUCE and on a web host.
        void shader(uint32_t id, const Rect& r, const float* params = nullptr, int nParams = 0);

    protected:
        // Appends one Prim stamped with the current tag and live flag and with d2[2] = kind (K3 #16): the HR
        // primitives (Canvas.cpp, G3) and the new ones (CanvasShapes.cpp, G4) both append through it.
        Prim& emit(PrimKind);

    private:
        // Private state: completed by the implementing card (G3); not part of the frozen API.
        const FontAtlasSdf& atlas_;
        PrimList list_;
        Tag      tag_ = 0;
        bool     live_ = false;

        // v0.9.0: the open clips, innermost last (each already intersected with the one before), and where each one's
        // primitives start in list_.prims. No allocation: a fixed stack.
        struct Clip
        {
            Rect        r{};
            std::size_t first = 0;
        };
        std::array<Clip, kMaxClips> clips_{};
        int clipDepth_ = 0;
    };
}

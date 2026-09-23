#pragma once

#include <bgfx/bgfx.h>
#include "Col.h"
#include <cstdint>
#include <vector>

namespace funkgui
{
    class BgfxContext;

    // Immediate-mode SDF drawing into one bgfx view per frame.
    //
    // Every primitive goes into a single vertex list and is submitted as one
    // draw call, so draw order is exactly call order. The previous design kept
    // three separate batches and always emitted rects, then arcs, then text,
    // which made z-order impossible to express.
    class SdfCanvas
    {
    public:
        explicit SdfCanvas(BgfxContext& ctx) : ctx_(ctx) {}

        void begin(bgfx::ViewId view, bgfx::FrameBufferHandle fb,
                   int logicalW, int logicalH, int physW, int physH,
                   Col clear, float seconds);

        // radius may be a single value or per corner (topLeft, topRight,
        // bottomRight, bottomLeft). softness > 0 turns the shape into a glow.
        void rrect(float x, float y, float w, float h, float radius,
                   Col fill, float borderW = 0.0f, Col border = {},
                   float softness = 0.0f);
        void rrect4(float x, float y, float w, float h,
                    float rTL, float rTR, float rBR, float rBL,
                    Col fill, float borderW = 0.0f, Col border = {},
                    float softness = 0.0f);

        // Hairlines and anything else that must land on an exact device pixel.
        // A 1-logical-px rule at 2x backing scale straddles two physical
        // pixels and renders as a soft grey band unless it is snapped.
        void hairlineH(float x, float y, float w, Col c);
        void hairlineV(float x, float y, float h, Col c);

        // An antialiased line of any angle: a capsule of the given width from
        // (x0,y0) to (x1,y1). A curve is a run of these; consecutive segments
        // overlap by a radius at the joins, which the SDF feathering makes
        // seamless. Same draw call as everything else.
        void segment(float x0, float y0, float x1, float y1, float width, Col c,
                     float softness = 0.0f);
        float snapY(float y) const;

        struct TextStyle
        {
            float px       = 13.0f;
            float tracking = 0.0f;   // extra advance per glyph, logical px
            float weight   = 0.0f;   // SDF threshold shift; ~0.05 reads semibold
            bool  tabular  = false;  // every digit takes the widest digit's slot
        };

        enum class Align { left, centre, right };

        // y is the TOP of the line's em box.
        void  text(const char* s, float x, float y, const TextStyle& st, Col c,
                   Align align = Align::left);
        float textWidth(const char* s, const TextStyle& st) const;

        // Vertical placement helpers derived from real outline metrics, so a
        // label sits optically centred rather than centred on the font's line
        // box (which leaves it visibly high).
        float capCentreTop(float centreY, const TextStyle& st) const;

        // Top-of-em-box for `mine` such that it sits on the same baseline as a
        // run of `other` drawn at otherTop. Call sites used to open-code this
        // as a 0.78 fudge factor because the canvas exposed no way to ask; the
        // exact multiplier is the font's own ascent/kBasePx, which changes
        // whenever the bundled face does.
        float sharedBaselineTop(float otherTop, const TextStyle& other,
                                const TextStyle& mine) const;

        void end();

        // Dark-on-light needs the inverse of the light-on-dark correction.
        void setTextGamma(float g) { textGamma_ = g; }

        // Diagnostics: when <ENV_PREFIX>CANVAS_DUMP names a path, the first frame's
        // vertex list is written there as text. The editor has no headless
        // mode, so this is the only way to inspect the geometry the layout
        // code really produces rather than a reimplementation of it.
        static void dumpNextFrameTo(const char* path, int skipFrames = 0);

    private:
        struct Vtx
        {
            float x, y;
            uint32_t c0, c1;
            float d0[4];
            float d1[4];
            float d2[4];
        };

        void quad(const Vtx& a, const Vtx& b, const Vtx& c, const Vtx& d);

        BgfxContext& ctx_;
        bgfx::ViewId view_ = 0;
        float viewW_ = 1.0f, viewH_ = 1.0f;
        float dpiScale_ = 1.0f;
        float seconds_ = 0.0f;
        float textGamma_ = 1.0f / 1.4f;
        Col   clear_{};
        std::vector<Vtx> verts_;
    };
}

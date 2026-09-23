#include <funkgui/canvas/Canvas.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>

// The recorder's new shapes (02 §4.1, §4.2; G4). Canvas.cpp (G3) holds HR's primitives; these append through the same
// emit(), which stamps the current tag and live flag. Only area() is a new kind (PrimKind::area, fs_ui.sc's fourth
// branch, mirrored by SoftRaster); polyline, disc and dotted are HR primitives underneath, and axis() draws nothing.
//
// Choices where 02 §4.1–§4.2 is silent:
// - area(): the quad's y extent covers all four edge ends (the smaller of the four minus the apron to the larger plus
//   it), which is 02 §4.1's min(yTop) / max(yBot) whenever the top edge is above the bottom one, and still covers the
//   stroke when a caller crosses them. d0/e0 carry the quad's own half extents (hw = half the column width, hh = half
//   the quad height, apron included): the corners' local coordinates are exactly (-hw, -hh) and (hw, hh), so the
//   shader's t = p.x / (2 hw) + 0.5 runs 0..1 across the column. A column that is not strictly wider than 0 (or has a
//   NaN end) records nothing, as rrect does for an empty rect; a stroke width that is not > 0 records no stroke.
// - areaStrip(): column k spans [xs[k], xs[k+1]] with the edges' samples k and k+1 at its ends, so neighbouring
//   columns share their x exactly and tile (the rasteriser's top-left rule gives each pixel to one of them). A
//   non-increasing pair records nothing for that column. Fewer than two samples, or a null xs/yTop, record nothing.
// - polyline(): n-1 capsule segments; a Debug assert requires an opaque colour (translucent capsules bead where their
//   ends overlap, A §2.6: premix() the ground instead).
// - dotted(): dots at x + k * step for every k with k * step < w (the half-open track [x, x + w)), each one device px
//   square at (snapX(x + k * step), snapY(y)). A step or width that is not > 0 draws nothing; at most kMaxDots dots.
// - axis(): recorded whatever the arguments, one AxisRec per call; a null map is an absent axis ("-" in the dump).

namespace funkgui
{
    namespace
    {
        // HR SdfCanvas.cpp's pack() (Canvas.cpp keeps its own copy): RGBA8 as r | g<<8 | b<<16 | a<<24.
        uint32_t pack(Col c) noexcept
        {
            return static_cast<uint32_t>(c.r)
                 | (static_cast<uint32_t>(c.g) << 8)
                 | (static_cast<uint32_t>(c.b) << 16)
                 | (static_cast<uint32_t>(c.a) << 24);
        }

        // The AA apron above and below an AREA column, as HR's rrect and segment pad (02 §4.1).
        constexpr float kApron = 1.5f;

        // A bound on dotted(): a track 10 000 logical px long at a 0.25 px step, far beyond any panel.
        constexpr int kMaxDots = 40000;

        uint32_t edgeFlags(AreaEdge edge) noexcept
        {
            switch (edge)
            {
                case AreaEdge::top:    return 0;
                case AreaEdge::bottom: return pflag::strokeBottom;
                case AreaEdge::both:   return pflag::strokeBoth;
            }
            return 0;
        }
    }

    void Canvas::area(float x0, float x1, float yTop0, float yTop1, float yBot0, float yBot1, Col fill, float strokeW,
                      Col stroke, AreaEdge edge)
    {
        if (!(x1 > x0))
            return;                                      // empty or inverted column (or a NaN end): nothing

        const float hs = strokeW > 0.0f ? strokeW * 0.5f : 0.0f;
        const float pad = kApron + hs;
        const float top = std::min(std::min(yTop0, yTop1), std::min(yBot0, yBot1)) - pad;
        const float bot = std::max(std::max(yTop0, yTop1), std::max(yBot0, yBot1)) + pad;
        const float cy = (top + bot) * 0.5f;             // the edges travel centre-relative (x needs no centre)
        const float hw = (x1 - x0) * 0.5f, hh = (bot - top) * 0.5f;

        Prim& p = emit(PrimKind::area);
        p.c0 = pack(fill);
        p.c1 = pack(stroke);
        p.x0 = x0;                                       // no x apron: columns tile exactly
        p.y0 = top;
        p.x1 = x1;
        p.y1 = bot;
        p.d0[0] = -hw;
        p.d0[1] = -hh;
        p.d0[2] = hw;
        p.d0[3] = hh;
        p.e0[0] = hw;
        p.e0[1] = hh;
        p.d1[0] = yTop0 - cy;
        p.d1[1] = yTop1 - cy;
        p.d1[2] = yBot0 - cy;
        p.d1[3] = yBot1 - cy;
        p.d2[0] = hs;
        p.d2[1] = 0.0f;
        // emit() wrote the kind and the live bit; the edge bits join them.
        p.d2[3] = static_cast<float>((live() ? pflag::live : 0u) | edgeFlags(edge));
    }

    void Canvas::areaStrip(const float* xs, int n, const float* yTop, const float* yBot, float yBase, Col fill,
                           float strokeW, Col stroke, AreaEdge edge)
    {
        if (xs == nullptr || yTop == nullptr || n < 2)
            return;
        for (int k = 0; k + 1 < n; ++k)
        {
            const float b0 = yBot != nullptr ? yBot[k] : yBase;
            const float b1 = yBot != nullptr ? yBot[k + 1] : yBase;
            area(xs[k], xs[k + 1], yTop[k], yTop[k + 1], b0, b1, fill, strokeW, stroke, edge);
        }
    }

    void Canvas::polyline(const float* xs, const float* ys, int n, float width, Col opaque)
    {
        // Overlapping capsule ends double-blend at any alpha below 1 (A §2.6); fades use premix(ground, c, a).
        assert(opaque.a == 255 && "polyline needs an opaque colour: premix() it over the ground");
        if (xs == nullptr || ys == nullptr || n < 2)
            return;
        for (int k = 0; k + 1 < n; ++k)
            segment(xs[k], ys[k], xs[k + 1], ys[k + 1], width, opaque);
    }

    void Canvas::disc(float cx, float cy, float r, Col fill, float ringW, Col ring)
    {
        // HR's ring trick (A §2.1): a square rrect whose radius is its half side; the shader clamps the radius.
        rrect(cx - r, cy - r, 2.0f * r, 2.0f * r, r, fill, ringW, ring);
    }

    void Canvas::dotted(float x, float y, float w, float step, Col c)
    {
        if (!(w > 0.0f) || !(step > 0.0f))
            return;
        const float px = 1.0f / dpi();                   // one device px
        const float yy = snapY(y);
        for (int k = 0; k < kMaxDots; ++k)
        {
            const float off = static_cast<float>(k) * step;
            if (!(off < w))
                break;
            rrect(snapX(x + off), yy, px, px, 0.0f, c);
        }
    }

    void Canvas::axis(Tag t, const AxisMap* x, const AxisMap* y)
    {
        AxisRec a;
        a.tag = t;
        a.hasX = x != nullptr;
        a.hasY = y != nullptr;
        if (x != nullptr)
            a.x = *x;
        if (y != nullptr)
            a.y = *y;
        list_.axes.push_back(a);
    }
}

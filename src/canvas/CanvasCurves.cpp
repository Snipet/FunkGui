#include <funkgui/canvas/Canvas.h>

#include <funkgui/core/Flatten.h>

#include <cmath>

// The recorder's curves (canvas/Canvas.h, "FSynth G1 additions"): an arc, a dotted arc and a cubic Bezier, each
// flattened by core/Flatten.h at the frame's dpi and recorded through polyline() as capsule segments. Nothing here is
// a new primitive: CanvasShapes.cpp's polyline() records the segments and asserts the opaque colour, so the dump, the
// fingerprint, SoftRaster, BgfxSink and WebGlSink see what they have always seen.
//
// The points are Flatten.h's, and so is the rule that keeps them the same on macOS, Linux and wasm32: the sine and
// the cosine of a dot's angle are taken in double and each rounded to float once; everything else is float + - * /.
// Choices where the header is silent:
// - A dot's angle is a0 + (k * stepPx) / r towards a1, in float. The dots are counted by arc length, not by angle, so
//   the same step gives the same spacing on every radius.
// - A zero-length capsule goes through polyline() as two equal points: one segment primitive whose ends coincide.

namespace funkgui
{
    namespace
    {
        constexpr int kMaxArcDots = 4096;                // a full turn at r 650 px and a 1 px step

        // A flattened curve as polyline()'s two arrays.
        void record(Canvas& c, const Point* pts, int n, float width, Col opaque)
        {
            float xs[geom::kMaxCurvePoints], ys[geom::kMaxCurvePoints];
            for (int i = 0; i < n; ++i)
            {
                xs[i] = pts[i].x;
                ys[i] = pts[i].y;
            }
            c.polyline(xs, ys, n, width, opaque);
        }
    }

    void Canvas::arc(Point centre, float r, float a0, float a1, float width, Col opaque)
    {
        if (!(r > 0.0f) || !(width > 0.0f))
            return;
        Point pts[geom::kMaxCurvePoints];
        const int n = geom::flattenArc(centre, r, a0, a1, dpi(), pts, geom::kMaxCurvePoints);
        record(*this, pts, n, width, opaque);
    }

    void Canvas::arcDotted(Point centre, float r, float a0, float a1, float stepPx, float dotW, Col opaque)
    {
        if (!(r > 0.0f) || !(stepPx > 0.0f) || !(dotW > 0.0f))
            return;
        const float length = r * std::fabs(a1 - a0);
        const float dir = a1 < a0 ? -1.0f : 1.0f;
        for (int k = 0; k < kMaxArcDots; ++k)
        {
            const float along = static_cast<float>(k) * stepPx;
            if (!(along < length))
                break;                                   // the half-open arc; a NaN length draws nothing
            const float a = a0 + dir * (along / r);
            const float x = centre.x + r * static_cast<float>(std::cos(static_cast<double>(a)));
            const float y = centre.y + r * static_cast<float>(std::sin(static_cast<double>(a)));
            const float xs[2] = { x, x }, ys[2] = { y, y };
            polyline(xs, ys, 2, dotW, opaque);
        }
    }

    void Canvas::cubic(const Point p[4], float width, Col opaque)
    {
        if (p == nullptr || !(width > 0.0f))
            return;
        Point pts[geom::kMaxCurvePoints];
        const int n = geom::flattenCubic(p, dpi(), pts, geom::kMaxCurvePoints);
        record(*this, pts, n, width, opaque);
    }
}

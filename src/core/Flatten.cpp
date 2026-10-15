#include <funkgui/core/Flatten.h>

#include <cmath>
#include <limits>

// Curve flattening and hit testing (core/Flatten.h; FSynth G1). The rules that keep one set of points on every
// platform are the header's: std::sin, std::cos and std::acos are called on a double and the result is rounded to
// float once; nothing else here calls a transcendental function, and no hypot, fma, pow or atan2 appears (their last
// bits differ between maths libraries, or they fuse a rounding away). Choices where the header is silent:
//
// - flattenCubic walks the pieces depth-first, left half first, on a fixed stack: a piece is written as its end point
//   when it is flat or at the depth limit, and is halved by de Casteljau otherwise. No recursion, no allocation. The
//   flatness test compares squared distances, so it takes no square root.
// - A NaN control point is never "within" a tolerance, so such a curve subdivides to the depth limit and writes its
//   NaNs out; the count stays in range.
// - arcSegments clamps 1 - e / r to -1 (a tolerance beyond the diameter: acos gives pi, and one chord may span a full
//   turn). A radius so large against the tolerance that 1 - e / r rounds to 1 needs more pieces than the cap, and
//   gets the cap.

namespace funkgui::geom
{
    namespace
    {
        constexpr int kMaxDepth = 7;                     // 2^7 = kMaxCurvePoints - 1 pieces
        static_assert((1 << kMaxDepth) == kMaxCurvePoints - 1);

        // The squared distance from q to the segment a-b (to the point a when the segment has no length).
        float distSqToSegment(Point a, Point b, Point q) noexcept
        {
            const float abx = b.x - a.x, aby = b.y - a.y;
            const float aqx = q.x - a.x, aqy = q.y - a.y;
            const float len2 = abx * abx + aby * aby;
            float t = 0.0f;
            if (len2 > 0.0f)
            {
                t = (aqx * abx + aqy * aby) / len2;
                t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            }
            const float dx = aqx - abx * t, dy = aqy - aby * t;
            return dx * dx + dy * dy;
        }

        Point mid(Point a, Point b) noexcept
        {
            return { (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f };
        }

        struct Piece
        {
            Point c[4];
            int   depth = 0;
        };
    }

    int flattenCubic(const Point p[4], float dpi, Point* out, int maxOut, float tolDevicePx) noexcept
    {
        if (p == nullptr || out == nullptr || maxOut < 2)
            return 0;
        const int limit = maxOut < kMaxCurvePoints ? maxOut : kMaxCurvePoints;
        int maxDepth = 0;                                // the largest depth whose 2^depth pieces fit limit points
        while (maxDepth < kMaxDepth && (2 << maxDepth) <= limit - 1)
            ++maxDepth;

        float tol2 = -1.0f;                              // no squared distance is <= -1: nothing is flat
        if (dpi > 0.0f && tolDevicePx > 0.0f)
        {
            const float tol = tolDevicePx / dpi;
            tol2 = tol * tol;
        }

        // Depth first, the left half next: at most one waiting right half per level, plus the piece in hand.
        Piece stack[kMaxDepth + 2];
        int top = 0;
        stack[top++] = Piece{ { p[0], p[1], p[2], p[3] }, 0 };
        int n = 0;
        out[n++] = p[0];
        while (top > 0)
        {
            const Piece cur = stack[--top];
            const Point* c = cur.c;
            const bool flat = distSqToSegment(c[0], c[3], c[1]) <= tol2 && distSqToSegment(c[0], c[3], c[2]) <= tol2;
            if (flat || cur.depth >= maxDepth)
            {
                out[n++] = c[3];                         // the piece as its chord
                continue;
            }
            const Point ab = mid(c[0], c[1]), bc = mid(c[1], c[2]), cd = mid(c[2], c[3]);
            const Point abc = mid(ab, bc), bcd = mid(bc, cd);
            const Point m = mid(abc, bcd);               // the curve at the piece's middle
            stack[top++] = Piece{ { m, bcd, cd, c[3] }, cur.depth + 1 };
            stack[top++] = Piece{ { c[0], ab, abc, m }, cur.depth + 1 };
        }
        return n;
    }

    int arcSegments(float r, float a0, float a1, float dpi, float tolDevicePx) noexcept
    {
        if (!(r > 0.0f) || !(dpi > 0.0f) || !(tolDevicePx > 0.0f))
            return 1;
        const float e = tolDevicePx / dpi;
        float c = 1.0f - e / r;
        if (!(c > -1.0f))
            c = -1.0f;
        // The one transcendental call: in double, rounded to float once.
        const float half = static_cast<float>(std::acos(static_cast<double>(c)));
        const float step = 2.0f * half;                  // the angle one chord may span
        const float sweep = std::fabs(a1 - a0);
        if (!(sweep > 0.0f))
            return 1;                                    // no sweep, or a NaN angle
        if (!(step > 0.0f))
            return kMaxCurvePoints - 1;
        const float n = std::ceil(sweep / step);
        if (!(n >= 1.0f))
            return 1;
        return n >= static_cast<float>(kMaxCurvePoints - 1) ? kMaxCurvePoints - 1 : static_cast<int>(n);
    }

    int flattenArc(Point centre, float r, float a0, float a1, float dpi, Point* out, int maxOut,
                   float tolDevicePx) noexcept
    {
        if (out == nullptr || maxOut < 2)
            return 0;
        int pieces = arcSegments(r, a0, a1, dpi, tolDevicePx);
        if (pieces > maxOut - 1)
            pieces = maxOut - 1;
        const float sweep = a1 - a0;
        for (int k = 0; k <= pieces; ++k)
        {
            const float a = k == pieces ? a1 : a0 + sweep * (static_cast<float>(k) / static_cast<float>(pieces));
            // In double, each rounded to float once (the header's rule).
            const float c = static_cast<float>(std::cos(static_cast<double>(a)));
            const float s = static_cast<float>(std::sin(static_cast<double>(a)));
            out[k] = { centre.x + r * c, centre.y + r * s };
        }
        return pieces + 1;
    }

    Point cubicAt(const Point p[4], float t) noexcept
    {
        const float u = 1.0f - t;
        const auto lerp = [u, t](Point a, Point b) -> Point { return { a.x * u + b.x * t, a.y * u + b.y * t }; };
        const Point ab = lerp(p[0], p[1]), bc = lerp(p[1], p[2]), cd = lerp(p[2], p[3]);
        return lerp(lerp(ab, bc), lerp(bc, cd));
    }

    float distToPolyline(const Point* pts, int n, Point p) noexcept
    {
        if (pts == nullptr || n < 1)
            return std::numeric_limits<float>::infinity();
        float best = distSqToSegment(pts[0], pts[0], p);
        for (int i = 0; i + 1 < n; ++i)
        {
            const float d = distSqToSegment(pts[i], pts[i + 1], p);
            if (d < best)
                best = d;
        }
        return std::sqrt(best);
    }
}

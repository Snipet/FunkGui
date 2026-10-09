// FUNKGUI_TEST name=fg.canvas.curves timeout=600 gpu=0
//
// fg.canvas.curves (FSynth G1): core/Flatten.h and Canvas::arc / arcDotted / cubic. Spec rows only; it includes no
// JUCE and runs in every configuration, wasm32 under node included.
//
// - cubic.*   flattenCubic over 64 cables on a 1120 x 640 panel (horizontal tangents, forwards and backwards, and
//             16 hanging ones): the end points are the control points bit for bit; at dpi 1, 2 and 2.5 every
//             flattened point lies on a 4096-step reference of the curve and every reference point lies within
//             0.6 / dpi px of the polyline (within 3/4 of it, the bound the control-point test gives); the piece counts
//             at the tolerances of FSynth research note H §2 Table 6 (0.5, 0.25 and 0.1 logical px) stay inside the
//             cap and are pinned, with the bits of every point, so every architecture flattens the same cables
//             (flattenCubic calls no maths library); the cap, a small buffer, a straight curve, control points beyond
//             the chord, NaN.
// - arc.*     arcSegments equals ceil(theta / (2 acos(1 - e / r))) (22 for 270 degrees at r 16 and e 0.1 px, H §2
//             "Rings") over a grid, and its clamps; flattenArc's points lie on the circle, start at a0, end at a1, and
//             no chord strays more than e from the arc.
// - at.*      cubicAt at 0, 1 and 0.5.
// - dist.*    distToPolyline against a dense sampling of the polyline.
// - same.*    the same call twice gives the same bits.
// - canvas.*  arc, arcDotted and cubic record only `segment` primitives, as many as Flatten.h says, in the colour,
//             width and tag asked for; a dot is a capsule of no length.

#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/Prim.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/core/Col.h>
#include <funkgui/core/Flatten.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/test/Harness.h>
#include <funkgui/text/FontService.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace T = funkgui::test;
namespace geom = funkgui::geom;
using funkgui::Canvas;
using funkgui::Col;
using funkgui::FrameInfo;
using funkgui::Point;
using funkgui::Prim;
using funkgui::PrimList;

namespace
{
    constexpr double kPi = 3.14159265358979323846;
    constexpr int    kCables = 64;

    struct Cable
    {
        Point p[4];
    };

    // 64 cables between ports on a 1120 x 640 panel, from a fixed generator: 48 with horizontal tangents (the handle
    // is half the horizontal span, at least 40 px, so a cable that runs backwards loops out and back), and 16 hanging
    // ones that sag by a quarter of their span (FSynth research note H §2 Table 6's two styles).
    std::vector<Cable> cables()
    {
        std::vector<Cable> out;
        uint32_t seed = 0x1234abcdu;
        const auto next = [&seed](int span) {
            seed = seed * 1664525u + 1013904223u;
            return static_cast<float>(static_cast<int>((seed >> 8) % static_cast<uint32_t>(span)));
        };
        for (int i = 0; i < kCables; ++i)
        {
            const float x0 = 24.0f + next(1072), y0 = 24.0f + next(592);
            const float x1 = 24.0f + next(1072), y1 = 24.0f + next(592);
            Cable c;
            if (i % 4 != 3)
            {
                const float reach = std::max(40.0f, std::fabs(x1 - x0) * 0.5f);
                c = { { { x0, y0 }, { x0 + reach, y0 }, { x1 - reach, y1 }, { x1, y1 } } };
            }
            else
            {
                const float dx = x1 - x0, dy = y1 - y0;
                const float sag = 0.25f * std::sqrt(dx * dx + dy * dy);
                c = { { { x0, y0 }, { x0 + dx / 3.0f, y0 + dy / 3.0f + sag },
                        { x0 + 2.0f * dx / 3.0f, y0 + 2.0f * dy / 3.0f + sag }, { x1, y1 } } };
            }
            out.push_back(c);
        }
        return out;
    }

    bool sameBits(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }
    bool samePoint(Point a, Point b) { return sameBits(a.x, b.x) && sameBits(a.y, b.y); }

    struct D
    {
        double x = 0.0, y = 0.0;
    };

    // The curve in double, for the reference.
    D curveAt(const Point p[4], double t)
    {
        const double u = 1.0 - t;
        const double b0 = u * u * u, b1 = 3.0 * u * u * t, b2 = 3.0 * u * t * t, b3 = t * t * t;
        return { b0 * static_cast<double>(p[0].x) + b1 * static_cast<double>(p[1].x) + b2 * static_cast<double>(p[2].x)
                     + b3 * static_cast<double>(p[3].x),
                 b0 * static_cast<double>(p[0].y) + b1 * static_cast<double>(p[1].y) + b2 * static_cast<double>(p[2].y)
                     + b3 * static_cast<double>(p[3].y) };
    }

    double distToSegment(D a, D b, D q)
    {
        const double abx = b.x - a.x, aby = b.y - a.y;
        const double len2 = abx * abx + aby * aby;
        double t = len2 > 0.0 ? ((q.x - a.x) * abx + (q.y - a.y) * aby) / len2 : 0.0;
        t = std::clamp(t, 0.0, 1.0);
        const double dx = q.x - (a.x + abx * t), dy = q.y - (a.y + aby * t);
        return std::sqrt(dx * dx + dy * dy);
    }

    D asD(Point p) { return { static_cast<double>(p.x), static_cast<double>(p.y) }; }

    // The largest distance of a 4096-step reference of the curve from the polyline, and of the polyline's points
    // from the reference. Both run in parameter order, so each search starts where the last one ended.
    struct Error
    {
        double refFromLine = 0.0, lineFromRef = 0.0;
    };

    Error flattenError(const Point p[4], const Point* pts, int n)
    {
        constexpr int kSteps = 4096;
        std::vector<D> ref(static_cast<size_t>(kSteps) + 1);
        for (int k = 0; k <= kSteps; ++k)
            ref[static_cast<size_t>(k)] = curveAt(p, static_cast<double>(k) / kSteps);
        Error e;
        for (const D& r : ref)
        {
            double best = std::numeric_limits<double>::infinity();
            for (int i = 0; i + 1 < n; ++i)
                best = std::min(best, distToSegment(asD(pts[i]), asD(pts[i + 1]), r));
            e.refFromLine = std::max(e.refFromLine, best);
        }
        for (int i = 0; i < n; ++i)
        {
            double best = std::numeric_limits<double>::infinity();
            for (int k = 0; k < kSteps; ++k)
                best = std::min(best, distToSegment(ref[static_cast<size_t>(k)], ref[static_cast<size_t>(k) + 1],
                                                    asD(pts[i])));
            e.lineFromRef = std::max(e.lineFromRef, best);
        }
        return e;
    }

    FrameInfo frame(float dpi)
    {
        FrameInfo f;
        f.logicalW = 320;
        f.logicalH = 200;
        f.dpi = dpi;
        f.clear = Col{ 10, 12, 14, 255 };
        f.fixedClock = true;
        return f;
    }

    int kindOf(const Prim& p) { return static_cast<int>(p.d2[2] + 0.5f); }

    uint32_t packed(Col c)
    {
        return static_cast<uint32_t>(c.r) | (static_cast<uint32_t>(c.g) << 8) | (static_cast<uint32_t>(c.b) << 16)
             | (static_cast<uint32_t>(c.a) << 24);
    }

    // Whether every primitive is a segment of this half width, colour and tag.
    bool allSegments(const PrimList& l, float halfWidth, Col c, funkgui::Tag tag)
    {
        return std::all_of(l.prims.begin(), l.prims.end(), [&](const Prim& p) {
            return kindOf(p) == static_cast<int>(funkgui::PrimKind::segment) && sameBits(p.d2[0], halfWidth)
                && p.c0 == packed(c) && p.tag == tag;
        });
    }

    std::string dpiKey(float dpi)
    {
        char buf[16];
        std::snprintf(buf, sizeof buf, "dpi%g", static_cast<double>(dpi));
        std::string s(buf);
        std::replace(s.begin(), s.end(), '.', '_');
        return s;
    }
}

int main(int argc, char** argv)
{
    const funkgui::HeadlessGuiScope gui;                 // JUCE's GUI side, when there is JUCE: the atlas bakes there
    T::Probe P("fg.canvas.curves", "", argc, argv);
    const std::vector<Cable> set = cables();

    // ---- cubic: end points, and the error against a reference at three densities -----------------------------------
    {
        bool endsExact = true, inRange = true;
        for (const float dpi : { 1.0f, 2.0f, 2.5f })
        {
            const double tol = static_cast<double>(geom::kFlattenDevicePx) / static_cast<double>(dpi);
            double worstRef = 0.0, worstLine = 0.0;
            for (const Cable& c : set)
            {
                Point pts[geom::kMaxCurvePoints];
                const int n = geom::flattenCubic(c.p, dpi, pts, geom::kMaxCurvePoints);
                inRange = inRange && n >= 2 && n <= geom::kMaxCurvePoints;
                if (n < 2)
                    continue;
                endsExact = endsExact && samePoint(pts[0], c.p[0]) && samePoint(pts[n - 1], c.p[3]);
                const Error e = flattenError(c.p, pts, n);
                worstRef = std::max(worstRef, e.refFromLine);
                worstLine = std::max(worstLine, e.lineFromRef);
            }
            const std::string k = "cubic." + dpiKey(dpi);
            std::printf("NOTE     %s: tolerance %.4f px, the curve strays %.4f px from the polyline (%.3f of it), the "
                        "polyline's points %.6f px from the curve\n",
                        k.c_str(), tol, worstRef, worstRef / tol, worstLine);
            // Every reference point within the tolerance, and within 3/4 of it: the bound the control points give.
            P.le(k + ".curve_within_tolerance", worstRef, tol);
            P.le(k + ".curve_within_three_quarters", worstRef, 0.75 * tol + 1.0e-4);
            // Every flattened point on the curve (float rounding, and the reference's own step).
            P.le(k + ".points_on_curve", worstLine, 1.0e-3);
        }
        P.eq("cubic.ends_exact", endsExact, 1);
        P.eq("cubic.count_in_range", inRange, 1);
    }

    // ---- cubic: piece counts at FSynth research note H §2 Table 6's tolerances ---------------------------------------
    // The table's own 64 cables are not kept, so its figures (a mean of 19.9 pieces and at most 34 at 0.5 px, 28.0 and
    // 48 at 0.25, 44.0 and 78 at 0.1) are a scale, not an expectation: this set's means stay within 30 % of them (it
    // measures 21.4 and 36, 29.3 and 50, 46.6 and 76), every count stays inside the cap, and the totals and the bits
    // of every point are pinned, so that every architecture flattens the same cables the same way.
    {
        struct Case { const char* key; float tolPx; double meanLo, meanHi; long long total; };
        static constexpr Case cases[] = {
            { "tol0_5", 0.5f, 19.9 * 0.7, 19.9 * 1.3, 1371 },
            { "tol0_25", 0.25f, 28.0 * 0.7, 28.0 * 1.3, 1878 },
            { "tol0_1", 0.1f, 44.0 * 0.7, 44.0 * 1.3, 2981 },
        };
        std::vector<float> bits;
        for (const Case& k : cases)
        {
            long long total = 0;
            int most = 0;
            for (const Cable& c : set)
            {
                Point pts[geom::kMaxCurvePoints];
                const int n = geom::flattenCubic(c.p, 1.0f, pts, geom::kMaxCurvePoints, k.tolPx);
                total += n - 1;
                most = std::max(most, n - 1);
                for (int i = 0; i < n; ++i)
                {
                    bits.push_back(pts[i].x);
                    bits.push_back(pts[i].y);
                }
            }
            const double mean = static_cast<double>(total) / kCables;
            std::printf("NOTE     cubic.%s: %lld pieces over %d cables, mean %.2f, most %d\n", k.key, total, kCables,
                        mean, most);
            const std::string key = std::string("cubic.pieces.") + k.key;
            P.in(key + ".mean", mean, k.meanLo, k.meanHi);
            P.le(key + ".most", most, geom::kMaxCurvePoints - 1);
            P.eq(key + ".total", total, k.total);
        }
        const uint64_t hash = T::hashFloats(bits);
        std::printf("NOTE     cubic.points.hash %016llx over %zu floats\n", static_cast<unsigned long long>(hash),
                    bits.size());
        P.eq("cubic.points.hash", static_cast<int64_t>(hash), static_cast<int64_t>(0x9f6f2dc238bf0bebull));
    }

    // ---- cubic: the cap, small buffers, degenerate curves -----------------------------------------------------------
    {
        const Point wild[4] = { { 0.0f, 0.0f }, { 4000.0f, 3000.0f }, { -3000.0f, 3000.0f }, { 1000.0f, 0.0f } };
        Point pts[geom::kMaxCurvePoints + 8];
        for (Point& q : pts)
            q = { -7.0f, -7.0f };
        const int capped = geom::flattenCubic(wild, 4.0f, pts, geom::kMaxCurvePoints + 8, 0.01f);
        P.eq("cubic.cap.points", capped, geom::kMaxCurvePoints);
        P.eq("cubic.cap.ends", samePoint(pts[0], wild[0]) && samePoint(pts[capped - 1], wild[3]), 1);
        P.eq("cubic.cap.nothing_beyond", samePoint(pts[geom::kMaxCurvePoints], Point{ -7.0f, -7.0f }), 1);

        // A buffer of m points takes the deepest subdivision whose pieces all fit: 2^floor(log2(m - 1)) + 1 points.
        static constexpr int sizes[][2] = { { 2, 2 }, { 3, 3 }, { 4, 3 }, { 5, 5 }, { 8, 5 }, { 9, 9 }, { 64, 33 },
                                            { 128, 65 }, { 129, 129 } };
        bool fits = true, ends = true;
        for (const auto& sz : sizes)
        {
            const int n = geom::flattenCubic(wild, 4.0f, pts, sz[0], 0.01f);
            fits = fits && n == sz[1];
            ends = ends && n >= 2 && samePoint(pts[0], wild[0]) && samePoint(pts[n - 1], wild[3]);
        }
        P.eq("cubic.small_buffer.counts", fits, 1);
        P.eq("cubic.small_buffer.ends", ends, 1);
        P.eq("cubic.no_buffer", geom::flattenCubic(wild, 1.0f, nullptr, 16), 0);
        P.eq("cubic.buffer_of_one", geom::flattenCubic(wild, 1.0f, pts, 1), 0);
        P.eq("cubic.no_curve", geom::flattenCubic(nullptr, 1.0f, pts, 16), 0);

        // Control points on the chord: one piece. Beyond its ends: the curve overshoots the chord, and the distance to
        // the chord as a segment sees it.
        const Point straight[4] = { { 10.0f, 10.0f }, { 40.0f, 25.0f }, { 70.0f, 40.0f }, { 100.0f, 55.0f } };
        P.eq("cubic.straight.points", geom::flattenCubic(straight, 2.0f, pts, geom::kMaxCurvePoints), 2);
        // (all four points on one line: their distance to the chord's LINE is 0, and the curve still runs to x = 237.5)
        const Point beyond[4] = { { 0.0f, 0.0f }, { 300.0f, 0.0f }, { 300.0f, 0.0f }, { 100.0f, 0.0f } };
        const int nb = geom::flattenCubic(beyond, 1.0f, pts, geom::kMaxCurvePoints);
        float reach = 0.0f;
        for (int i = 0; i < nb; ++i)
            reach = std::max(reach, pts[i].x);
        P.ge("cubic.overshoot.points", nb, 3);
        P.ge("cubic.overshoot.reach", static_cast<double>(reach), 237.0);
        const Point closed[4] = { { 50.0f, 50.0f }, { 150.0f, 0.0f }, { 150.0f, 100.0f }, { 50.0f, 50.0f } };
        P.ge("cubic.closed_loop.points", geom::flattenCubic(closed, 1.0f, pts, geom::kMaxCurvePoints), 9);

        // No usable tolerance: to the depth limit, and the count stays in range.
        const float nan = std::numeric_limits<float>::quiet_NaN();
        P.eq("cubic.dpi_zero.points", geom::flattenCubic(straight, 0.0f, pts, geom::kMaxCurvePoints),
             geom::kMaxCurvePoints);
        P.eq("cubic.dpi_nan.points", geom::flattenCubic(straight, nan, pts, geom::kMaxCurvePoints),
             geom::kMaxCurvePoints);
        const Point bad[4] = { { 0.0f, 0.0f }, { nan, 10.0f }, { 20.0f, 10.0f }, { 30.0f, 0.0f } };
        P.eq("cubic.nan_point.points", geom::flattenCubic(bad, 1.0f, pts, geom::kMaxCurvePoints),
             geom::kMaxCurvePoints);
    }

    // ---- arcs -------------------------------------------------------------------------------------------------------
    {
        const float turn270 = static_cast<float>(1.5 * kPi);
        // H §2 "Rings": 22 pieces for 270 degrees at r = 16 px, e = 0.1 px.
        P.eq("arc.segments.r16_270deg_e0_1", geom::arcSegments(16.0f, 0.0f, turn270, 1.0f, 0.1f), 22);
        P.eq("arc.segments.backwards_is_the_same", geom::arcSegments(16.0f, turn270, 0.0f, 1.0f, 0.1f), 22);
        // The dpi turns the device tolerance into logical px: 0.2 device px at dpi 2 is 0.1 px.
        P.eq("arc.segments.dpi_scales_tolerance", geom::arcSegments(16.0f, 0.0f, turn270, 2.0f, 0.2f), 22);

        // ceil(theta / (2 acos(1 - e / r))) over a grid; a quotient within 1e-4 of a whole number is not judged (the
        // float and the double round it to either side).
        int judged = 0, wrong = 0;
        for (const float r : { 5.5f, 11.0f, 15.0f, 16.0f, 20.0f, 37.5f, 120.0f, 640.0f })
            for (const float deg : { 15.0f, 90.0f, 133.0f, 270.0f, 360.0f, 720.0f })
                for (const float dpi : { 1.0f, 1.25f, 2.0f, 2.5f, 3.5f })
                {
                    const float sweep = static_cast<float>(static_cast<double>(deg) * kPi / 180.0);
                    const double e = static_cast<double>(geom::kFlattenDevicePx) / static_cast<double>(dpi);
                    const double q = static_cast<double>(sweep) / (2.0 * std::acos(1.0 - e / static_cast<double>(r)));
                    if (std::fabs(q - std::round(q)) < 1.0e-4)
                        continue;
                    const int want = std::clamp(static_cast<int>(std::ceil(q)), 1, geom::kMaxCurvePoints - 1);
                    ++judged;
                    if (geom::arcSegments(r, 0.25f, 0.25f + sweep, dpi) != want)
                    {
                        ++wrong;
                        std::printf("NOTE     arcSegments(r %g, %g deg, dpi %g) = %d, want %d\n",
                                    static_cast<double>(r), static_cast<double>(deg), static_cast<double>(dpi),
                                    geom::arcSegments(r, 0.25f, 0.25f + sweep, dpi), want);
                    }
                }
        P.ge("arc.segments.grid.judged", judged, 200);
        P.eq("arc.segments.grid.wrong", wrong, 0);

        const float nan = std::numeric_limits<float>::quiet_NaN();
        P.eq("arc.segments.no_sweep", geom::arcSegments(16.0f, 1.0f, 1.0f, 2.0f), 1);
        P.eq("arc.segments.no_radius", geom::arcSegments(0.0f, 0.0f, 3.0f, 2.0f), 1);
        P.eq("arc.segments.no_dpi", geom::arcSegments(16.0f, 0.0f, 3.0f, 0.0f), 1);
        P.eq("arc.segments.nan_angle", geom::arcSegments(16.0f, 0.0f, nan, 2.0f), 1);
        P.eq("arc.segments.coarse_tolerance", geom::arcSegments(4.0f, 0.0f, 3.0f, 1.0f, 100.0f), 1);
        P.eq("arc.segments.cap", geom::arcSegments(100000.0f, 0.0f, 600.0f, 4.0f, 0.001f),
             geom::kMaxCurvePoints - 1);

        // flattenArc: arcSegments + 1 points on the circle, from a0 to a1, no chord further than e from the arc.
        bool counts = true, starts = true, endsAt = true;
        double offCircle = 0.0, chordError = 0.0;
        for (const float r : { 11.0f, 15.0f, 20.0f, 64.0f })
            for (const float dpi : { 1.0f, 2.0f, 2.5f })
            {
                const Point centre{ 100.25f, 80.5f };
                const float a0 = static_cast<float>(0.75 * kPi), a1 = a0 + turn270;
                Point pts[geom::kMaxCurvePoints];
                const int n = geom::flattenArc(centre, r, a0, a1, dpi, pts, geom::kMaxCurvePoints);
                counts = counts && n == geom::arcSegments(r, a0, a1, dpi) + 1;
                const auto at = [&](float a) {
                    return Point{ centre.x + r * static_cast<float>(std::cos(static_cast<double>(a))),
                                  centre.y + r * static_cast<float>(std::sin(static_cast<double>(a))) };
                };
                starts = starts && samePoint(pts[0], at(a0));
                endsAt = endsAt && n >= 2 && samePoint(pts[n - 1], at(a1));
                const double e = static_cast<double>(geom::kFlattenDevicePx) / static_cast<double>(dpi);
                for (int i = 0; i < n; ++i)
                {
                    const D c = asD(centre), a = asD(pts[i]);
                    const double radius = static_cast<double>(r);
                    offCircle = std::max(offCircle, std::fabs(distToSegment(c, c, a) - radius));
                    if (i + 1 < n)
                    {
                        const D b = asD(pts[i + 1]);
                        const D mid{ 0.5 * (a.x + b.x), 0.5 * (a.y + b.y) };       // a chord is furthest at its middle
                        chordError = std::max(chordError, (radius - distToSegment(c, c, mid)) / e);
                    }
                }
            }
        P.eq("arc.flatten.counts", counts, 1);
        P.eq("arc.flatten.starts_at_a0", starts, 1);
        P.eq("arc.flatten.ends_at_a1", endsAt, 1);
        P.le("arc.flatten.on_circle", offCircle, 1.0e-4);
        P.le("arc.flatten.chord_error_over_tolerance", chordError, 1.0 + 1.0e-3);

        // Down the screen: y grows with the angle, so a quarter turn from 0 goes from +x to +y.
        Point q[geom::kMaxCurvePoints];
        const int nq = geom::flattenArc({ 0.0f, 0.0f }, 10.0f, 0.0f, static_cast<float>(0.5 * kPi), 2.0f, q,
                                        geom::kMaxCurvePoints);
        P.near("arc.clockwise.start_x", static_cast<double>(q[0].x), 10.0, 1.0e-6);
        P.near("arc.clockwise.end_y", static_cast<double>(q[nq - 1].y), 10.0, 1.0e-6);
        P.ge("arc.clockwise.middle_y", static_cast<double>(q[nq / 2].y), 1.0);

        // A buffer too small: fewer chords over the same arc.
        Point few[4];
        const int nf = geom::flattenArc({ 0.0f, 0.0f }, 50.0f, 0.0f, turn270, 2.0f, few, 4);
        P.eq("arc.small_buffer.points", nf, 4);
        P.near("arc.small_buffer.ends_at_a1_x", static_cast<double>(few[3].x), 50.0 * std::cos(1.5 * kPi), 1.0e-4);
        P.near("arc.small_buffer.ends_at_a1_y", static_cast<double>(few[3].y), 50.0 * std::sin(1.5 * kPi), 1.0e-4);
        P.eq("arc.no_buffer", geom::flattenArc({ 0.0f, 0.0f }, 50.0f, 0.0f, 1.0f, 2.0f, nullptr, 8), 0);
        P.eq("arc.buffer_of_one", geom::flattenArc({ 0.0f, 0.0f }, 50.0f, 0.0f, 1.0f, 2.0f, few, 1), 0);
    }

    // ---- cubicAt ----------------------------------------------------------------------------------------------------
    {
        bool ends = true;
        double mid = 0.0;
        for (const Cable& c : set)
        {
            const Point a = geom::cubicAt(c.p, 0.0f), b = geom::cubicAt(c.p, 1.0f), m = geom::cubicAt(c.p, 0.5f);
            ends = ends && a.x == c.p[0].x && a.y == c.p[0].y && b.x == c.p[3].x && b.y == c.p[3].y;
            const D want = curveAt(c.p, 0.5);
            mid = std::max(mid, std::max(std::fabs(static_cast<double>(m.x) - want.x),
                                         std::fabs(static_cast<double>(m.y) - want.y)));
        }
        P.eq("at.ends_exact", ends, 1);
        P.le("at.half", mid, 1.0e-3);
    }

    // ---- distToPolyline against a dense sampling --------------------------------------------------------------------
    {
        uint32_t seed = 0x51f15eedu;
        const auto rnd = [&seed](float lo, float hi) {
            seed = seed * 1664525u + 1013904223u;
            return lo + (hi - lo) * (static_cast<float>(seed >> 8) / 16777216.0f);
        };
        double worst = 0.0;
        for (int trial = 0; trial < 40; ++trial)
        {
            Point line[6];
            const int n = 2 + trial % 5;
            for (int i = 0; i < n; ++i)
                line[i] = { rnd(0.0f, 200.0f), rnd(0.0f, 200.0f) };
            for (int k = 0; k < 8; ++k)
            {
                const Point q{ rnd(-50.0f, 250.0f), rnd(-50.0f, 250.0f) };
                // Samples no further apart than 0.05 px (a segment is at most 283 px long), so the sampled minimum is
                // within 0.025 px of the true distance.
                constexpr int kSamples = 6000;
                double brute = std::numeric_limits<double>::infinity();
                for (int i = 0; i + 1 < n; ++i)
                    for (int s = 0; s <= kSamples; ++s)
                    {
                        const double t = static_cast<double>(s) / kSamples;
                        const double x = static_cast<double>(line[i].x)
                                       + (static_cast<double>(line[i + 1].x) - static_cast<double>(line[i].x)) * t;
                        const double y = static_cast<double>(line[i].y)
                                       + (static_cast<double>(line[i + 1].y) - static_cast<double>(line[i].y)) * t;
                        const double dx = x - static_cast<double>(q.x), dy = y - static_cast<double>(q.y);
                        brute = std::min(brute, std::sqrt(dx * dx + dy * dy));
                    }
                worst = std::max(worst, std::fabs(static_cast<double>(geom::distToPolyline(line, n, q)) - brute));
            }
        }
        P.le("dist.against_sampling", worst, 0.03);

        const Point one[1] = { { 3.0f, 4.0f } };
        P.near("dist.one_point", static_cast<double>(geom::distToPolyline(one, 1, { 0.0f, 0.0f })), 5.0, 1.0e-6);
        const Point seg[2] = { { 0.0f, 0.0f }, { 10.0f, 0.0f } };
        P.near("dist.on_the_line", static_cast<double>(geom::distToPolyline(seg, 2, { 4.0f, 0.0f })), 0.0, 0.0);
        P.near("dist.beside", static_cast<double>(geom::distToPolyline(seg, 2, { 4.0f, -3.0f })), 3.0, 1.0e-6);
        P.near("dist.past_the_end", static_cast<double>(geom::distToPolyline(seg, 2, { 13.0f, 4.0f })), 5.0, 1.0e-6);
        P.eq("dist.no_points", std::isinf(geom::distToPolyline(seg, 0, { 0.0f, 0.0f })), 1);
        P.eq("dist.null", std::isinf(geom::distToPolyline(nullptr, 3, { 0.0f, 0.0f })), 1);
    }

    // ---- the same call twice: the same bits -------------------------------------------------------------------------
    {
        std::vector<float> a, b;
        for (std::vector<float>* out : { &a, &b })
            for (const Cable& c : set)
            {
                Point pts[geom::kMaxCurvePoints];
                int n = geom::flattenCubic(c.p, 2.5f, pts, geom::kMaxCurvePoints);
                for (int i = 0; i < n; ++i)
                {
                    out->push_back(pts[i].x);
                    out->push_back(pts[i].y);
                }
                n = geom::flattenArc(c.p[0], 15.0f, c.p[1].x * 0.01f, c.p[2].y * 0.01f, 2.5f, pts,
                                     geom::kMaxCurvePoints);
                for (int i = 0; i < n; ++i)
                {
                    out->push_back(pts[i].x);
                    out->push_back(pts[i].y);
                }
                out->push_back(geom::distToPolyline(pts, n, c.p[3]));
                out->push_back(geom::cubicAt(c.p, 0.3f).x);
            }
        P.eq("same.sizes", a.size() == b.size() && !a.empty(), 1);
        P.eq("same.bits", T::hashFloats(a) == T::hashFloats(b), 1);
    }

    // ---- Canvas: only segments, as many as Flatten.h says -----------------------------------------------------------
    {
        auto& fonts = funkgui::FontService::get();
        const funkgui::FontAtlasSdf& atlas = fonts.atlas();
        if (!P.eq("font.ok", fonts.ok(), 1))
            return P.finish();
        Canvas canvas(atlas);
        const Col ink{ 200, 120, 40, 255 };
        const float turn270 = static_cast<float>(1.5 * kPi), a0 = static_cast<float>(0.75 * kPi);
        const Point centre{ 60.0f, 60.0f };
        const Point cable[4] = { { 20.0f, 150.0f }, { 120.0f, 150.0f }, { 180.0f, 40.0f }, { 280.0f, 40.0f } };

        for (const float dpi : { 1.0f, 2.0f, 2.5f })
        {
            const std::string k = "canvas." + dpiKey(dpi);
            canvas.begin(frame(dpi));
            canvas.setTag(funkgui::tags::firstProduct + 3);
            canvas.arc(centre, 20.0f, a0, a0 + turn270, 2.0f, ink);
            const PrimList arc = canvas.end();
            P.eq(k + ".arc.count", static_cast<int64_t>(arc.prims.size()),
                 geom::arcSegments(20.0f, a0, a0 + turn270, dpi));
            P.eq(k + ".arc.only_segments", allSegments(arc, 1.0f, ink, funkgui::tags::firstProduct + 3), 1);

            canvas.begin(frame(dpi));
            canvas.setTag(funkgui::tags::none);              // the tag is the Canvas's until changed, across frames
            canvas.cubic(cable, 2.5f, ink);
            const PrimList cubic = canvas.end();
            Point pts[geom::kMaxCurvePoints];
            const int n = geom::flattenCubic(cable, dpi, pts, geom::kMaxCurvePoints);
            P.eq(k + ".cubic.count", static_cast<int64_t>(cubic.prims.size()), n - 1);
            P.eq(k + ".cubic.only_segments", allSegments(cubic, 1.25f, ink, 0), 1);
            // The run starts and ends on the control points: a segment's ends travel relative to its quad's centre.
            bool joined = !cubic.prims.empty();
            if (joined)
            {
                const Prim& first = cubic.prims.front();
                const Prim& last = cubic.prims.back();
                const float fx = (first.x0 + first.x1) * 0.5f + first.d1[0];
                const float fy = (first.y0 + first.y1) * 0.5f + first.d1[1];
                const float lx = (last.x0 + last.x1) * 0.5f + last.d1[2];
                const float ly = (last.y0 + last.y1) * 0.5f + last.d1[3];
                joined = std::fabs(fx - cable[0].x) < 1.0e-3f && std::fabs(fy - cable[0].y) < 1.0e-3f
                      && std::fabs(lx - cable[3].x) < 1.0e-3f && std::fabs(ly - cable[3].y) < 1.0e-3f;
            }
            P.eq(k + ".cubic.ends_on_control_points", joined, 1);
        }

        // A dotted arc: a dot every 4 px of a quarter turn at r 40 (62.83 px), from a0: 16 dots (0, 4, ..., 60).
        canvas.begin(frame(2.0f));
        canvas.arcDotted(centre, 40.0f, 0.0f, static_cast<float>(0.5 * kPi), 4.0f, 1.5f, ink);
        const PrimList dots = canvas.end();
        P.eq("canvas.dotted.count", static_cast<int64_t>(dots.prims.size()), 16);
        P.eq("canvas.dotted.only_segments", allSegments(dots, 0.75f, ink, 0), 1);
        const bool zeroLength = std::all_of(dots.prims.begin(), dots.prims.end(), [](const Prim& p) {
            return sameBits(p.d1[0], p.d1[2]) && sameBits(p.d1[1], p.d1[3]);
        });
        P.eq("canvas.dotted.zero_length_capsules", zeroLength, 1);
        if (!dots.prims.empty())
        {
            // The first dot sits at a0, on +x; the dots run towards a1, down the screen.
            const Prim& d = dots.prims.front();
            P.near("canvas.dotted.first_x", static_cast<double>((d.x0 + d.x1) * 0.5f), 100.0, 1.0e-4);
            P.near("canvas.dotted.first_y", static_cast<double>((d.y0 + d.y1) * 0.5f), 60.0, 1.0e-4);
            const Prim& e = dots.prims.back();
            P.ge("canvas.dotted.last_below", static_cast<double>((e.y0 + e.y1) * 0.5f), 90.0);
        }
        // A step that divides the length exactly: the end is not dotted (the half-open arc). r 32, a sweep of 1
        // radian, a step of 8 px: dots at 0, 8, 16, 24.
        canvas.begin(frame(2.0f));
        canvas.arcDotted(centre, 32.0f, 2.0f, 1.0f, 8.0f, 1.5f, ink);
        P.eq("canvas.dotted.half_open", static_cast<int64_t>(canvas.end().prims.size()), 4);

        // Nothing for no radius, no width, no step, no curve; a dot for no sweep.
        canvas.begin(frame(2.0f));
        canvas.arc(centre, 0.0f, 0.0f, 1.0f, 2.0f, ink);
        canvas.arc(centre, 20.0f, 0.0f, 1.0f, 0.0f, ink);
        canvas.arcDotted(centre, 20.0f, 0.0f, 1.0f, 0.0f, 1.5f, ink);
        canvas.arcDotted(centre, 20.0f, 0.0f, 1.0f, 4.0f, 0.0f, ink);
        canvas.arcDotted(centre, 0.0f, 0.0f, 1.0f, 4.0f, 1.5f, ink);
        canvas.arcDotted(centre, 20.0f, 1.0f, 1.0f, 4.0f, 1.5f, ink);
        canvas.cubic(nullptr, 2.0f, ink);
        canvas.cubic(cable, 0.0f, ink);
        P.eq("canvas.nothing_recorded", static_cast<int64_t>(canvas.end().prims.size()), 0);
        canvas.begin(frame(2.0f));
        canvas.arc(centre, 20.0f, 1.0f, 1.0f, 2.0f, ink);
        const PrimList dot = canvas.end();
        P.eq("canvas.arc.no_sweep_is_one_dot", dot.prims.size() == 1 && sameBits(dot.prims[0].d1[0], dot.prims[0].d1[2])
                                                  && sameBits(dot.prims[0].d1[1], dot.prims[0].d1[3]), 1);
    }

    return P.finish();
}

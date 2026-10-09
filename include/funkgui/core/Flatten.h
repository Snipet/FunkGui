#pragma once

// Curve flattening and hit testing for the recorder and for products that cache flattened curves (a patch cable is
// flattened once per edge and redrawn from the cache). FSynth G1.
//
// Pure functions of their arguments: no state, no allocation. Angles are radians, 0 = +x, increasing clockwise on
// screen (y grows down). Tolerances are device px, turned into logical px by the frame's dpi, so a curve is exactly as
// smooth at every zoom and backing scale.
//
// One set of points on every platform: sines, cosines and the arc cosine are taken in double and each result is
// rounded to float once; everything else is + - * / in float and the exactly rounded sqrt, fabs and ceil. So macOS,
// Linux and wasm32 record the same points, and a golden row that hashes them is one row for all of them (a last-bit
// difference between two maths libraries stays far below a fingerprint's quantum of 1/1024 px). FunkGui's own targets
// build with -ffp-contract=off; a consumer that lets its compiler fuse a * b + c may see other last bits.
//
// Linux, the JUCE-free builds and a web host: the same source and the same points (platform-neutral, <cmath> only).

#include <funkgui/core/Geometry.h>

namespace funkgui::geom
{
    inline constexpr float kFlattenDevicePx = 0.6f;   // the largest chord error of a flattened curve, in device px
    inline constexpr int   kMaxCurvePoints  = 129;    // a flattened curve never exceeds 128 pieces

    // Recursive subdivision of the cubic Bezier p[0..3], halving a piece until both of its control points lie within
    // tolDevicePx / dpi logical px of its chord (the distance to the chord as a segment, so a control point beyond an
    // end of the chord counts by its distance to that end). The curve's own distance from the polyline is at most 3/4
    // of that. Points are written in order: out[0] == p[0] and out[n - 1] == p[3] exactly, every point between lies on
    // the curve at a dyadic parameter.
    // Returns n, the points written: 2 .. min(maxOut, kMaxCurvePoints). The depth of the subdivision is the largest
    // one whose pieces all fit, floor(log2(min(maxOut, kMaxCurvePoints) - 1)), which is 7 for kMaxCurvePoints; a piece
    // at that depth is a chord whatever its error. A dpi or a tolerance that is not > 0 (or is NaN) subdivides to that
    // depth. Returns 0, and writes nothing, when out is null or maxOut < 2.
    int   flattenCubic(const Point p[4], float dpi, Point* out, int maxOut,
                       float tolDevicePx = kFlattenDevicePx) noexcept;

    // The pieces an arc of radius r from a0 to a1 needs: ceil(|a1 - a0| / (2 acos(1 - e / r))) with
    // e = tolDevicePx / dpi, the largest distance of a chord from its arc; at least 1, at most kMaxCurvePoints - 1.
    // 1 when r, dpi or the tolerance is not > 0.
    int   arcSegments(float r, float a0, float a1, float dpi, float tolDevicePx = kFlattenDevicePx) noexcept;

    // The arc's arcSegments + 1 points, evenly spaced in angle, from a0 to a1: out[0] is at a0 and the last point at
    // a1. Returns the count written. A buffer too small for them takes maxOut points (fewer, longer chords over the
    // same arc); returns 0, and writes nothing, when out is null or maxOut < 2.
    int   flattenArc(Point centre, float r, float a0, float a1, float dpi, Point* out, int maxOut,
                     float tolDevicePx = kFlattenDevicePx) noexcept;

    // The point of the cubic at t (de Casteljau; p[0] at t = 0 and p[3] at t = 1 exactly): a cable's handle at 0.5.
    Point cubicAt(const Point p[4], float t) noexcept;

    // The shortest distance from p to the polyline pts[0..n-1], in logical px; n >= 1 (one point: the distance to it).
    // +infinity when pts is null or n < 1.
    float distToPolyline(const Point* pts, int n, Point p) noexcept;
}

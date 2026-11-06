#pragma once

// Whether two recorded frames draw the same picture (0.25.0): EditorHost's safety net for unchanged frames
// (gpu/EditorHost.h "Unchanged frames"), which skips the submit of a frame whose recording equals the last one it drew,
// and a test's way to say that two recordings would look alike. Plain C++, no GPU and no JUCE.
//
// Compared, exactly (floats by their bits, so a NaN equals itself and 0 differs from -0):
// - FrameInfo: logicalW, logicalH, dpi, clear and textGamma; seconds too when either frame has a shader draw (it is
//   their u_viewSize.w, which a custom shader may animate with);
// - every primitive, byte for byte (its tag included: tags are the only bytes that do not reach the GPU, and comparing
//   them costs nothing);
// - every shader draw's id, at, rect, clip, clipped and 16 params;
// - every image draw's id, at, shaderAt, dst, uv, radius, opacity, clip and clipped.
// Not compared, because no sink draws them: the rest of FrameInfo (frame, dt, fps, the clock and rate, theme,
// overflows), axes, the missing-glyph and missing-image counts, and the tags and live flags of shader and image draws.
//
// What a frame draws also depends on what it names: the pixels of each image id (canvas/Images.h) and the program of
// each shader id (gpu/ShaderLibrary.h), the font atlas and the surface it is drawn into. Two equal recordings draw
// the same picture only while those are unchanged; EditorHost tracks them itself.

#include <funkgui/canvas/PrimList.h>

namespace funkgui
{
    bool sameDrawing(const PrimList& a, const PrimList& b) noexcept;
}

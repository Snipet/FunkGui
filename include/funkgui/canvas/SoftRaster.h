#pragma once

// The CPU rasteriser (02 §5.10): fs_ui.sc's four kinds evaluated on the CPU (the FrameRender maths, now a library), so
// an agent can look at a frame with no GPU (HeadlessHost::writePng, `fcmp_probe_plugin ui.dump --png`). The shader and
// this mirror change together (02 §4.3, A §2.6 #4).
//
// Declared in G2 (v0.2.0, frozen at FZ1); implemented by G4 (src/canvas/SoftRaster.cpp).

#include <funkgui/canvas/PrimList.h>
#include <funkgui/text/FontAtlasSdf.h>

#include <cstdint>
#include <vector>

namespace funkgui
{
    struct Image
    {
        int w = 0, h = 0;                        // physical pixels
        std::vector<uint8_t> rgba;               // w * h * 4, row-major from the top-left, straight alpha
    };

    // v0.19.0, the stated fallback for shader draws (Canvas::shader, PrimList::shaders): there is no CPU rendering of a
    // host's fragment shader, so rasterise() draws this placeholder in each one's place in the order (over what was
    // recorded before it, under what was recorded after it), and a PNG shows where the shader sits. It covers the
    // samples whose centres lie in the half-open rect ∩ clip (a primitive's rule) and is opaque, with hard edges. In
    // the rect's own logical px (lx, ly from its top-left corner) a sample within one device px (1 / dpi) of the
    // rect's edge is `border`; else one with fmod(lx + ly, stripePeriod) < stripeWidth is `stripe` (diagonals rising to
    // the right); else `fill`. The id and the params change nothing here.
    namespace shaderPlaceholder
    {
        inline constexpr Col   fill{ 28, 30, 34, 255 };
        inline constexpr Col   stripe{ 46, 49, 55, 255 };
        inline constexpr Col   border{ 112, 118, 128, 255 };
        inline constexpr float stripePeriod = 24.0f;         // logical px, along lx + ly
        inline constexpr float stripeWidth = 6.0f;
    }

    // The frame at its physical size (logical size * info.dpi), supersampled per axis and box-filtered down.
    Image rasterise(const PrimList&, const FontAtlasSdf&, int supersample = 2);

    // An RGBA PNG; false when the file cannot be written.
    bool writePng(const Image&, const char* path);
}

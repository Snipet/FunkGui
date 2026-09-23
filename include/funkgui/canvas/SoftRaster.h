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

    // The frame at its physical size (logical size * info.dpi), supersampled per axis and box-filtered down.
    Image rasterise(const PrimList&, const FontAtlasSdf&, int supersample = 2);

    // An RGBA PNG; false when the file cannot be written.
    bool writePng(const Image&, const char* path);
}

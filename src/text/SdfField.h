#pragma once

// Internal to FunkGui's sources (not installed, not API): the JUCE-free half of the atlas bake, shared by
// src/text/FontAtlasSdf.cpp, which defines it, and src/juce/FontAtlasBake.cpp, which rasterises glyph coverage with
// JUCE and calls it (v0.12.0: bake() moved under src/juce/, the distance transform stayed in core).

#include <vector>

namespace funkgui::detail
{
    // "No site in reach": larger than any squared distance inside one glyph cell.
    inline constexpr float kSdfInf = 1.0e20f;

    // Exact squared Euclidean distance transform of the set { grid[i] == 0 }, in place (w * h floats, row-major):
    // Felzenszwalb & Huttenlocher's 1-D transform over the columns, then over the rows.
    void sdfEdt2d(std::vector<float>& grid, int w, int h);
}

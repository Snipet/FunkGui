#pragma once

// How a run of text is set (02 §2.2: moved out of the snapshot's SdfCanvas so that the type scale, the recorder and
// the text utilities need no bgfx). The fields and their meaning are HR's SdfCanvas::TextStyle, unchanged.

#include <cstdint>

namespace funkgui
{
    struct TextStyle
    {
        float px       = 13.0f;  // JUCE font height (ascent + descent), not the em (A §2.5); the atlas bakes at 48
        float tracking = 0.0f;   // extra advance per glyph, logical px; not added after the last glyph
        float weight   = 0.0f;   // SDF threshold shift; ~0.05 reads semibold
        bool  tabular  = false;  // every digit takes the widest digit's slot
    };

    // Horizontal anchor of Canvas::text's x: the run's left edge, centre or right edge.
    enum class Align : uint8_t { left, centre, right };
}

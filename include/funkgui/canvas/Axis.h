#pragma once

// A plot's value <-> pixel mapping, recorded into the PrimList for probes (02 §3.2, §3.8 "axis" lines, §4.2 axis()).
// Recording it draws nothing: a probe reads the mapping back to check a curve against its analytic values (C's G2)
// and a dot or meter against the audio (G3).

#include <funkgui/canvas/Tags.h>

namespace funkgui
{
    // Value v0 sits at pixel px0 and v1 at px1; linear between them, or logarithmic in the value when `log`.
    struct AxisMap
    {
        float px0 = 0.0f, px1 = 0.0f, v0 = 0.0f, v1 = 0.0f;
        bool  log = false;
    };

    // One plot's axes under its tag. hasX / hasY say which maps are present ("-" in the dump for an absent one).
    struct AxisRec
    {
        Tag     tag = 0;
        AxisMap x{}, y{};
        bool    hasX = false, hasY = false;
    };
}

#pragma once

// The keyboard focus ring (02 §5.8): four accent hairlines on hit.reduced(1) (HR BgfxEditor.cpp:1542-1551), tagged
// FOCUS_RING. Every control in the Tab order draws it when focused, including locked, derived and n/a slots (02 §8.1).
//
// Declared in G2 (v0.2.0, frozen at FZ1); implemented by G5 (src/widgets/FocusRing.cpp).

#include <funkgui/core/Col.h>
#include <funkgui/core/Geometry.h>

namespace funkgui
{
    class Canvas;

    void drawFocusRing(Canvas&, Rect hit, Col accent);
}

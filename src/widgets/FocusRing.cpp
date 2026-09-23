#include <funkgui/widgets/FocusRing.h>

#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/Tags.h>

// The keyboard focus ring (02 §5.8): HR's four accent hairlines on the hit rectangle inset by one pixel
// (BgfxEditor.cpp:1542-1551, drawControls :1622-1629), tagged FOCUS_RING. The hairlines floor to a device pixel as
// HR's did, so the ring sits on the same device pixels at every dpi. Never live: focus is state, not data.

namespace funkgui
{
    void drawFocusRing(Canvas& c, Rect hit, Col accent)
    {
        const Rect r = hit.reduced(1.0f);
        if (r.isEmpty())
            return;
        const Canvas::Scope s(c, tags::focusRing, false);
        c.hairlineH(r.x, r.y, r.w, accent);
        c.hairlineH(r.x, r.bottom(), r.w, accent);
        c.hairlineV(r.x, r.y, r.h, accent);
        c.hairlineV(r.right(), r.y, r.h, accent);
    }
}

// Windows has no display link here (v0.24.0; include/funkgui/gpu/DisplayLink.h), as Linux: createDisplayLink() refuses
// every view, so the FramePump runs on its fallback juce::Timer: kFullHz while anything moves, kIdleHz at rest, one
// bgfx::frame() per tick for every editor in the process. bgfx presents without vsync (src/gpu/BgfxReset.h: a DXGI
// Present with no sync interval), so a present never blocks the message thread, and each tick measures its own dt, so
// animation speed does not depend on the timer's regularity. A frame is presented by the bgfx::frame() that renders it
// (v0.25.1).

#include <funkgui/gpu/DisplayLink.h>

namespace funkgui
{
    void* createDisplayLink(void*, DisplayLinkCallback, void*)
    {
        return nullptr;
    }

    void destroyDisplayLink(void*) {}

    void setDisplayLinkRate(void*, float, float, float) {}

    bool viewOnScreen(void* view)
    {
        return view != nullptr;                      // no link to stop (DisplayLink.h)
    }

    const char* displayLinkTargetClassName()
    {
        return "";
    }
}

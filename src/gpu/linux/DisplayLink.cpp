// Linux has no display link (v0.11.0; include/funkgui/gpu/DisplayLink.h). createDisplayLink() refuses every view, so
// the FramePump runs on its fallback juce::Timer: kFullHz while anything moves, kIdleHz at rest, one bgfx::frame() per
// tick for every editor in the process. bgfx presents without vsync (BGFX_RESET_NONE), so a present never blocks the
// message thread, and each tick measures its own dt, so animation speed does not depend on the timer's regularity.

#include <funkgui/gpu/DisplayLink.h>

namespace funkgui
{
    void* createDisplayLink(void*, DisplayLinkCallback, void*)
    {
        return nullptr;
    }

    void destroyDisplayLink(void*) {}

    void setDisplayLinkRate(void*, float, float, float) {}

    const char* displayLinkTargetClassName()
    {
        return "";
    }
}

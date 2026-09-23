#pragma once

// Thin C interface over the macOS render view. The view is click-through
// (hitTest returns nil) so JUCE's peer NSView keeps receiving every mouse
// event; bgfx attaches its CAMetalLayer to this child view.

namespace hrvbgui
{
    void*  createRenderView(void* parentNSView, int x, int y, int w, int h);
    void   setRenderViewFrame(void* view, int x, int y, int w, int h);
    void   destroyRenderView(void* view);
    double getBackingScale(void* view);
}

#pragma once

// Thin C interface over the macOS render view. The view is click-through
// (hitTest returns nil) so JUCE's peer NSView keeps receiving every mouse
// event; bgfx attaches its CAMetalLayer to this child view. Its class is
// registered at run time under a randomised name (src/gpu/ObjcClasses.h).

namespace funkgui
{
    void*  createRenderView(void* parentNSView, int x, int y, int w, int h);
    void   setRenderViewFrame(void* view, int x, int y, int w, int h);
    void   destroyRenderView(void* view);
    double getBackingScale(void* view);

    // The Objective-C runtime name of the render view's class in this binary: FUNKGUI_OBJC_PREFIX "RenderView_" and a
    // random hex suffix, registered once per binary on first use (G7; 02 §1.8, K2 #16). Diagnostics and fg.objc.names;
    // nothing looks the class up by this name.
    const char* renderViewClassName();
}

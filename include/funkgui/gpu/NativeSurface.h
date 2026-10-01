#pragma once

// Thin C interface over the platform's render view: the child of JUCE's peer that bgfx draws into. Positions and sizes
// are in the peer's logical units, as ComponentPeer::getAreaCoveredBy() gives them; getBackingScale() is the device
// pixels per unit.
//
// macOS: the view is click-through (hitTest returns nil) so JUCE's peer NSView keeps receiving every mouse event; bgfx
// attaches its CAMetalLayer to this child view. Its class is registered at run time under a randomised name
// (src/gpu/ObjcClasses.h). The handle is the NSView.
//
// Linux (v0.11.0, src/gpu/linux/NativeSurface.cpp): an X11 child window of the peer's window, on JUCE's display
// connection. It selects no input events, so every pointer and key event propagates to JUCE's peer window, as it does
// for JUCE's own OpenGL child window; bgfx creates its Vulkan (or OpenGL) surface on it. The handle is the X11 window
// ID. JUCE's peers are X11 windows, so on a Wayland desktop this runs through XWayland, like every JUCE plug-in and the
// Linux hosts that embed them.

namespace funkgui
{
    void*  createRenderView(void* parentNSView, int x, int y, int w, int h);
    void   setRenderViewFrame(void* view, int x, int y, int w, int h);
    void   destroyRenderView(void* view);
    // Device pixels per logical unit of the window holding <view>: a peer's native handle or a render view.
    double getBackingScale(void* view);

    // The device pixels per logical unit EditorHost sizes <view>'s drawable at: getBackingScale(), or its UI_SCALE
    // capture override (v0.11.0). On Linux the X window is placed and sized at this scale (the peer's until it is set),
    // so the window, the Vulkan swapchain and the drawable always have one size, and the last frame is re-applied at
    // once. On macOS it does nothing: the CAMetalLayer scales any drawable into the view.
    void   setRenderViewScale(void* view, double scale);

    // What bgfx's PlatformData::ndt pairs with the render views (v0.11.0): the X11 Display* on Linux (JUCE's
    // connection, which the views are created on; nullptr when JUCE has no display open); nullptr on macOS.
    void*  nativeDisplay();

    // The Objective-C runtime name of the render view's class in this binary: FUNKGUI_OBJC_PREFIX "RenderView_" and a
    // random hex suffix, registered once per binary on first use (G7; 02 §1.8, K2 #16). Diagnostics and fg.objc.names;
    // nothing looks the class up by this name. Linux defines no class: "" (v0.11.0).
    const char* renderViewClassName();
}

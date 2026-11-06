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
//
// Windows (v0.24.0, src/gpu/win/NativeSurface.cpp): a child window of the peer's HWND, made in the peer's DPI awareness
// context, of a window class registered per binary under a randomised name. It is transparent to hit testing
// (WM_NCHITTEST answers HTTRANSPARENT), so every pointer event goes to JUCE's peer, which keeps the keyboard focus;
// bgfx's Direct3D 11 presents into it through a DXGI swap chain. The handle is the HWND.

namespace funkgui
{
    void*  createRenderView(void* parentNSView, int x, int y, int w, int h);
    void   setRenderViewFrame(void* view, int x, int y, int w, int h);
    void   destroyRenderView(void* view);
    // Device pixels per logical unit of the window holding <view>: a peer's native handle or a render view.
    double getBackingScale(void* view);

    // v0.21.0: a hold on <view> that outlives destroyRenderView(), for BgfxContext: bgfx runs the commands that name a
    // view (a framebuffer's creation, its destruction) at its next frame, so a view an editor destroys before that
    // frame must stay allocated until then. macOS: the NSView retained and released (destroyRenderView still takes
    // it out of its window at once). Linux: both do nothing, and an X window goes when destroyRenderView destroys it
    // (stated fallback: X windows have no reference count). Windows (v0.24.0): the window is counted, as macOS's
    // NSView; destroyRenderView hides it and takes it out of the peer at once, and it is destroyed with its last
    // reference.
    void   retainRenderView(void* view);
    void   releaseRenderView(void* view);

    // The device pixels per logical unit EditorHost sizes <view>'s drawable at: getBackingScale(), or its UI_SCALE
    // capture override (v0.11.0). On Linux the X window is placed and sized at this scale (the peer's until it is set),
    // so the window, the Vulkan swapchain and the drawable always have one size, and the last frame is re-applied at
    // once. On Windows likewise: the child window is placed and sized in the peer's client pixels at this scale, the
    // swap chain's size (v0.24.0). On macOS it does nothing: the CAMetalLayer scales any drawable into the view.
    void   setRenderViewScale(void* view, double scale);

    // Unchanged frames (0.25.0; gpu/EditorHost.h "Unchanged frames"): what an editor that leaves its last picture up
    // must know about the screen.
    // - renderViewOccluded: whether a frame drawn into <view> now would not reach the screen. macOS: its window's
    //   occlusion state lacks NSWindowOcclusionStateVisible, exactly the test bgfx's Metal renderer makes before it
    //   takes a drawable (it draws such a frame offscreen and presents nothing). Linux and Windows: false (bgfx's
    //   Vulkan and Direct3D 11 present whatever covers the window). nullptr: false.
    // - renderViewKeepsFrame: whether <view> goes on showing the last frame presented into it, through being covered
    //   and uncovered, until the next. macOS: true (a CAMetalLayer keeps its contents). Windows: true (the desktop
    //   window manager composes every window). Linux: true while a compositing manager runs (one owns the
    //   _NET_WM_CM_S<screen> selection on JUCE's display; asked at most twice a second), false without one, where the
    //   X server hands an uncovered part of the window back to be drawn and the render view, which selects no events,
    //   hears nothing of it: the editor then draws every frame, as before (the stated fallback). nullptr: false.
    bool   renderViewOccluded(void* view);
    bool   renderViewKeepsFrame(void* view);

    // What bgfx's PlatformData::ndt pairs with the render views (v0.11.0): the X11 Display* on Linux (JUCE's
    // connection, which the views are created on; nullptr when JUCE has no display open); nullptr on macOS and
    // Windows.
    void*  nativeDisplay();

    // The Objective-C runtime name of the render view's class in this binary: FUNKGUI_OBJC_PREFIX "RenderView_" and a
    // random hex suffix, registered once per binary on first use (G7; 02 §1.8, K2 #16). Diagnostics and fg.objc.names;
    // nothing looks the class up by this name. Linux defines no class: "" (v0.11.0). Windows: the render view's window
    // class, named by the same rule (v0.24.0).
    const char* renderViewClassName();
}

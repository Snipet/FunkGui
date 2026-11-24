#include "DesktopSurface.h"

#include <funkgui/gpu/NativeSurface.h>

// The three functions of gpu/NativeSurface.h that BgfxContext calls, for a build without JUCE (v0.28.0). With JUCE
// they are defined beside the render views (src/gpu/NativeSurface.mm, src/gpu/linux/, src/gpu/win/), which this build
// does not compile; the rest of that header (render views as children of a JUCE peer) is not defined here, and
// nothing in a JUCE-free FunkGui::gpu calls it.
//
// - nativeDisplay(): what bgfx's PlatformData::ndt pairs with the window. Linux: the X11 Display* of SDL's own
//   connection, on which SDL made the window (DesktopHost sets it); nullptr on macOS and Windows, and on Linux before
//   a window is open, where BgfxContext then refuses to start.
// - retainRenderView() and releaseRenderView(): nothing. BgfxContext holds a view from acquire() until bgfx has run the
//   last command that names it, because an editor in a plug-in host may free its view while other editors keep bgfx
//   running. DesktopHost registers one window, so its release is bgfx's shutdown, which runs or drops every command
//   before it returns; and DesktopHost destroys the window (and macOS's Metal view) only after that.

namespace funkgui
{
    namespace
    {
        void* display = nullptr;
    }

    void desktop::detail::setNativeDisplay(void* d)
    {
        display = d;
    }

    void* nativeDisplay()
    {
        return display;
    }

    void retainRenderView(void*) {}

    void releaseRenderView(void*) {}
}

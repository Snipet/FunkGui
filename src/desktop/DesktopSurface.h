#pragma once

// Internal to src/desktop/ (not API): the native display BgfxContext pairs with the window it is given
// (gpu/NativeSurface.h nativeDisplay()). DesktopHost sets it before it registers its window and clears it after it
// released it. Main thread.

namespace funkgui::desktop::detail
{
    void setNativeDisplay(void* display);            // Linux: the X11 Display* of SDL's connection; else nullptr
}

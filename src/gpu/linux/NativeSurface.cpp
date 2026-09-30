// The Linux render view (v0.11.0; include/funkgui/gpu/NativeSurface.h): an X11 child window of JUCE's peer window.
//
// - JUCE's display connection and its dynamically loaded Xlib (juce::X11Symbols): FunkGui links no X11 library of its
//   own, and the view lives on the connection JUCE creates and destroys the peer on. JUCE_GUI_BASICS_INCLUDE_XHEADERS
//   exposes both, and must precede the first JUCE header of this translation unit.
// - The view selects no events and has no background: pointer, wheel and key events propagate to the peer window,
//   whose JUCE handler treats a crossing into the child as an ordinary move (juce_XWindowSystem_linux.cpp,
//   handleLeaveNotifyEvent), exactly as for JUCE's own OpenGL child window; and the server never clears it, so a
//   resize shows the last frame rather than a flash of the parent.
// - Geometry: the arguments are the peer's logical units (getAreaCoveredBy), and an X window is placed in device
//   pixels: each edge is rounded at the view's scale, which is the peer's (ComponentPeer::getPlatformScaleFactor(),
//   JUCE's per-display scale) until EditorHost sets the one it sizes the drawable at (setRenderViewScale: the same,
//   or the UI_SCALE capture override). The size is then roundToInt(w * scale), EditorHost's drawable size, so the
//   window and the Vulkan swapchain, whose extent an X11 surface pins to the window's, always match the drawable.
// - Lifetime: destroying a peer's window destroys this child with it on the server. A host closing the editor removes
//   it from its parent before the peer goes (~Component removes children first), so EditorHost detaches while the
//   window still exists; but a path that deletes the peer first (Component::removeFromDesktop() tells no child) would
//   destroy a dead window. Creating and destroying therefore run under an X error trap: in a plug-in JUCE installs no
//   X error handler (only a standalone app gets one), so a BadWindow would reach the host's, and GDK's aborts on an
//   error nobody trapped.
//
// Message thread only, like everything that calls it.

#define JUCE_GUI_BASICS_INCLUDE_XHEADERS 1
#include <juce_gui_basics/juce_gui_basics.h>

#include <funkgui/gpu/NativeSurface.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace funkgui
{
    namespace
    {
        struct RenderView
        {
            ::Window window = 0;
            ::Window parent = 0;
            double   scale = 0.0;                    // setRenderViewScale's; 0 = the peer's
            int x = 0, y = 0, w = 1, h = 1;          // the last frame, logical
        };

        // The live render views, for setRenderViewFrame(), setRenderViewScale() and getBackingScale(). Never
        // destroyed: the process can outlive JUCE's statics (FramePump.cpp's reasoning).
        std::vector<RenderView>& views()
        {
            static auto* v = new std::vector<RenderView>();
            return *v;
        }

        ::Window windowOf(void* handle) noexcept
        {
            return static_cast<::Window>(reinterpret_cast<std::uintptr_t>(handle));
        }

        void* handleOf(::Window w) noexcept
        {
            return reinterpret_cast<void*>(static_cast<std::uintptr_t>(w));
        }

        ::Display* xDisplay()
        {
            auto* xws = juce::XWindowSystem::getInstanceWithoutCreating();
            return xws != nullptr ? xws->getDisplay() : nullptr;
        }

        RenderView* findView(::Window w)
        {
            auto& v = views();
            const auto it = std::find_if(v.begin(), v.end(), [w](const RenderView& r) { return r.window == w; });
            return it != v.end() ? &*it : nullptr;
        }

        // JUCE's peer whose X window is <w>.
        juce::ComponentPeer* peerOf(::Window w)
        {
            for (int i = 0; i < juce::ComponentPeer::getNumPeers(); ++i)
                if (auto* p = juce::ComponentPeer::getPeer(i); p != nullptr && windowOf(p->getNativeHandle()) == w)
                    return p;
            return nullptr;
        }

        double scaleOfPeerWindow(::Window w)
        {
            const auto* p = peerOf(w);
            const double s = p != nullptr ? p->getPlatformScaleFactor() : 1.0;
            return s > 0.0 ? s : 1.0;
        }

        double scaleOf(const RenderView& rv)
        {
            return rv.scale > 0.0 ? rv.scale : scaleOfPeerWindow(rv.parent);
        }

        // Errors caused by the requests made while it lives are dropped: the destructor syncs, so they arrive while the
        // trap is still installed, then restores the previous handler (GDK's error-trap pattern). The handler is
        // process-wide, so the trap is kept to single, rare operations on the message thread.
        class ScopedXErrorTrap
        {
        public:
            explicit ScopedXErrorTrap(::Display* d)
                : display_(d), previous_(juce::X11Symbols::getInstance()->xSetErrorHandler(&ignore)) {}

            ~ScopedXErrorTrap()
            {
                auto* xs = juce::X11Symbols::getInstance();
                xs->xSync(display_, False);
                xs->xSetErrorHandler(previous_);
            }

            ScopedXErrorTrap(const ScopedXErrorTrap&) = delete;
            ScopedXErrorTrap& operator=(const ScopedXErrorTrap&) = delete;

        private:
            static int ignore(::Display*, XErrorEvent*) { return 0; }

            ::Display* display_;
            XErrorHandler previous_;
        };

        struct DeviceRect
        {
            int x = 0, y = 0;
            unsigned w = 1, h = 1;                   // an X window is at least 1 x 1 (0 is BadValue)
        };

        DeviceRect toDevice(int x, int y, int w, int h, double s)
        {
            DeviceRect r;
            r.x = juce::roundToInt(x * s);
            r.y = juce::roundToInt(y * s);
            r.w = static_cast<unsigned>(std::max(1, juce::roundToInt(w * s)));
            r.h = static_cast<unsigned>(std::max(1, juce::roundToInt(h * s)));
            return r;
        }

        void place(::Display* d, const RenderView& rv)
        {
            const DeviceRect r = toDevice(rv.x, rv.y, rv.w, rv.h, scaleOf(rv));
            auto* xs = juce::X11Symbols::getInstance();
            xs->xMoveResizeWindow(d, rv.window, r.x, r.y, r.w, r.h);
            xs->xFlush(d);
        }
    }

    void* createRenderView(void* parentHandle, int x, int y, int w, int h)
    {
        const ::Window parent = windowOf(parentHandle);
        ::Display* d = xDisplay();
        auto* xs = juce::X11Symbols::getInstance();
        if (parent == 0 || d == nullptr || xs == nullptr)
            return nullptr;

        RenderView rv;
        rv.parent = parent;
        rv.x = x; rv.y = y; rv.w = w; rv.h = h;
        const DeviceRect r = toDevice(x, y, w, h, scaleOf(rv));
        XSetWindowAttributes swa{};
        swa.background_pixmap = None;                // never cleared by the server
        swa.border_pixel = 0;
        swa.event_mask = NoEventMask;                // every event goes on to JUCE's peer window
        {
            const ScopedXErrorTrap trap(d);          // and its sync: the window exists before bgfx asks about it
            // Depth, visual and colormap from the parent (CopyFromParent), so any visual JUCE chose for its peer works.
            rv.window = xs->xCreateWindow(d, parent, r.x, r.y, r.w, r.h, 0, CopyFromParent, InputOutput, nullptr,
                                          CWBackPixmap | CWBorderPixel | CWEventMask, &swa);
            if (rv.window != 0)
                xs->xMapWindow(d, rv.window);
        }
        if (rv.window == 0)
            return nullptr;
        views().push_back(rv);
        return handleOf(rv.window);
    }

    void setRenderViewFrame(void* view, int x, int y, int w, int h)
    {
        ::Display* d = xDisplay();
        RenderView* rv = findView(windowOf(view));
        if (rv == nullptr || d == nullptr)
            return;
        rv->x = x; rv->y = y; rv->w = w; rv->h = h;
        place(d, *rv);
    }

    void setRenderViewScale(void* view, double scale)
    {
        ::Display* d = xDisplay();
        RenderView* rv = findView(windowOf(view));
        if (rv == nullptr || d == nullptr || !(scale > 0.0) || std::fabs(scale - rv->scale) < 1.0e-6)
            return;                                  // EditorHost's own test for an unchanged scale
        rv->scale = scale;
        place(d, *rv);
    }

    void destroyRenderView(void* view)
    {
        const ::Window w = windowOf(view);
        auto& v = views();
        const auto it = std::find_if(v.begin(), v.end(), [w](const RenderView& r) { return r.window == w; });
        if (it == v.end())
            return;
        v.erase(it);
        if (::Display* d = xDisplay())
        {
            const ScopedXErrorTrap trap(d);          // the peer's window, and with it this one, may be gone already
            juce::X11Symbols::getInstance()->xDestroyWindow(d, w);
        }
    }

    double getBackingScale(void* view)
    {
        const ::Window w = windowOf(view);
        if (w == 0)
            return 1.0;
        if (const RenderView* rv = findView(w))
            return scaleOfPeerWindow(rv->parent);
        return scaleOfPeerWindow(w);
    }

    void* nativeDisplay()
    {
        return xDisplay();
    }

    const char* renderViewClassName()
    {
        return "";
    }
}

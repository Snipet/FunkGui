// The Windows render view (v0.24.0; include/funkgui/gpu/NativeSurface.h): a child window of JUCE's peer window, which
// bgfx's Direct3D 11 presents into through a DXGI swap chain.
//
// - A WS_CHILD window of the peer's HWND, of a window class this binary registers itself, under the name macOS gives
//   its Objective-C class (FUNKGUI_OBJC_PREFIX "RenderView_" and a random hex suffix): two products in one host
//   register one each, and a plug-in unloaded and loaded again never meets its old registration. The class goes with
//   the last view (UnregisterClassW), so an unloaded plug-in leaves no class whose window procedure is gone.
// - Input reaches JUCE's peer: the view answers WM_NCHITTEST with HTTRANSPARENT, as JUCE's own windows that ignore
//   clicks do, so Windows sends every pointer and wheel message to the peer under it (on the same thread), and the
//   peer keeps the keyboard focus: the view is never activated and never takes the focus. A file dragged over it is
//   the peer's too (OLE looks up the window chain for a drop target).
// - It erases and paints nothing (WM_ERASEBKGND, WM_PAINT validated): DXGI presents into it, and a resize shows the
//   last frame rather than a flash of the parent.
// - DPI: it is made in the peer's DPI awareness context (GetWindowDpiAwarenessContext, Windows 10 1607 and later,
//   resolved from user32 at run time), so the child of a per-monitor-aware peer is per-monitor aware and the child of
//   an unaware host's peer is unaware, and both measure in the peer's client pixels. The arguments are the peer's
//   logical units (getAreaCoveredBy); each edge is rounded at the view's scale, the peer's (getPlatformScaleFactor())
//   until EditorHost sets the one it sizes the drawable at (setRenderViewScale: the same, or the UI_SCALE capture
//   override). The size is then roundToInt(w * scale), EditorHost's drawable size, so the window and the swap chain
//   always have the drawable's size (DXGI_SCALING_NONE: bgfx never stretches it).
// - Lifetime: bgfx may name a view's window until two frames after its framebuffer goes (NativeSurface.h, v0.21.0),
//   and a swap chain made for a window that is gone is a fatal error in bgfx. So the window is counted, as macOS
//   retains its NSView: createRenderView holds one reference and retainRenderView adds one; destroyRenderView hides
//   the window and takes it out of the peer at once (a message-only window: drawn nowhere, and no longer a child that
//   the peer's own destruction would take along), then drops its reference; the window goes with the last one.
//
// Message thread only, like everything that calls it.

#include <juce_gui_basics/juce_gui_basics.h>

#include <funkgui/core/Config.h>
#include <funkgui/gpu/NativeSurface.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#ifndef NOMINMAX
  #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace funkgui
{
    namespace
    {
        struct RenderView
        {
            HWND   window = nullptr;
            HWND   parent = nullptr;
            double scale = 0.0;                      // setRenderViewScale's; 0 = the peer's
            int x = 0, y = 0, w = 1, h = 1;          // the last frame, logical
            int refs = 1;                            // createRenderView's, then retainRenderView's
            bool gone = false;                       // destroyRenderView has run: hidden, out of the peer
        };

        // The live render views. Never destroyed: the process can outlive JUCE's statics (FramePump.cpp's reasoning).
        std::vector<RenderView>& views()
        {
            static auto* v = new std::vector<RenderView>();
            return *v;
        }

        RenderView* findView(HWND w)
        {
            auto& v = views();
            const auto it = std::find_if(v.begin(), v.end(), [w](const RenderView& r) { return r.window == w; });
            return it != v.end() ? &*it : nullptr;
        }

        // ---- the DPI context (user32, Windows 10 1607 and later; before it, every window is the process's) ---------
        using DpiContext = HANDLE;                   // DPI_AWARENESS_CONTEXT, which older SDK headers do not declare
        using GetWindowDpiContextFn = DpiContext(WINAPI*)(HWND);
        using SetThreadDpiContextFn = DpiContext(WINAPI*)(DpiContext);

        template <typename Fn>
        Fn user32(const char* name)
        {
            const HMODULE user = GetModuleHandleW(L"user32.dll");
            if (user == nullptr)
                return nullptr;
            // Through void(*)(): the one function type a cast from FARPROC to another one never warns about.
            return reinterpret_cast<Fn>(reinterpret_cast<void (*)()>(GetProcAddress(user, name)));
        }

        // Makes the calling thread's DPI awareness <hwnd>'s while it lives.
        class ScopedDpiContextOf
        {
        public:
            explicit ScopedDpiContextOf(HWND hwnd)
            {
                static const auto windowContext = user32<GetWindowDpiContextFn>("GetWindowDpiAwarenessContext");
                static const auto setThread = user32<SetThreadDpiContextFn>("SetThreadDpiAwarenessContext");
                if (windowContext == nullptr || setThread == nullptr)
                    return;
                if (const DpiContext ctx = windowContext(hwnd); ctx != nullptr)
                {
                    set_ = setThread;
                    previous_ = setThread(ctx);
                }
            }

            ~ScopedDpiContextOf()
            {
                if (set_ != nullptr && previous_ != nullptr)
                    set_(previous_);
            }

            ScopedDpiContextOf(const ScopedDpiContextOf&) = delete;
            ScopedDpiContextOf& operator=(const ScopedDpiContextOf&) = delete;

        private:
            SetThreadDpiContextFn set_ = nullptr;
            DpiContext previous_ = nullptr;
        };

        // ---- the window class ------------------------------------------------------------------------------------
        LRESULT CALLBACK renderViewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
        {
            switch (msg)
            {
                case WM_NCHITTEST:     return HTTRANSPARENT;   // every pointer message goes to JUCE's peer below
                case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
                case WM_ERASEBKGND:    return 1;               // never cleared: a resize shows the last frame
                case WM_PAINT:         ValidateRect(hwnd, nullptr); return 0;   // DXGI presents into it
                default:               return DefWindowProcW(hwnd, msg, wp, lp);
            }
        }

        // The module this code is in (a plug-in's DLL, an app's EXE): the class is registered for it alone.
        HINSTANCE thisModule()
        {
            HMODULE m = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&renderViewProc), &m);
            return m;
        }

        // The class's name in this binary, made once: the product's prefix, then 16 random hex digits (macOS's rule,
        // ObjcClasses.h).
        const std::string& classNameUtf8()
        {
            static const std::string name = [] {
                const juce::String n = juce::String(FUNKGUI_OBJC_PREFIX_STR "RenderView_")
                                       + juce::String::toHexString(juce::Random::getSystemRandom().nextInt64())
                                             .paddedLeft('0', 16);
                return n.toStdString();
            }();
            return name;
        }

        const std::wstring& className()
        {
            static const std::wstring name = juce::String::fromUTF8(classNameUtf8().c_str()).toWideCharPointer();
            return name;
        }

        ATOM& classAtom()
        {
            static ATOM atom = 0;
            return atom;
        }

        bool registerClass()
        {
            if (classAtom() != 0)
                return true;
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof wc;
            wc.lpfnWndProc = renderViewProc;
            wc.hInstance = thisModule();
            wc.lpszClassName = className().c_str();
            wc.hCursor = nullptr;                    // the peer's cursor shows: the view never hit-tests
            wc.hbrBackground = nullptr;              // nothing erased
            classAtom() = RegisterClassExW(&wc);
            return classAtom() != 0;
        }

        void unregisterClassIfUnused()
        {
            if (classAtom() == 0 || !views().empty())
                return;
            UnregisterClassW(className().c_str(), thisModule());
            classAtom() = 0;
        }

        // ---- geometry --------------------------------------------------------------------------------------------
        // JUCE's peer whose window is <w>.
        juce::ComponentPeer* peerOf(HWND w)
        {
            for (int i = 0; i < juce::ComponentPeer::getNumPeers(); ++i)
            {
                auto* p = juce::ComponentPeer::getPeer(i);
                if (p != nullptr && static_cast<HWND>(p->getNativeHandle()) == w)
                    return p;
            }
            return nullptr;
        }

        double scaleOfPeerWindow(HWND w)
        {
            const auto* p = peerOf(w);
            const double s = p != nullptr ? p->getPlatformScaleFactor() : 1.0;
            return s > 0.0 ? s : 1.0;
        }

        double scaleOf(const RenderView& rv)
        {
            return rv.scale > 0.0 ? rv.scale : scaleOfPeerWindow(rv.parent);
        }

        struct DeviceRect
        {
            int x = 0, y = 0, w = 1, h = 1;          // a window of 0 x 0 gets no swap chain: at least 1 x 1
        };

        DeviceRect toDevice(int x, int y, int w, int h, double s)
        {
            DeviceRect r;
            r.x = juce::roundToInt(x * s);
            r.y = juce::roundToInt(y * s);
            r.w = std::max(1, juce::roundToInt(w * s));
            r.h = std::max(1, juce::roundToInt(h * s));
            return r;
        }

        void place(const RenderView& rv)
        {
            const DeviceRect r = toDevice(rv.x, rv.y, rv.w, rv.h, scaleOf(rv));
            SetWindowPos(rv.window, nullptr, r.x, r.y, r.w, r.h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }

        // One reference less; the window and its record go with the last (and the class with the last window).
        void drop(HWND w)
        {
            auto& v = views();
            const auto it = std::find_if(v.begin(), v.end(), [w](const RenderView& r) { return r.window == w; });
            if (it == v.end() || --it->refs > 0)
                return;
            v.erase(it);
            if (IsWindow(w))
                DestroyWindow(w);
            unregisterClassIfUnused();
        }
    }

    void* createRenderView(void* parentHandle, int x, int y, int w, int h)
    {
        const HWND parent = static_cast<HWND>(parentHandle);
        if (parent == nullptr || !IsWindow(parent) || !registerClass())
            return nullptr;

        RenderView rv;
        rv.parent = parent;
        rv.x = x; rv.y = y; rv.w = w; rv.h = h;
        const DeviceRect r = toDevice(x, y, w, h, scaleOf(rv));
        {
            const ScopedDpiContextOf dpi(parent);    // the peer's coordinates and DPI behaviour
            rv.window = CreateWindowExW(WS_EX_NOPARENTNOTIFY, className().c_str(), L"",
                                        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, r.x, r.y, r.w, r.h,
                                        parent, nullptr, thisModule(), nullptr);
        }
        if (rv.window == nullptr)
        {
            unregisterClassIfUnused();
            return nullptr;
        }
        views().push_back(rv);
        return rv.window;
    }

    void setRenderViewFrame(void* view, int x, int y, int w, int h)
    {
        RenderView* rv = findView(static_cast<HWND>(view));
        if (rv == nullptr || rv->gone)
            return;
        rv->x = x; rv->y = y; rv->w = w; rv->h = h;
        place(*rv);
    }

    void setRenderViewScale(void* view, double scale)
    {
        RenderView* rv = findView(static_cast<HWND>(view));
        if (rv == nullptr || rv->gone || !(scale > 0.0) || std::fabs(scale - rv->scale) < 1.0e-6)
            return;                                  // EditorHost's own test for an unchanged scale
        rv->scale = scale;
        place(*rv);
    }

    void retainRenderView(void* view)
    {
        if (RenderView* rv = findView(static_cast<HWND>(view)))
            ++rv->refs;
    }

    void releaseRenderView(void* view)
    {
        drop(static_cast<HWND>(view));
    }

    void destroyRenderView(void* view)
    {
        const HWND w = static_cast<HWND>(view);
        RenderView* rv = findView(w);
        if (rv == nullptr || rv->gone)
            return;
        rv->gone = true;
        // Out of the peer at once: hidden, then a message-only window. Should Windows refuse the new parent (a DPI
        // hosting mismatch), it stays a hidden child, and goes with the peer if the peer goes first.
        ShowWindow(w, SW_HIDE);
        SetParent(w, HWND_MESSAGE);
        drop(w);
    }

    double getBackingScale(void* view)
    {
        const HWND w = static_cast<HWND>(view);
        if (w == nullptr)
            return 1.0;
        if (const RenderView* rv = findView(w))
            return scaleOfPeerWindow(rv->parent);
        return scaleOfPeerWindow(w);
    }

    void* nativeDisplay()
    {
        return nullptr;                              // Direct3D needs no display connection
    }

    const char* renderViewClassName()
    {
        return classNameUtf8().c_str();
    }
}

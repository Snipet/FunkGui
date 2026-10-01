// FUNKGUI_TEST name=fg.smoke.gpu timeout=900 gpu=1
//
// fg.smoke.gpu on Linux (v0.11.0; the macOS test is smoke_gpu.mm): a console app that links FunkGui::gpu, so every
// source under src/gpu/ (BgfxContext, BgfxSink, EditorHost, FramePump, linux/NativeSurface.cpp, linux/DisplayLink.cpp)
// compiles and links against bgfx and the SPIR-V shaders FunkGuiShaders embedded. At run time it checks what needs no
// GPU: the build seams, that nothing makes a render view without a parent, that there is no display link (the pump
// runs on its timer), that bgfx has its Vulkan backend and that the context is idle until a window is acquired.
//
// With an X display (DISPLAY set; XWayland on a Wayland desktop) it also checks the render view on a real, unmapped
// JUCE peer: an X11 child of the peer's window, selecting no events (so every event reaches JUCE's peer), placed and
// resized in device pixels at the peer's scale, at the peer's scale for getBackingScale(), and gone after
// destroyRenderView(). Without one those rows are skipped with a note, so the test also runs on a headless machine.
// Spec rows only: no golden.

#define JUCE_GUI_BASICS_INCLUDE_XHEADERS 1
#include <juce_gui_basics/juce_gui_basics.h>

#include <funkgui/core/Config.h>
#include <funkgui/gpu/BgfxContext.h>
#include <funkgui/gpu/DisplayLink.h>
#include <funkgui/gpu/NativeSurface.h>
#include <funkgui/test/Harness.h>

#include <bgfx/bgfx.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace T = funkgui::test;

namespace
{
    ::Window windowOf(void* handle)
    {
        return static_cast<::Window>(reinterpret_cast<std::uintptr_t>(handle));
    }

    // The children of <w>, in stacking order.
    std::vector<::Window> childrenOf(::Display* d, ::Window w)
    {
        ::Window root = 0, parent = 0;
        ::Window* kids = nullptr;
        unsigned n = 0;
        std::vector<::Window> out;
        auto* xs = juce::X11Symbols::getInstance();
        if (xs->xQueryTree(d, w, &root, &parent, &kids, &n) != 0 && kids != nullptr)
        {
            out.assign(kids, kids + n);
            xs->xFree(kids);
        }
        return out;
    }

    void surfaceRows(T::Probe& P)
    {
        auto* xws = juce::XWindowSystem::getInstance();
        ::Display* d = xws != nullptr ? xws->getDisplay() : nullptr;
        if (d == nullptr)
        {
            std::printf("NOTE     no X display: the X11 render view rows are skipped\n");
            return;
        }
        P.eq("surface.native_display_is_juces", funkgui::nativeDisplay() == static_cast<void*>(d), 1);

        juce::Component owner;
        owner.setSize(320, 200);
        owner.addToDesktop(0);                       // a peer and its X window; never shown
        auto* peer = owner.getPeer();
        P.eq("surface.peer", peer != nullptr, 1);
        if (peer == nullptr)
            return;
        const ::Window parentWindow = windowOf(peer->getNativeHandle());
        const double s = peer->getPlatformScaleFactor();
        std::printf("INFO     peer window 0x%lx, scale %g\n", static_cast<unsigned long>(parentWindow), s);

        void* view = funkgui::createRenderView(peer->getNativeHandle(), 10, 20, 100, 50);
        P.eq("surface.created", view != nullptr, 1);
        const ::Window v = windowOf(view);
        const auto kids = childrenOf(d, parentWindow);
        P.eq("surface.child_of_peer", std::find(kids.begin(), kids.end(), v) != kids.end(), 1);

        auto* xs = juce::X11Symbols::getInstance();
        XWindowAttributes a{};
        const bool haveAttrs = view != nullptr && xs->xGetWindowAttributes(d, v, &a) != 0;
        P.eq("surface.attributes", haveAttrs, 1);
        P.eq("surface.selects_no_events", haveAttrs && a.your_event_mask == NoEventMask, 1);
        P.eq("surface.input_output", haveAttrs && a.c_class == InputOutput, 1);
        P.eq("surface.x_device_px", haveAttrs ? a.x : -1, juce::roundToInt(10 * s));
        P.eq("surface.y_device_px", haveAttrs ? a.y : -1, juce::roundToInt(20 * s));
        P.eq("surface.width_device_px", haveAttrs ? a.width : -1, juce::roundToInt(100 * s));
        P.eq("surface.height_device_px", haveAttrs ? a.height : -1, juce::roundToInt(50 * s));

        funkgui::setRenderViewFrame(view, 0, 0, 64, 32);
        xs->xSync(d, False);
        XWindowAttributes b{};
        const bool haveMoved = view != nullptr && xs->xGetWindowAttributes(d, v, &b) != 0;
        P.eq("surface.moved_x", haveMoved ? b.x : -1, 0);
        P.eq("surface.resized_width_device_px", haveMoved ? b.width : -1, juce::roundToInt(64 * s));
        P.eq("surface.resized_height_device_px", haveMoved ? b.height : -1, juce::roundToInt(32 * s));
        // The drawable's scale (EditorHost: a UI_SCALE override, say): the window follows it at once, and
        // getBackingScale() still reports the peer's.
        funkgui::setRenderViewScale(view, 2.0 * s);
        xs->xSync(d, False);
        XWindowAttributes c{};
        const bool haveScaled = view != nullptr && xs->xGetWindowAttributes(d, v, &c) != 0;
        P.eq("surface.scaled_width_device_px", haveScaled ? c.width : -1, juce::roundToInt(64 * 2.0 * s));
        P.eq("surface.scaled_height_device_px", haveScaled ? c.height : -1, juce::roundToInt(32 * 2.0 * s));
        P.near("surface.backing_scale_is_peers", funkgui::getBackingScale(view), s, 0.0);
        P.near("surface.backing_scale_of_peer", funkgui::getBackingScale(peer->getNativeHandle()), s, 0.0);
        P.eq("display_link.refuses_view",
             funkgui::createDisplayLink(view, [](void*, double) {}, nullptr) == nullptr, 1);

        funkgui::destroyRenderView(view);
        const auto after = childrenOf(d, parentWindow);
        P.eq("surface.destroyed", std::find(after.begin(), after.end(), v) == after.end(), 1);
        funkgui::destroyRenderView(view);            // a second destroy of the same handle is ignored
        funkgui::setRenderViewFrame(view, 0, 0, 1, 1);   // as is a frame for a view that is gone
        P.eq("surface.unknown_view_ignored", 1, 1);
        owner.removeFromDesktop();
    }
}

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    T::Probe P("fg.smoke.gpu", "", argc, argv);

    // ---- build seams ------------------------------------------------------------------------------------------------
    P.eq("build.has_bgfx", FUNKGUI_HAS_BGFX, 1);
    P.eq("build.transient_vb_bytes", static_cast<int64_t>(FUNKGUI_TRANSIENT_VB_BYTES), int64_t{ 32 } << 20);

    // ---- linux/NativeSurface.cpp and linux/DisplayLink.cpp without a window -----------------------------------------
    P.eq("surface.refuses_null_parent", funkgui::createRenderView(nullptr, 0, 0, 10, 10) == nullptr, 1);
    P.near("surface.backing_scale_of_nothing", funkgui::getBackingScale(nullptr), 1.0, 0.0);
    P.eq("display_link.none", funkgui::createDisplayLink(nullptr, [](void*, double) {}, nullptr) == nullptr, 1);
    funkgui::setDisplayLinkRate(nullptr, 12.0f, 60.0f, 60.0f);              // null handles are ignored
    funkgui::destroyDisplayLink(nullptr);
    P.eq("objc.no_classes", std::strlen(funkgui::renderViewClassName()) == 0
                                && std::strlen(funkgui::displayLinkTargetClassName()) == 0, 1);

    surfaceRows(P);

    // ---- bgfx and the shared context, before any window is acquired -------------------------------------------------
    // Compiled in, read from bgfx's table before anything initialises it (a renderer's name is there either way).
    bgfx::RendererType::Enum renderers[bgfx::RendererType::Count];
    const uint8_t nRenderers = bgfx::getSupportedRenderers(static_cast<uint8_t>(bgfx::RendererType::Count), renderers);
    const bool hasVulkan = std::find(renderers, renderers + nRenderers, bgfx::RendererType::Vulkan)
                           != renderers + nRenderers;
    P.eq("bgfx.vulkan_supported", hasVulkan, 1);
    auto& ctx = funkgui::BgfxContext::get();
    P.eq("context.idle", !ctx.valid(), 1);
    P.eq("context.has_free_slot", ctx.hasFreeSlot(), 1);
    P.eq("context.owns_nothing", !ctx.owns(nullptr), 1);

    return P.finish();
}

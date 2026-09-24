// FUNKGUI_TEST name=fg.smoke.gpu timeout=900 gpu=1
//
// fg.smoke.gpu: the GPU build-chain spike's consumer (SPRINTS.md S0.1, 03 §4.9.5). A console app that links
// FunkGui::gpu, so every source under src/gpu/ (BgfxContext, BgfxSink, EditorHost, FramePump, DisplayLink.mm,
// NativeSurface.mm) compiles and links inside it against bgfx and the shaders embedded by FunkGuiShaders. At run time
// it checks what needs no GPU and no window: the render view's Objective-C class name starts with the configured
// FUNKGUI_OBJC_PREFIX "RenderView" (02 §1.8), the render view behaves as HR's did (click-through, flipped), a display
// link refuses a view that has no window, bgfx reports its Metal backend, and the context is idle until a window is
// acquired. Spec rows only. No row looks a class up by name: G7 registers the classes at runtime under randomised
// names with that root (<OBJC_PREFIX>RenderView_<suffix>, K2 #16), so only the name root of the class of the view
// createRenderView returns is checked (S0 review R-G1 #11).

#import <Cocoa/Cocoa.h>

#include <funkgui/core/Config.h>
#include <funkgui/gpu/BgfxContext.h>
#include <funkgui/gpu/DisplayLink.h>
#include <funkgui/gpu/NativeSurface.h>
#include <funkgui/test/Harness.h>

#include <bgfx/bgfx.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace T = funkgui::test;

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;          // NSApplication, as in a host
    T::Probe P("fg.smoke.gpu", "", argc, argv);

    // ---- build seams ------------------------------------------------------------------------------------------------
    P.eq("build.has_bgfx", FUNKGUI_HAS_BGFX, 1);
    P.eq("build.transient_vb_bytes", static_cast<int64_t>(FUNKGUI_TRANSIENT_VB_BYTES), int64_t{ 32 } << 20);

    // ---- NativeSurface.mm / DisplayLink.mm without a window ---------------------------------------------------------
    @autoreleasepool
    {
        NSView* parent = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 320, 200)];
        void* view = funkgui::createRenderView((void*) parent, 10, 20, 100, 50);
        NSView* v = (NSView*) view;
        P.eq("surface.created", view != nullptr, 1);
        // ---- the view's class: an NSView whose name has the product's root (static now, randomised from G7) -------
        const std::string cls = v != nil ? std::string([NSStringFromClass([v class]) UTF8String]) : std::string();
        std::printf("INFO     render view class %s\n", cls.c_str());
        P.eq("objc.render_view_is_nsview", v != nil && [v isKindOfClass:[NSView class]], 1);
        P.eq("objc.render_view_name_root", std::string_view(cls).starts_with(FUNKGUI_OBJC_PREFIX_STR "RenderView"), 1);
        P.eq("objc.render_view_not_hr", !std::string_view(cls).starts_with("Hrvb"), 1);
        P.eq("surface.child_of_parent", v != nil && [v superview] == parent, 1);
        P.eq("surface.click_through", v != nil && [v hitTest:NSMakePoint(15, 25)] == nil, 1);
        P.eq("surface.flipped", v != nil && [v isFlipped], 1);
        P.eq("surface.layer_backed", v != nil && [v wantsLayer], 1);
        funkgui::setRenderViewFrame(view, 0, 0, 64, 32);
        P.eq("surface.frame_width", v != nil ? static_cast<int64_t>(NSWidth([v frame])) : -1, 64);
        P.near("surface.backing_scale_without_window", funkgui::getBackingScale(view), 1.0, 0.0);
        P.eq("display_link.refuses_windowless_view",
             funkgui::createDisplayLink(view, [](void*, double) {}, nullptr) == nullptr, 1);
        funkgui::setDisplayLinkRate(nullptr, 12.0f, 60.0f, 60.0f);          // null handles are ignored
        funkgui::destroyDisplayLink(nullptr);
        funkgui::destroyRenderView(view);
        P.eq("surface.destroyed", [[parent subviews] count] == 0, 1);
        [parent release];
    }

    // ---- bgfx and the shared context, before any window is acquired -------------------------------------------------
    P.eq("bgfx.metal_backend_name", std::strcmp(bgfx::getRendererName(bgfx::RendererType::Metal), "Metal") == 0, 1);
    auto& ctx = funkgui::BgfxContext::get();
    P.eq("context.idle", !ctx.valid(), 1);
    P.eq("context.has_free_slot", ctx.hasFreeSlot(), 1);
    P.eq("context.owns_nothing", !ctx.owns(nullptr), 1);

    return P.finish();
}

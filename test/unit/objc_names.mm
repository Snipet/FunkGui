// FUNKGUI_TEST name=fg.objc.names timeout=120 gpu=1
//
// fg.objc.names (G7; FCompressor docs/design/02-funkgui-and-ui.md §1.8, K2 #16; A §6.1): FunkGui's Objective-C classes
// are registered at run time under randomised names, so two binaries of one product in one process (an AU and a VST3
// in the same host, an installed build beside a dev build) never share a class. The binary's own registrations
// (renderViewClassName(), displayLinkTargetClassName()) are compared with a second registration of each class, made
// here from the same builders (src/gpu/ObjcClasses.h) exactly as a second binary would make it: the names differ,
// both carry the readable root <FUNKGUI_OBJC_PREFIX>RenderView_ / <FUNKGUI_OBJC_PREFIX>DisplayLinkTarget_, neither is
// the static snapshot name, each class is found by its own name and by no other, and a view of the second class
// behaves as the first (click-through, flipped, layer-backed). Needs no window and no GPU. Spec rows only.

#include "../../src/gpu/ObjcClasses.h"                // first: JUCE's Objective-C helpers

#include <funkgui/core/Config.h>
#include <funkgui/gpu/DisplayLink.h>
#include <funkgui/gpu/NativeSurface.h>
#include <funkgui/test/Harness.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdio>
#include <string>
#include <string_view>

namespace T = funkgui::test;

namespace
{
    // <root> followed by one or more hex digits: JUCE's getRandomisedName (root + String::toHexString(int64)).
    bool rootedRandomName(std::string_view name, std::string_view root)
    {
        if (!name.starts_with(root) || name.size() == root.size())
            return false;
        for (const char c : name.substr(root.size()))
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                return false;
        return true;
    }

    std::string nameOf(Class cls) { return cls != nil ? std::string(class_getName(cls)) : std::string(); }
}

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;          // NSApplication, as in a host
    T::Probe P("fg.objc.names", "", argc, argv);

    const std::string_view viewRoot = funkgui::objc::kRenderViewRoot;
    const std::string_view linkRoot = funkgui::objc::kDisplayLinkTargetRoot;
    P.eq("root.render_view", viewRoot == FUNKGUI_OBJC_PREFIX_STR "RenderView_", 1);
    P.eq("root.display_link_target", linkRoot == FUNKGUI_OBJC_PREFIX_STR "DisplayLinkTarget_", 1);

    // ---- this binary's registrations (made on first use) -------------------------------------------------------------
    const std::string view1 = funkgui::renderViewClassName();
    const std::string link1 = funkgui::displayLinkTargetClassName();
    std::printf("INFO     first registrations  %s  %s\n", view1.c_str(), link1.c_str());
    P.eq("first.render_view_rooted", rootedRandomName(view1, viewRoot), 1);
    P.eq("first.display_link_target_rooted", rootedRandomName(link1, linkRoot), 1);
    P.eq("first.stable", view1 == funkgui::renderViewClassName() && link1 == funkgui::displayLinkTargetClassName(), 1);
    P.eq("static_name.render_view_unused", objc_getClass(FUNKGUI_OBJC_PREFIX_STR "RenderView") == nil, 1);
    P.eq("static_name.display_link_target_unused", objc_getClass(FUNKGUI_OBJC_PREFIX_STR "DisplayLinkTarget") == nil,
         1);

    // The view createRenderView makes is of this binary's class.
    @autoreleasepool
    {
        NSView* parent = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 320, 200)];
        void* view = funkgui::createRenderView((void*) parent, 10, 20, 100, 50);
        NSView* v = (NSView*) view;
        P.eq("first.view_class", v != nil && nameOf([v class]) == view1, 1);
        P.eq("first.view_is_nsview", v != nil && [v isKindOfClass:[NSView class]], 1);
        funkgui::destroyRenderView(view);
        [parent release];
    }

    // ---- a second registration of each class, as a second binary of the same product makes it -------------------
    {
        funkgui::objc::RenderViewClass secondView;
        funkgui::objc::DisplayLinkTargetClass secondLink;
        const std::string view2 = nameOf(secondView.cls), link2 = nameOf(secondLink.cls);
        std::printf("INFO     second registrations %s  %s\n", view2.c_str(), link2.c_str());

        P.eq("second.render_view_rooted", rootedRandomName(view2, viewRoot), 1);
        P.eq("second.display_link_target_rooted", rootedRandomName(link2, linkRoot), 1);
        P.eq("distinct.render_view", !view2.empty() && view2 != view1, 1);
        P.eq("distinct.display_link_target", !link2.empty() && link2 != link1, 1);
        P.eq("lookup.render_view_first", objc_getClass(view1.c_str()) == funkgui::objc::renderViewClass().cls, 1);
        P.eq("lookup.display_link_target_first",
             objc_getClass(link1.c_str()) == funkgui::objc::displayLinkTargetClass().cls, 1);
        P.eq("lookup.render_view_second", objc_getClass(view2.c_str()) == secondView.cls, 1);
        P.eq("lookup.display_link_target_second", objc_getClass(link2.c_str()) == secondLink.cls, 1);
        P.eq("lookup.classes_differ", secondView.cls != funkgui::objc::renderViewClass().cls
                                          && secondLink.cls != funkgui::objc::displayLinkTargetClass().cls,
             1);

        // The second class is a working render view: HR's click-through, flipped, layer-backed child.
        @autoreleasepool
        {
            NSView* v = [secondView.createInstance() initWithFrame:NSMakeRect(0, 0, 64, 32)];
            [v setWantsLayer:YES];
            P.eq("second.view_class", v != nil && nameOf([v class]) == view2, 1);
            P.eq("second.click_through", v != nil && [v hitTest:NSMakePoint(5, 5)] == nil, 1);
            P.eq("second.flipped", v != nil && [v isFlipped], 1);
            P.eq("second.layer_backed", v != nil && [v wantsLayer], 1);
            [v release];
        }
        // ... and a working display-link target: -step: and the "state" ivar the handle fills.
        P.eq("second.target_step", class_respondsToSelector(secondLink.cls, @selector(step:)), 1);
        P.eq("second.target_state_ivar",
             class_getInstanceVariable(secondLink.cls, funkgui::objc::DisplayLinkTargetClass::kStateIvar) != nullptr, 1);
    }

    return P.finish();
}

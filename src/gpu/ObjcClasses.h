#pragma once

// The Objective-C classes FunkGui defines, registered at run time under randomised names (02 §1.8, K2 #16; A §6.1).
//
// Class names are process-global, and a product prefix separates products but not binaries: FCompressor's AU and VST3
// in one host, or an installed build beside a dev build, would both define FcmpRenderView, the runtime would say
// "implemented in both", and by-name lookups would reach the other image's FramePump and BgfxContext. So, as JUCE does
// for its own classes (juce_ObjCHelpers_mac.h, ObjCClass), each binary builds its classes with objc_allocateClassPair
// under <FUNKGUI_OBJC_PREFIX>RenderView_<random hex> and <FUNKGUI_OBJC_PREFIX>DisplayLinkTarget_<random hex>. The prefix
// stays only as a readable root. Nothing ever looks a class up by name.
//
// Private to src/gpu: NativeSurface.mm and DisplayLink.mm hold this binary's registrations in function-local statics
// (renderViewClass(), displayLinkTargetClass()), and test/unit/objc_names.mm constructs a second registration of each,
// as a second binary of the same product would. Objective-C++ only, and it must precede every JUCE header of the
// translation unit: it turns on JUCE's Objective-C helpers (juce_core.h:376-379).

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/message.h>                             // objc_msgSendSuper, as juce_BasicNativeHeaders.h has it
#import <objc/runtime.h>                             // objc_allocateClassPair, class_addMethod, object_*InstanceVariable

#if defined(JUCE_CORE_H_INCLUDED) && !defined(JUCE_CORE_INCLUDE_OBJC_HELPERS)
  #error "src/gpu/ObjcClasses.h must precede every JUCE header (it enables JUCE_CORE_INCLUDE_OBJC_HELPERS)"
#endif
#ifndef JUCE_CORE_INCLUDE_OBJC_HELPERS
  #define JUCE_CORE_INCLUDE_OBJC_HELPERS 1
#endif
#include <juce_core/juce_core.h>

#include <funkgui/core/Config.h>
#include <funkgui/gpu/DisplayLink.h>

namespace funkgui::objc
{
    // The readable roots; the runtime name appends a random 64-bit hex suffix (JUCE's ObjCClass).
    inline constexpr const char* kRenderViewRoot = FUNKGUI_OBJC_PREFIX_STR "RenderView_";
    inline constexpr const char* kDisplayLinkTargetRoot = FUNKGUI_OBJC_PREFIX_STR "DisplayLinkTarget_";

    // The render view (HR NativeSurface.mm): a layer-backed, click-through (hitTest: -> nil), flipped NSView that
    // bgfx attaches its CAMetalLayer to; JUCE's peer view keeps receiving every event.
    struct RenderViewClass final : juce::ObjCClass<NSView>
    {
        RenderViewClass() : ObjCClass(kRenderViewRoot)
        {
            addMethod(@selector(hitTest:), [](id, SEL, NSPoint) -> NSView* { return nil; });
            addMethod(@selector(isFlipped), [](id, SEL) -> BOOL { return YES; });
            addMethod(@selector(wantsUpdateLayer), [](id, SEL) -> BOOL { return YES; });
            registerClass();
        }
    };

    // What a display-link target calls; held by the target's "state" ivar, owned by DisplayLink.mm's handle.
    struct DisplayLinkState
    {
        DisplayLinkCallback cb = nullptr;
        void* user = nullptr;
    };

    // The CADisplayLink target (HR DisplayLink.mm): -step: forwards the link's target presentation time.
    struct DisplayLinkTargetClass final : juce::ObjCClass<NSObject>
    {
        static constexpr const char* kStateIvar = "state";

        DisplayLinkTargetClass() : ObjCClass(kDisplayLinkTargetRoot)
        {
            addIvar<DisplayLinkState*>(kStateIvar);
            addMethod(@selector(step:), [](id self, SEL, CADisplayLink* sender) {
                auto* s = juce::getIvar<DisplayLinkState*>(self, kStateIvar);
                if (s != nullptr && s->cb != nullptr)
                    s->cb(s->user, sender.targetTimestamp);
            });
            registerClass();
        }
    };

    // This binary's registrations: made on first use, kept for the life of the image (function-local statics in
    // NativeSurface.mm and DisplayLink.mm).
    RenderViewClass& renderViewClass();
    DisplayLinkTargetClass& displayLinkTargetClass();
}

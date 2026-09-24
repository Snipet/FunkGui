#include "ObjcClasses.h"                             // first: JUCE's Objective-C helpers (juce::ObjCClass)

#include <funkgui/gpu/DisplayLink.h>

// The CADisplayLink wrapper (HR DisplayLink.mm), its target class now registered at run time under a randomised name
// (G7; 02 §1.8, K2 #16): see ObjcClasses.h.
//
// Compiled without ARC (JUCE's default), so the retain/release below is deliberate. CADisplayLink retains its target;
// -invalidate removes the link from the run loop and releases that retain, which is what breaks the cycle. The handle
// is a C++ object that owns the target (+1), the link (+1) and the target's DisplayLinkState.

namespace funkgui
{
    namespace objc
    {
        DisplayLinkTargetClass& displayLinkTargetClass()
        {
            static DisplayLinkTargetClass cls;       // one registration per binary (JUCE's idiom)
            return cls;
        }
    }

    namespace
    {
        struct LinkHandle
        {
            objc::DisplayLinkState state;
            id target = nil;
            CADisplayLink* link = nil;
        };

        void detachState(id target)
        {
            object_setInstanceVariable(target, objc::DisplayLinkTargetClass::kStateIvar, nullptr);
        }
    }

    void* createDisplayLink(void* nsView, DisplayLinkCallback cb, void* user)
    {
        NSView* v = (NSView*) nsView;
        if (v == nil || [v window] == nil)
            return nullptr;   // no screen to sync to yet

        auto* h = new LinkHandle();
        h->state.cb = cb;
        h->state.user = user;
        h->target = [objc::displayLinkTargetClass().createInstance() init];
        object_setInstanceVariable(h->target, objc::DisplayLinkTargetClass::kStateIvar, &h->state);

        // Per-view rather than per-screen: the link then follows the window
        // when it is dragged to a display with a different refresh rate,
        // which is exactly the case a fixed-rate timer gets wrong.
        CADisplayLink* link = [v displayLinkWithTarget: h->target selector: @selector(step:)];
        if (link == nil)
        {
            detachState(h->target);
            [h->target release];
            delete h;
            return nullptr;
        }

        h->link = [link retain];
        // Common modes so the UI keeps animating through menu tracking and
        // live window resizes, which run the run loop in a modal mode.
        [link addToRunLoop: [NSRunLoop mainRunLoop] forMode: NSRunLoopCommonModes];
        return h;
    }

    void destroyDisplayLink(void* handle)
    {
        if (handle == nullptr) return;
        auto* h = static_cast<LinkHandle*>(handle);
        h->state.cb = nullptr;
        h->state.user = nullptr;
        [h->link invalidate];   // off the run loop; releases the link's retain on the target
        [h->link release];
        detachState(h->target);
        [h->target release];
        delete h;
    }

    void setDisplayLinkRate(void* handle, float minHz, float maxHz, float preferredHz)
    {
        if (handle == nullptr) return;
        auto* h = static_cast<LinkHandle*>(handle);
        if (h->link == nil) return;
        h->link.preferredFrameRateRange = CAFrameRateRangeMake(minHz, maxHz, preferredHz);
    }

    const char* displayLinkTargetClassName()
    {
        return class_getName(objc::displayLinkTargetClass().cls);
    }
}

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

#include "DisplayLink.h"

// Compiled without ARC (JUCE default), so the retain/release dance below is
// deliberate. CADisplayLink retains its target and the target retains the
// link, so -invalidate is what breaks the cycle; nothing else will.
@interface HrvbDisplayLinkTarget : NSObject
{
@public
    hrvbgui::DisplayLinkCallback cb;
    void* user;
}
@property (nonatomic, retain) CADisplayLink* link;
@end

@implementation HrvbDisplayLinkTarget

- (void) step: (CADisplayLink*) sender
{
    if (cb != nullptr)
        cb(user, sender.targetTimestamp);
}

@end

namespace hrvbgui
{
    void* createDisplayLink(void* nsView, DisplayLinkCallback cb, void* user)
    {
        NSView* v = (NSView*) nsView;
        if (v == nil || [v window] == nil)
            return nullptr;   // no screen to sync to yet

        HrvbDisplayLinkTarget* t = [[HrvbDisplayLinkTarget alloc] init];
        t->cb   = cb;
        t->user = user;

        // Per-view rather than per-screen: the link then follows the window
        // when it is dragged to a display with a different refresh rate,
        // which is exactly the case a fixed-rate timer gets wrong.
        CADisplayLink* link = [v displayLinkWithTarget: t selector: @selector(step:)];
        if (link == nil)
        {
            [t release];
            return nullptr;
        }

        t.link = link;
        // Common modes so the UI keeps animating through menu tracking and
        // live window resizes, which run the run loop in a modal mode.
        [link addToRunLoop: [NSRunLoop mainRunLoop] forMode: NSRunLoopCommonModes];
        return (void*) t;
    }

    void destroyDisplayLink(void* handle)
    {
        if (handle == nullptr) return;
        HrvbDisplayLinkTarget* t = (HrvbDisplayLinkTarget*) handle;
        t->cb   = nullptr;
        t->user = nullptr;
        [t.link invalidate];   // releases the link's retain on t
        t.link = nil;
        [t release];
    }

    void setDisplayLinkRate(void* handle, float minHz, float maxHz, float preferredHz)
    {
        if (handle == nullptr) return;
        HrvbDisplayLinkTarget* t = (HrvbDisplayLinkTarget*) handle;
        if (t.link == nil) return;
        t.link.preferredFrameRateRange =
            CAFrameRateRangeMake(minHz, maxHz, preferredHz);
    }
}

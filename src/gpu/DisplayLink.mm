#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

#include <funkgui/gpu/DisplayLink.h>

#include <funkgui/core/Config.h>   // FUNKGUI_OBJC_NAME

// Compiled without ARC (JUCE default), so the retain/release dance below is
// deliberate. CADisplayLink retains its target and the target retains the
// link, so -invalidate is what breaks the cycle; nothing else will.
@interface FUNKGUI_OBJC_NAME(DisplayLinkTarget) : NSObject
{
@public
    funkgui::DisplayLinkCallback cb;
    void* user;
}
@property (nonatomic, retain) CADisplayLink* link;
@end

@implementation FUNKGUI_OBJC_NAME(DisplayLinkTarget)

- (void) step: (CADisplayLink*) sender
{
    if (cb != nullptr)
        cb(user, sender.targetTimestamp);
}

@end

namespace funkgui
{
    void* createDisplayLink(void* nsView, DisplayLinkCallback cb, void* user)
    {
        NSView* v = (NSView*) nsView;
        if (v == nil || [v window] == nil)
            return nullptr;   // no screen to sync to yet

        FUNKGUI_OBJC_NAME(DisplayLinkTarget)* t = [[FUNKGUI_OBJC_NAME(DisplayLinkTarget) alloc] init];
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
        FUNKGUI_OBJC_NAME(DisplayLinkTarget)* t = (FUNKGUI_OBJC_NAME(DisplayLinkTarget)*) handle;
        t->cb   = nullptr;
        t->user = nullptr;
        [t.link invalidate];   // releases the link's retain on t
        t.link = nil;
        [t release];
    }

    void setDisplayLinkRate(void* handle, float minHz, float maxHz, float preferredHz)
    {
        if (handle == nullptr) return;
        FUNKGUI_OBJC_NAME(DisplayLinkTarget)* t = (FUNKGUI_OBJC_NAME(DisplayLinkTarget)*) handle;
        if (t.link == nil) return;
        t.link.preferredFrameRateRange =
            CAFrameRateRangeMake(minHz, maxHz, preferredHz);
    }
}

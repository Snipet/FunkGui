#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

#include "NativeSurface.h"

// Layer-backed, click-through child view. bgfx replaces/attaches the Metal
// layer; JUCE's flipped peer view keeps handling all input.
@interface HrvbRenderView : NSView
@end

@implementation HrvbRenderView
- (NSView*) hitTest:(NSPoint) point { (void) point; return nil; }
- (BOOL) isFlipped { return YES; }
- (BOOL) wantsUpdateLayer { return YES; }
@end

namespace hrvbgui
{
    // Compiled without ARC (JUCE default): the pointer we hand back owns the
    // +1 from alloc, released in destroyRenderView.
    void* createRenderView(void* parentNSView, int x, int y, int w, int h)
    {
        NSView* parent = (NSView*) parentNSView;
        if (parent == nil) return nullptr;

        HrvbRenderView* v = [[HrvbRenderView alloc]
            initWithFrame: NSMakeRect(x, y, w, h)];
        [v setWantsLayer: YES];
        // JUCE's peer view is flipped, so child frames use top-left origin
        // coordinates directly.
        [parent addSubview: v];
        return (void*) v;
    }

    void setRenderViewFrame(void* view, int x, int y, int w, int h)
    {
        if (view == nullptr) return;
        NSView* v = (NSView*) view;
        [v setFrame: NSMakeRect(x, y, w, h)];
    }

    void destroyRenderView(void* view)
    {
        if (view == nullptr) return;
        NSView* v = (NSView*) view;
        [v removeFromSuperview];
        [v release];
    }

    double getBackingScale(void* view)
    {
        if (view == nullptr) return 1.0;
        NSView* v = (NSView*) view;
        NSWindow* w = [v window];
        return w != nil ? [w backingScaleFactor] : 1.0;
    }
}

#include "ObjcClasses.h"                             // first: JUCE's Objective-C helpers (juce::ObjCClass)

#include <funkgui/gpu/NativeSurface.h>

// The render view (HR NativeSurface.mm), now registered at run time under a randomised name (G7; 02 §1.8, K2 #16):
// see ObjcClasses.h. Compiled without ARC (JUCE's default): the pointer createRenderView hands back owns the +1 of the
// allocation, released in destroyRenderView.

namespace funkgui
{
    namespace objc
    {
        RenderViewClass& renderViewClass()
        {
            static RenderViewClass cls;              // one registration per binary (JUCE's idiom)
            return cls;
        }
    }

    void* createRenderView(void* parentNSView, int x, int y, int w, int h)
    {
        NSView* parent = (NSView*) parentNSView;
        if (parent == nil) return nullptr;

        NSView* v = [objc::renderViewClass().createInstance() initWithFrame: NSMakeRect(x, y, w, h)];
        if (v == nil) return nullptr;
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

    void setRenderViewScale(void*, double)
    {
        // The CAMetalLayer scales any drawable into the view's points (v0.11.0; NativeSurface.h).
    }

    void* nativeDisplay()
    {
        return nullptr;                              // Metal needs no display connection (v0.11.0)
    }

    const char* renderViewClassName()
    {
        return class_getName(objc::renderViewClass().cls);
    }
}

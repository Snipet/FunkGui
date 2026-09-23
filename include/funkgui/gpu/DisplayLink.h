#pragma once

// Thin C interface over CADisplayLink, attached to a view so it follows
// whichever display that view is actually on. Callbacks arrive on the main
// thread, already in phase with the compositor.
//
// The alternative — a juce::Timer at a fixed 60 Hz — free-runs against the
// refresh rate. On a 120 Hz ProMotion display the two beat against each other
// and every animation judders; on any display it means presenting at a moment
// the compositor did not ask for.

namespace hrvbgui
{
    // timestampSec is the display link's target presentation time, which is
    // the correct clock to advance animation against.
    using DisplayLinkCallback = void (*)(void* user, double timestampSec);

    // Returns nullptr if the view is not yet in a window (and therefore has no
    // screen to sync to). The caller should retry later.
    void* createDisplayLink(void* nsView, DisplayLinkCallback cb, void* user);
    void  destroyDisplayLink(void* link);

    // Ask the system for a cadence. On a variable-refresh display this is what
    // lets an idle panel drop to a low rate instead of waking the GPU 120
    // times a second to redraw an identical frame. Fixed-refresh displays
    // ignore it.
    void  setDisplayLinkRate(void* link, float minHz, float maxHz, float preferredHz);
}

#pragma once

// What a modulated control draws on top of its value (FSynth: routes from patch data, the live value from telemetry).
// Plain data the product owns and refreshes; a widget re-reads it only when `key` changes. Positions are in the
// widget's drawn track space 0..1 (ValueView::track), so a Mode's DisplayMap and a stepped role's snapping are the
// product's business. Drawn in the theme's `signal` token, which a product names as its modulation hue; `editing`
// swaps the edited band to `accent` (the thing under the hand).
//
// FSynth G2 (FSynth docs/design/02-funkgui-and-ui.md §4.1, §14.1, §14.3). Read by Dial::setMod and RuleSlider::setMod:
// - The widget keeps a pointer and a copy: it copies the overlay at setMod() and again whenever `key` differs from the
//   copy's, in tick() and at the start of every input call, as it re-reads its ValueModel's view; draw(), the a11y item
//   and the hit tests use the copy. settled() is false while the pointed-to key differs from the copy's, so a product
//   that changes a field must change `key` too, and gets one more frame.
// - The widget sanitises the copy: lo, hi, live, editLo and editHi are clamped to 0..1 (a NaN counts as 0) and each
//   pair is put in order, so a band never runs backwards.
// - Nothing is drawn while `routes` <= 0. Otherwise the band runs from lo to hi (a band with lo == hi is a 2 px mark:
//   a route at depth 0 is still seen), in `signal`, or in ink16 when `inert`; while `editing`, the edited route's band
//   from editLo to editHi is drawn over it in `accent`; the live value is drawn when `liveValid` and not `inert`, as a
//   live primitive (tag MOD_DOT), so geometry fingerprints leave telemetry out.
// - The widget's a11y description gains "modulated by <n> route(s)" (", no effect" when inert); the range in display
//   units is the product's to add, as it rewrites the slider's range (02 §14.1).
//
// Linux, the JUCE-free builds and a web host: plain data, the same on every platform.

#include <cstdint>

namespace funkgui
{
    struct ModOverlay
    {
        int      routes = 0;             // routes reaching the control; 0 draws nothing
        float    lo = 0.0f, hi = 0.0f;   // the reachable range, 0 <= lo <= hi <= 1
        float    live = 0.0f;            // the modulated position now
        bool     liveValid = false;      // draw the live dot (fresh telemetry and the target is in the frame)
        bool     inert = false;          // the routes have no effect here (a locked, derived or n/a value): ink16
        bool     editing = false;        // a depth edit is under the hand: the edited band draws in accent
        float    editLo = 0.0f, editHi = 0.0f;   // the edited route's own band (used while editing)
        uint64_t key = 0;                // changes whenever any field would
    };
}

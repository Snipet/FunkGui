#pragma once

// F1: the TextEditor's wheel rule and scrollbar geometry (widgets/TextEditor.h), public, so a list or any other view
// that scrolls moves and draws as the TextEditor does. src/widgets/TextEditor.cpp uses these itself: there is one
// definition. Plain arithmetic in logical px; no JUCE, every platform.
//
// The wheel: WheelEvent::dy (and dx) is JUCE's delta. A smooth one (a trackpad, a precise wheel) moves
// kWheelPxPerUnit px per unit of delta; a notched one moves kRowsPerNotch rows per notch of kWheelNotch. A view
// subtracts wheelPx(e.dy, e.smooth, rowHeight) from its offset (a positive dy scrolls up, toward offset 0) and clamps.
//
// The bar: kBarW wide, kBarInset from the view's edge, its thumb at least kBarMin long; drawn as an rrect of radius
// kBarW / 2 (ink32, ink52 while dragged) only while the content overflows the view.

namespace funkgui::scroll
{
    inline constexpr float kWheelPxPerUnit = 512.0f;          // smooth: JUCE's precise device gives 0.5/256 per point
    inline constexpr float kWheelNotch = 50.0f / 256.0f;      // one notch of a notched wheel (JUCE on X11)
    inline constexpr float kRowsPerNotch = 3.0f;
    inline constexpr float kBarW = 4.0f;
    inline constexpr float kBarInset = 2.0f;
    inline constexpr float kBarMin = 12.0f;

    // The px a wheel delta scrolls: delta * kWheelPxPerUnit when smooth, else delta notches of kRowsPerNotch rows of
    // rowHeight px. The sign is the delta's.
    constexpr float wheelPx(float delta, bool smooth, float rowHeight) noexcept
    {
        return delta * (smooth ? kWheelPxPerUnit : kRowsPerNotch * rowHeight / kWheelNotch);
    }

    // A thumb along its track, both measured from the track's start.
    struct Thumb
    {
        float pos = 0.0f;
        float size = 0.0f;
    };

    // The thumb of a track `area` px long for `content` px of which `view` px are in view, scrolled `offset` px
    // (0 .. content - view): its size is area * view / content, at least kBarMin and at most area; it sits at
    // offset / (content - view) of the room the track leaves it (at 0 when nothing can scroll).
    constexpr Thumb thumb(float area, float content, float view, float offset) noexcept
    {
        const float c = content > 1.0f ? content : 1.0f;
        const float wanted = area * view / c;
        const float atLeast = wanted > kBarMin ? wanted : kBarMin;
        const float size = atLeast < area ? atLeast : area;
        const float range = content - view > 0.0f ? content - view : 0.0f;
        return { (range > 0.0f ? offset / range : 0.0f) * (area - size), size };
    }
}

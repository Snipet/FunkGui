#pragma once

// Logical-pixel geometry shared by the recorder, the panel and the widgets (02 §3–§5). A Panel lays out in absolute
// logical px of a fixed-size window (HR rule: no resize, no transform stack), so a point and an axis-aligned rectangle
// are all it needs. Plain aggregates: no JUCE types cross the Core API.

namespace funkgui
{
    struct Point
    {
        float x = 0.0f, y = 0.0f;
    };

    struct Rect
    {
        float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;

        constexpr float right() const noexcept   { return x + w; }
        constexpr float bottom() const noexcept  { return y + h; }
        constexpr float centreX() const noexcept { return x + w * 0.5f; }
        constexpr float centreY() const noexcept { return y + h * 0.5f; }
        constexpr bool  isEmpty() const noexcept { return !(w > 0.0f && h > 0.0f); }

        // Half-open, as juce::Rectangle<float>::contains (HR's hit tests): the left and top edges are inside, the right
        // and bottom edges are not, so two rectangles that share an edge never both claim a point.
        constexpr bool contains(Point p) const noexcept
        {
            return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h;
        }

        // Each edge moved inwards by d (outwards for a negative d), as juce::Rectangle::reduced: the width and height
        // never go below 0. drawFocusRing draws on hit.reduced(1) (HR BgfxEditor.cpp:1542-1551).
        constexpr Rect reduced(float d) const noexcept
        {
            const float nw = w - 2.0f * d, nh = h - 2.0f * d;
            return { x + d, y + d, nw > 0.0f ? nw : 0.0f, nh > 0.0f ? nh : 0.0f };
        }

        constexpr Rect expanded(float d) const noexcept { return reduced(-d); }
    };
}

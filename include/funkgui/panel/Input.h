#pragma once

// Input as plain structs (02 §3.5): EditorHost converts JUCE's events into these, HeadlessHost synthesises them, and a
// Panel never sees a juce::MouseEvent. Coordinates are logical px in the Panel's own space.

#include <cstdint>

namespace funkgui
{
    struct Mods
    {
        bool shift = false, cmd = false, alt = false, ctrl = false;
    };

    struct PointerEvent
    {
        float x = 0.0f, y = 0.0f;
        Mods  mods{};
        int   clicks = 1;
        bool  popup = false;                     // right-click or ctrl-click: the host menu, never a write
    };

    struct WheelEvent
    {
        float x = 0.0f, y = 0.0f;
        float dx = 0.0f, dy = 0.0f;              // JUCE's MouseWheelDetails deltas
        bool  smooth = false, reversed = false, inertial = false;
        Mods  mods{};
    };

    enum class Key : uint8_t { character, tab, up, down, left, right, pageUp, pageDown, home, end,
                               escape, enter, backspace, del, space };

    struct KeyEvent
    {
        Key      key = Key::character;
        Mods     mods{};
        char32_t ch = 0;                         // the character, for Key::character
    };

    enum class Cursor : uint8_t { normal, leftRight, upDown, pointingHand, crosshair };
}

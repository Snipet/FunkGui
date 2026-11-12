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

    // F1: the mouse button of a press, its drags and its release. A host that cannot tell (a move with no button
    // down, an old HeadlessHost call) says left. A Panel that never reads it behaves as before: EditorHost delivered a
    // middle press as a left one, and still delivers it, now with Button::middle. popup is unchanged and independent:
    // a right press is Button::right and popup; a ctrl-click is popup with Button::left.
    enum class Button : uint8_t { left, middle, right };

    struct PointerEvent
    {
        float  x = 0.0f, y = 0.0f;
        Mods   mods{};
        int    clicks = 1;
        bool   popup = false;                    // right-click or ctrl-click: the host menu, never a write
        Button button = Button::left;            // F1: see Button
    };

    struct WheelEvent
    {
        float x = 0.0f, y = 0.0f;
        float dx = 0.0f, dy = 0.0f;              // JUCE's MouseWheelDetails deltas
        bool  smooth = false, reversed = false, inertial = false;
        Mods  mods{};
    };

    // F1: a trackpad's pinch (JUCE's mouseMagnify). scale is the factor since the last event: > 1 opens (zoom in),
    // < 1 closes. A browser reports a pinch as a wheel with Ctrl, so a web host never sends one.
    struct MagnifyEvent
    {
        float x = 0.0f, y = 0.0f;
        float scale = 1.0f;
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

    // What Panel::cursor() asks its host to show. The last four are FSynth G1's, appended: the first five keep their
    // numbers. grab: an open hand over something that can be picked up. grabbing: the closed hand while it is carried.
    // copy: a drop here adds (an arrow with a plus). notAllowed: a drop here is refused.
    // EditorHost on macOS shows the system's cursors for all four. EditorHost on Linux shows JUCE's own hand for grab
    // and for grabbing and its arrow-with-plus for copy, and keeps the normal arrow for notAllowed (JUCE reaches no
    // closed-hand or not-allowed cursor there), so a refusal must also show in what is drawn. A web host shows the
    // CSS cursors grab, grabbing, copy and not-allowed. HeadlessHost shows nothing and reports the value
    // (HeadlessHost::cursor()).
    enum class Cursor : uint8_t { normal, leftRight, upDown, pointingHand, crosshair,
                                  grab, grabbing, copy, notAllowed };
}

#pragma once

// The accessibility model (02 §5.6): a Panel lists one A11yItem per operable or readable thing, A11yBridge (Gpu, G7)
// turns them into invisible juce::Components for VoiceOver, and probes compare them as text through a11yDumpLine
// (C's G4, `ui.a11y.<view>` `lines` sidecars). Stepped sliders live in index space {0..n-1, step 1} (F §8.2): JUCE's
// macOS increment is current + interval, so HR's 0..1 / 0.01 range never reached the next detent.

#include <funkgui/core/Geometry.h>

#include <cstdint>
#include <string>

namespace funkgui
{
    enum class A11yRole : uint8_t { slider, toggleButton, button, radioGroup, radioButton, comboBox,
                                    listItem, staticText, image, progressBar };

    enum class A11yAction : uint8_t { press, toggle, setValue, increment, decrement, showMenu, focus };

    struct A11yItem
    {
        uint32_t    id = 0;
        uint32_t    parent = 0;                  // 0 = top level; a radioButton's radioGroup otherwise
        A11yRole    role = A11yRole::staticText;
        Rect        bounds{};                    // logical px, the item's hit rectangle
        std::string title, description, help, value;   // value = the spoken current value
        double      v = 0, lo = 0, hi = 1, step = 0;     // value interface; stepped = index space (F §8.2)
        bool        enabled = true, readOnly = false, visible = true, checkable = false, checked = false;
    };

    // One line of the a11y dump (HR BgfxEditor.cpp:484-501, extended), fields separated by one space:
    //
    //   <role:2> <title:24> <x,y,w,h:20> value=<value>|checked|unchecked [ro] [off] help=<help>
    //
    // - role: the A11yRole's number, right-aligned in 2 columns (HR printed the JUCE role number the same way).
    // - title, then the bounds "x,y,w,h", each left-aligned and padded with spaces to 24 and 20 columns (counted in
    //   codepoints); a longer field is written whole and pushes the rest right.
    // - then "checked" or "unchecked" for a checkable item, else "value=<value>" when value is not empty; " [ro]" when
    //   readOnly; " [off]" when !enabled; " help=<help>" when help is not empty. Absent parts leave no space.
    // - Numbers are plain decimals (no exponent, ASCII '-', no locale) with the fewest decimals, at most 9, that give
    //   back the same float: "38.5", "120", "0.1". A CR, LF or tab inside a string is written as a space, so the item
    //   is exactly one line.
    // - Padding never trails (a line with nothing after the bounds ends at the bounds) and there is no newline.
    //   `visible` is not printed: the dump's caller skips invisible items, which are not in the tree an assistive
    //   technology sees (HR).
    std::string a11yDumpLine(const A11yItem&);
}

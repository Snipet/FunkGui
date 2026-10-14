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

    // ---- FSynth G1 additions: six roles as typed constants, values 10 to 15 ------------------------------------------
    // group: a named container of other items (a node of a graph and its ports). list: a container of listItems.
    // table, row, cell: a grid with one Tab stop (the table fields of A11yItem below). editableText: a text field,
    // whose value is its text.
    // They are constants of the enum's type, not enumerators: a consumer's `switch` over A11yRole that names the ten
    // enumerators and has no `default` still compiles under -Wswitch and -Wswitch-enum with -Werror. The enum's
    // underlying type is fixed, so every uint8_t value is a valid A11yRole. Handle a constant with `==` before a
    // switch over the enumerators, or in that switch's `default`; never as a `case` label.
    // On Linux, without JUCE and on a web host they are the same data, and a11yDumpLine prints the same lines.
    namespace a11yrole
    {
        inline constexpr A11yRole group{ 10 }, list{ 11 }, table{ 12 }, row{ 13 }, cell{ 14 }, editableText{ 15 };
    }

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

        // ---- FSynth G1 additions: table geometry ---------------------------------------------------------------------
        // A `table` item gives the WHOLE table's size (its rows may be listed only while visible). A `row` item gives
        // its row index and has the table as its parent. An item whose parent is a row is a cell of that row and gives
        // its column; its role is `cell`, or the role of the control it is (a slider, a button, a toggle), which the
        // bridge still reports as the table's cell. Indices count from 0. Unset: 0, 0, -1, -1.
        int rows = 0, cols = 0;                  // table only
        int row  = -1;                           // row only
        int col  = -1;                           // cell only
    };

    // One line of the a11y dump (HR BgfxEditor.cpp:484-501, extended), fields separated by one space:
    //
    //   <role:2> <title:24> <x,y,w,h:20> value=<value>|checked|unchecked [ro] [off] help=<help>
    //     table=<rows>x<cols> row=<row> col=<col>
    //
    // - role: the A11yRole's number, right-aligned in 2 columns (HR printed the JUCE role number the same way); the
    //   six FSynth G1 roles print 10 to 15, with no shift of the line.
    // - title, then the bounds "x,y,w,h", each left-aligned and padded with spaces to 24 and 20 columns (counted in
    //   codepoints); a longer field is written whole and pushes the rest right.
    // - then "checked" or "unchecked" for a checkable item, else "value=<value>" when value is not empty; " [ro]" when
    //   readOnly; " [off]" when !enabled; " help=<help>" when help is not empty. Absent parts leave no space.
    // - then (FSynth G1) " table=<rows>x<cols>" when rows > 0 or cols > 0, " row=<row>" when row >= 0 and
    //   " col=<col>" when col >= 0, whatever the role: plain integers. An item that sets none of them prints the line
    //   it always did.
    // - Numbers are plain decimals (no exponent, ASCII '-', no locale) with the fewest decimals, at most 9, that give
    //   back the same float: "38.5", "120", "0.1". A CR, LF or tab inside a string is written as a space, so the item
    //   is exactly one line.
    // - Padding never trails (a line with nothing after the bounds ends at the bounds) and there is no newline.
    //   `visible` is not printed: the dump's caller skips invisible items, which are not in the tree an assistive
    //   technology sees (HR).
    std::string a11yDumpLine(const A11yItem&);
}

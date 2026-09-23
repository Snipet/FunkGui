#pragma once

// The seven preset colour tags, per theme. Not theme tokens: a tag's colour is
// the user's meaning ("red = vocals"), so it must stay recognisably the same
// hue in both themes — only its lightness moves, to hold contrast against the
// ground. Chosen as a set: seven hues far enough apart to tell at a 5px dot,
// and none of them the accent's exact orange, which already means "the control
// under the hand".

#include "Col.h"

namespace funkgui
{
    inline Col tagColour(int themeIdx, int tag)
    {
        static constexpr Col dark[7] = {
            { 0xE5, 0x48, 0x4D }, { 0xF5, 0x9E, 0x3B }, { 0xF0, 0xD0, 0x45 },
            { 0x4C, 0xB0, 0x6A }, { 0x3E, 0x96, 0xF4 }, { 0xA7, 0x70, 0xE0 },
            { 0x95, 0x98, 0xA0 } };
        static constexpr Col light[7] = {
            { 0xCE, 0x2C, 0x31 }, { 0xD8, 0x7A, 0x12 }, { 0xB8, 0x92, 0x00 },
            { 0x2A, 0x8F, 0x4E }, { 0x1B, 0x6F, 0xD6 }, { 0x7E, 0x4B, 0xC0 },
            { 0x74, 0x77, 0x7E } };
        const int t = tag < 0 ? 0 : (tag > 6 ? 6 : tag);
        return themeIdx == 1 ? light[t] : dark[t];
    }
}

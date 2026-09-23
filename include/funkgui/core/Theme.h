#pragma once

#include "Col.h"

namespace funkgui
{
    // The whole visual system is nine ink levels plus two accents. Depth is
    // expressed only as ink percentage — there are no gradients, shadows or
    // bevels anywhere in this design, which is also exactly what the SDF
    // renderer draws perfectly.
    struct Theme
    {
        Col ground;      // window ground; also the clear colour
        Col ink100;      // values, live caps, active state
        Col ink70;       // rank shell strokes, hovered labels
        Col ink52;       // labels, units, section captions
        Col ink32;       // at-default values, inactive numerals, sub-readouts
        Col ink16;       // datum rule, tracks, ghost strokes
        Col accent;      // the control under the hand. Nothing else.
        Col accentDim;   // the drag/hover track fill
        Col signal;      // reserved for exactly one job, which the product names (HR's `ice`, 02 §2.3; same values)
        float textGamma; // sRGB blend correction; sign depends on polarity

        static constexpr Theme graphite()
        {
            return { { 0x16, 0x17, 0x1A }, { 0xEC, 0xEA, 0xE4 },
                     { 0xB4, 0xB8, 0xBE }, { 0x8A, 0x8E, 0x95 },
                     { 0x57, 0x5C, 0x63 }, { 0x33, 0x37, 0x3C },
                     { 0xFF, 0x5A, 0x1F }, { 0x7A, 0x3A, 0x22 },
                     { 0x7F, 0xD4, 0xE8 }, 1.0f / 1.4f };
        }

        // The light counterpart is a pure token swap: nothing in the panel
        // code reads a colour any other way. The ink ladder still runs from
        // most contrast (ink100) to least (ink16) — on this ground that means
        // darkest to lightest, the inverse of Graphite — so every widget state
        // keeps its relative weight without a single conditional.
        static constexpr Theme paper()
        {
            return { { 0xED, 0xEB, 0xE6 }, { 0x17, 0x18, 0x1A },
                     { 0x5B, 0x5E, 0x62 }, { 0x77, 0x77, 0x76 },
                     { 0xB0, 0xAF, 0xAB }, { 0xD3, 0xD2, 0xCE },
                     { 0xE8, 0x45, 0x1A }, { 0xF2, 0xBB, 0xA6 },
                     // textGamma inverts: coverage composites in gamma-encoded
                     // space, so where light-on-dark needed fattening,
                     // dark-on-light needs exactly the opposite.
                     { 0x1E, 0x7E, 0x96 }, 1.4f };
        }

        static constexpr Theme byIndex(int i)
        {
            return i == 1 ? paper() : graphite();
        }

        static constexpr int kCount = 2;
        static constexpr const char* name(int i) { return i == 1 ? "PAPER" : "GRAPHITE"; }
    };
}

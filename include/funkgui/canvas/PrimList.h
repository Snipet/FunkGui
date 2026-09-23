#pragma once

// A recorded frame (02 §3.2): what Canvas::end() returns, what BgfxSink submits, what the dump (§3.8) writes and parses
// and what the fingerprint (§3.9) hashes. Declared in G2 (v0.2.0, frozen at FZ1); clear(), writeText() and parseText()
// are implemented with the recorder (G3).

#include <funkgui/canvas/Axis.h>
#include <funkgui/canvas/Prim.h>
#include <funkgui/core/Col.h>

#include <cstdint>
#include <cstdio>
#include <string_view>
#include <vector>

namespace funkgui
{
    struct FrameInfo
    {
        int      logicalW = 0, logicalH = 0;
        float    dpi = 1.0f;                  // physical / logical height (HR)
        Col      clear{};                     // the theme's ground
        float    textGamma = 1.0f;
        int      theme = 0;
        float    seconds = 0.0f;              // u_viewSize.w
        uint32_t frame = 0;
        float    dt = 0.0f;
        bool     fixedClock = false;          // dump "clock fixed": a HeadlessHost or a FIXED_DT capture
        // G2 additions for the dump's view line and overflow count (02 §3.8, §4.5), which FrameInfo did not carry:
        bool     displayLinked = false;       // "clock displaylink" (else "timer") when not fixedClock
        float    fps = 0.0f;                  // measured frame rate ("fps 0.0" for a fixed clock)
        bool     fullRate = false;            // "rate full" / "rate idle"
        uint32_t overflows = 0;               // BgfxSink drops so far ("overflow N"; 0 = no line)
    };

    struct PrimList
    {
        FrameInfo            info{};
        std::vector<Prim>    prims;
        std::vector<AxisRec> axes;
        uint32_t missingGlyphs = 0;           // codepoints not in the atlas (HR drops them silently, A §2.5)
        uint32_t missingFirst[8] = {};        // the first distinct missing codepoints, for the dump

        void clear();                         // empties everything, keeps the vectors' capacity
        bool writeText(std::FILE*) const;     // dump v2 (§3.8); false on a write error
        static bool parseText(std::string_view, PrimList&);   // accepts HR's v1 and v2; false on malformed input
    };
}

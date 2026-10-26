#pragma once

// Frame fingerprints (02 §3.9): FNV-1a hashes of a PrimList's geometry and text, with the live primitives left out and
// every float quantised, so a probe can pin a frame's layout as golden rows. A live capture and a headless frame of the
// same state give equal hashes (C G1).
//
// Declared in G2 (v0.2.0, frozen at FZ1); implemented by G3 (src/canvas/Fingerprint.cpp).

#include <funkgui/canvas/PrimList.h>
#include <funkgui/canvas/Tags.h>

#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace funkgui
{
    namespace test
    {
        class Probe;                             // FunkGui::harness (test/Harness.h)
    }

    struct FingerprintOptions
    {
        bool  excludeLive = true;                // flags bit 0 (pflag::live)
        bool  themeInvariant = true;             // text d1[2] (gamma) hashed as 0 -> hash(theme 0) == hash(theme 1)
        float quantum = 1.0f / 1024.0f;          // every float hashed as int32 lrint(v / quantum)
        bool  legacyHr = false;                  // HR FrameRender.cpp:108-150 algorithm, for HR's migration proof only
    };

    struct Fingerprint
    {
        uint64_t geometry = 0;                   // FNV-1a over non-text, non-live prims: x0 y0 x1 y1 d0[4] e0[2] d1 d2
        uint64_t text = 0;                       // the same fields for text prims
        int   statics = 0, live = 0, texts = 0, rrects = 0, segments = 0, areas = 0;
        float maxX = 0.0f, maxY = 0.0f;
        std::vector<std::pair<Tag, int>> tagCounts;   // ascending tag, tags that occur only
        // v0.19.0: shader draws (PrimList::shaders) hashed into `geometry`, after every primitive, in order. Each mixes
        // its id, its position `at`, its rect, whether it was clipped and its clip ({} when not), quantised as the
        // primitives' floats are; its params are not hashed (values, like colours) and its tag is counted in tagCounts.
        // A live one (Canvas::setLive) is left out under excludeLive and counted in `live` either way; the hashed ones
        // count here and stretch maxX / maxY to their rect's right and bottom edges. A frame without shader draws
        // hashes exactly as before v0.19.0. legacyHr ignores them (HR had none).
        int   shaders = 0;
    };

    Fingerprint fingerprint(const PrimList&, const FingerprintOptions& = {});

    // The fingerprint as golden rows of a Harness v2 probe: <prefix>.geometry and <prefix>.text as hash rows, the
    // counts and extents as exact num rows, and <prefix>.tag.<NAME> per tag. (02 §3.9 wrote this against a
    // test::Metric list; Harness v2, frozen at FZ0, records rows on the Probe itself, so the sink is the Probe.)
    // v0.19.0: <prefix>.shaders (exact) only when it is not 0, so a frame without shader draws has the rows it had.
    void addMetrics(test::Probe&, std::string_view prefix, const Fingerprint&);
}

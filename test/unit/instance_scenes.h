#pragma once

// The frames fg.canvas.instance and fg.gpu.instancing record (v0.26.0, primitives as instances; canvas/Expand.h): every
// kind of primitive the Canvas records, every colour byte in every channel of both colours, a heavy frame of 5,000
// primitives, and a frame split into runs by image draws. Not a test of its own (no FUNKGUI_TEST line): included by
// both, so the CPU proof and the GPU comparison cover the same primitives. JUCE-free.

#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/core/Col.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/core/TypeScale.h>

#include <cstddef>
#include <cstdint>

namespace instance_scenes
{
    using funkgui::Canvas;
    using funkgui::Col;

    inline constexpr int kW = 400, kH = 300;            // logical px of every scene
    inline constexpr int kStressPrims = 5000;
    inline constexpr Col kClear{ 18, 20, 24, 255 };

    inline funkgui::FrameInfo frame(float dpi)
    {
        funkgui::FrameInfo f;
        f.logicalW = kW;
        f.logicalH = kH;
        f.dpi = dpi;
        f.clear = kClear;
        f.textGamma = 0.714285731f;
        f.fixedClock = true;
        return f;
    }

    // A small deterministic generator (the same frames on every host).
    struct Lcg
    {
        uint32_t s = 0x2545F491u;
        uint32_t next() noexcept
        {
            s = s * 1664525u + 1013904223u;
            return s >> 8;                               // 24 bits
        }
        float unit() noexcept { return static_cast<float>(next()) / 16777216.0f; }   // [0, 1)
        float in(float lo, float hi) noexcept { return lo + (hi - lo) * unit(); }
        uint8_t byte() noexcept { return static_cast<uint8_t>(next() & 0xffu); }
    };

    // Every kind and variant: rrects (radii, borders, glow, a pill, corners of their own), hairlines, segments, text in
    // four styles and three alignments, areas with each stroked edge, a strip, a polyline, a disc with a ring, dots,
    // an arc, a cubic, live and clipped primitives, translucent colours.
    inline void kinds(Canvas& c)
    {
        const Col ink{ 0xEC, 0xEA, 0xE4, 0xFF }, fill{ 40, 90, 160, 255 }, edge{ 230, 120, 40, 200 };
        const Col glass{ 120, 200, 255, 96 }, warm{ 250, 180, 60, 255 };
        c.rrect(-4.0f, -4.0f, kW + 8.0f, kH + 8.0f, 0.0f, Col{ 30, 33, 38, 255 });
        c.rrect(12.25f, 7.5f, 93.0f, 38.75f, 6.0f, fill, 1.5f, edge);
        c.rrect(120.3f, 10.4f, 55.5f, 26.6f, 9999.0f, warm, 0.0f, {}, 2.75f);
        c.rrect4(190.1f, 8.2f, 80.3f, 40.4f, 0.0f, 3.0f, 12.5f, 1.25f, glass, 2.5f, ink, 0.3f);
        c.rrect(285.7f, 12.3f, 17.9f, 0.3f, 1.0f, ink, 0.0f, {}, 0.3f);
        c.hairlineH(10.0f, 60.3f, 380.0f, ink);
        c.hairlineV(230.7f, 56.1f, 60.0f, edge);
        c.segment(20.0f, 70.0f, 150.0f, 110.3f, 1.25f, ink, 0.5f);
        c.segment(160.0f, 70.0f, 160.0f, 120.0f, 3.0f, warm);
        c.segment(180.5f, 118.5f, 300.5f, 72.25f, 6.5f, glass, 1.0f);
        c.text("Ab 1\xC2\xB0x q\xE2\x86\x92|%()", 20.3f, 125.7f, funkgui::type::kLabel, ink);
        c.text("-0123.45", 200.0f, 122.0f, funkgui::type::kValueP, warm, funkgui::Align::centre);
        c.text("THRESHOLD 1:4", 390.5f, 150.25f, funkgui::type::kCaption, edge, funkgui::Align::right);
        c.text("88.8", 300.0f, 70.0f, funkgui::type::kDisplay, ink);
        c.area(20.0f, 60.0f, 200.0f, 180.0f, 230.0f, 228.0f, glass, 1.5f, ink, funkgui::AreaEdge::top);
        c.area(60.0f, 100.0f, 180.0f, 190.0f, 226.0f, 232.0f, fill, 1.0f, warm, funkgui::AreaEdge::bottom);
        c.area(100.0f, 140.0f, 190.0f, 175.0f, 232.0f, 220.0f, edge, 2.0f, ink, funkgui::AreaEdge::both);
        {
            const float xs[6] = { 150.0f, 170.0f, 190.0f, 210.0f, 230.0f, 250.0f };
            const float top[6] = { 200.0f, 185.0f, 192.0f, 170.0f, 188.0f, 181.0f };
            c.areaStrip(xs, 6, top, nullptr, 235.0f, glass, 1.25f, warm);
            const float ys[6] = { 250.0f, 262.0f, 244.0f, 270.0f, 255.0f, 248.0f };
            c.polyline(xs, ys, 6, 1.5f, ink);
        }
        c.disc(300.0f, 220.0f, 18.5f, fill, 2.0f, warm);
        c.dotted(260.0f, 280.0f, 120.0f, 4.0f, ink);
        c.arc({ 340.0f, 215.0f }, 30.0f, -2.4f, 0.8f, 3.0f, warm);
        {
            const funkgui::Point p[4] = { { 20.0f, 290.0f }, { 60.0f, 240.0f }, { 110.0f, 300.0f }, { 140.0f, 250.0f } };
            c.cubic(p, 2.0f, ink);
        }
        {
            const Canvas::Scope live(c, funkgui::tags::none, true);
            c.segment(200.0f, 290.0f, 260.0f, 260.0f, 2.0f, edge);
            c.rrect(270.0f, 250.0f, 20.0f, 30.0f, 3.0f, glass);
        }
        {
            const Canvas::ClipScope clip(c, { 330.0f, 240.0f, 40.0f, 40.0f });
            c.rrect(320.0f, 230.0f, 70.0f, 70.0f, 12.0f, warm, 3.0f, ink);
            c.text("clip", 325.0f, 255.0f, funkgui::type::kLabel, fill);
            c.segment(310.0f, 300.0f, 390.0f, 220.0f, 4.0f, glass);
        }
    }

    // Every byte in every channel of both colours: 256 opaque fills over 256 translucent ones, each with a border whose
    // channels run through every byte in other orders (multipliers coprime to 256), and segments in between.
    inline void colours(Canvas& c)
    {
        c.rrect(-4.0f, -4.0f, kW + 8.0f, kH + 8.0f, 0.0f, Col{ 64, 64, 64, 255 });
        for (int i = 0; i < 256; ++i)
        {
            const auto b = [i](int m, int k) { return static_cast<uint8_t>((i * m + k) & 255); };
            const float x = 6.0f + static_cast<float>(i % 32) * 12.0f, y = 6.0f + static_cast<float>(i / 32) * 18.0f;
            c.rrect(x, y, 10.0f, 7.0f, 2.0f, Col{ b(1, 0), b(255, 255), b(37, 11), 255 }, 1.5f,
                    Col{ b(11, 3), b(101, 7), b(53, 1), b(1, 0) });
            c.rrect(x, y + 8.0f, 10.0f, 7.0f, 0.0f, Col{ b(73, 5), b(3, 9), b(1, 0), b(1, 0) }, 2.0f,
                    Col{ b(255, 128), b(1, 0), b(29, 0), b(97, 255) });
            c.segment(x, y + 16.5f, x + 10.0f, y + 16.5f, 1.0f, Col{ b(1, 0), b(1, 0), b(1, 0), b(1, 0) });
        }
    }

    // 5,000 primitives over the whole view: rrects (some bordered, some soft), segments and glyphs, seeded.
    inline void stress(Canvas& c)
    {
        Lcg g;
        int n = 0;
        while (n < kStressPrims)
        {
            const uint32_t pick = g.next() % 100u;
            const Col col{ g.byte(), g.byte(), g.byte(), static_cast<uint8_t>(128 + g.byte() / 2) };
            const float x = g.in(-10.0f, kW + 10.0f), y = g.in(-10.0f, kH + 10.0f);
            if (pick < 60)
            {
                const float w = g.in(2.0f, 60.0f), h = g.in(2.0f, 40.0f);
                if (pick < 20)
                    c.rrect(x, y, w, h, g.in(0.0f, 12.0f), col, g.in(0.5f, 3.0f),
                            Col{ g.byte(), g.byte(), g.byte(), 255 });
                else if (pick < 25)
                    c.rrect(x, y, w, h, g.in(0.0f, 12.0f), col, 0.0f, {}, g.in(0.5f, 6.0f));
                else
                    c.rrect(x, y, w, h, g.in(0.0f, 12.0f), col);
                ++n;
            }
            else if (pick < 85)
            {
                c.segment(x, y, x + g.in(-80.0f, 80.0f), y + g.in(-80.0f, 80.0f), g.in(0.5f, 5.0f), col,
                          g.in(0.0f, 1.0f));
                ++n;
            }
            else
            {
                static constexpr const char* kWords[4] = { "gain", "-12.5 dB", "RATIO", "4:1" };
                const char* w = kWords[g.next() % 4u];
                const std::size_t before = c.end().prims.size();   // no clip is open: end() only reads the list
                c.text(w, x, y, (g.next() & 1u) != 0 ? funkgui::type::kLabel : funkgui::type::kCaption, col);
                n += static_cast<int>(c.end().prims.size() - before);
            }
        }
    }

    // Primitives split into three runs by two image draws (ids the GPU test registers; unregistered, they draw
    // nothing and still split the runs).
    inline constexpr uint32_t kSplitImageA = 7101, kSplitImageB = 7102;
    inline void split(Canvas& c)
    {
        const Col a{ 200, 60, 60, 255 }, b{ 60, 200, 90, 180 }, ink{ 240, 240, 240, 255 };
        c.rrect(-4.0f, -4.0f, kW + 8.0f, kH + 8.0f, 0.0f, Col{ 25, 28, 34, 255 });
        c.rrect(20.0f, 20.0f, 160.0f, 100.0f, 10.0f, a, 2.0f, ink);
        c.text("under", 30.0f, 40.0f, funkgui::type::kValueP, ink);
        c.image(kSplitImageA, { 60.0f, 50.0f, 120.0f, 90.0f }, 8.0f);
        c.rrect(100.0f, 90.0f, 160.0f, 100.0f, 0.0f, b, 1.0f, ink);
        c.segment(40.0f, 200.0f, 360.0f, 140.0f, 5.0f, ink, 0.5f);
        c.image(kSplitImageB, { 200.0f, 120.0f, 150.0f, 120.0f }, 0.0f, 0.75f);
        c.text("over", 220.0f, 200.0f, funkgui::type::kDisplay, a);
        c.rrect4(240.0f, 30.0f, 140.0f, 60.0f, 20.0f, 0.0f, 20.0f, 0.0f, b, 3.0f, a, 1.0f);
    }
}

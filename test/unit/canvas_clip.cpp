// FUNKGUI_TEST name=fg.canvas.clip timeout=300 gpu=0
//
// fg.canvas.clip (v0.9.0, Canvas::pushClip / popClip): a clip crops what was recorded inside it on the CPU, so every
// sample inside the clip draws as the unclipped frame's did and nothing draws outside. Spec rows only.
//
// - scene.*: one frame of every kind (a bordered rounded rrect, a hairline, a diagonal segment, a disc, text in two
//   styles, an AREA strip, dotted) recorded twice, plainly and inside a clip whose edges cut through all of them.
//   Rasterised by SoftRaster at dpi 1 and 2, supersample 1 and 2: each pixel inside the clip differs from the plain
//   frame's by at most 1/255 per channel, and at most 0.1 % of them by any amount (the moved corners interpolate in
//   another order); every pixel outside is the clear colour.
// - prims.*: a primitive wholly inside is kept bit for bit; one wholly outside is dropped; a cut one keeps its colours,
//   extents, per-kind data, tag and flags, and its corners land on the clip.
// - nest.*: an inner clip is intersected with the outer one; popping restores the outer clip for what follows.
// - depth.*: pushes beyond kMaxClips are ignored with their pops; an unmatched pop is harmless; end() closes the clips
//   left open and begin() forgets them; ClipScope pushes and pops; axis records are never clipped.

#include <funkgui/canvas/Axis.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/Prim.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/canvas/SoftRaster.h>
#include <funkgui/core/Col.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/test/Harness.h>
#include <funkgui/text/FontService.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace T = funkgui::test;
using funkgui::Canvas;
using funkgui::Col;
using funkgui::FrameInfo;
using funkgui::Image;
using funkgui::Prim;
using funkgui::PrimList;
using funkgui::Rect;

namespace
{
    constexpr int kW = 160, kH = 96;
    constexpr Col kClear { 10, 12, 14, 255 };
    constexpr Rect kClip { 20.0f, 17.0f, 101.0f, 43.0f };      // cuts every shape below; whole logical px

    FrameInfo frame(float dpi)
    {
        FrameInfo f;
        f.logicalW = kW;
        f.logicalH = kH;
        f.dpi = dpi;
        f.clear = kClear;
        f.textGamma = 0.714285731f;
        f.fixedClock = true;
        return f;
    }

    void scene(Canvas& c)
    {
        c.setTag(7);
        c.rrect(8.0f, 8.0f, 60.0f, 30.0f, 6.0f, Col{ 60, 70, 80, 255 }, 1.5f, Col{ 200, 210, 220, 255 });
        c.hairlineH(4.0f, 17.0f, 150.0f, Col{ 120, 120, 120, 255 });
        c.segment(10.0f, 70.0f, 140.0f, 12.0f, 2.0f, Col{ 240, 180, 60, 255 }, 0.5f);
        c.disc(118.0f, 58.0f, 9.0f, Col{ 90, 160, 200, 255 }, 1.0f, Col{ 250, 250, 250, 255 });
        c.text("CLIPPED ROWS 0123", 12.0f, 50.0f, funkgui::type::kLabel, Col{ 230, 234, 236, 255 });
        c.text("SCROLL", 70.0f, 12.0f, funkgui::type::kCaption, Col{ 150, 160, 170, 200 });
        const float xs[] = { 30.0f, 55.5f, 80.25f, 100.0f, 130.0f };
        const float top[] = { 40.0f, 30.0f, 52.0f, 44.0f, 20.0f };
        c.areaStrip(xs, 5, top, nullptr, 80.0f, Col{ 80, 120, 90, 160 }, 1.5f, Col{ 170, 230, 180, 255 });
        c.dotted(4.0f, 59.0f, 150.0f, 3.0f, Col{ 200, 200, 200, 255 });
    }

    const uint8_t* px(const Image& img, int x, int y)
    {
        return &img.rgba[(static_cast<size_t>(y) * static_cast<size_t>(img.w) + static_cast<size_t>(x)) * 4u];
    }

    bool samePrim(const Prim& a, const Prim& b) { return std::memcmp(&a, &b, sizeof(Prim)) == 0; }
    bool sameBits(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }
}

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;      // the atlas bakes through JUCE's font stack
    T::Probe P("fg.canvas.clip", "", argc, argv);

    auto& fonts = funkgui::FontService::get();
    const funkgui::FontAtlasSdf& atlas = fonts.atlas();
    if (!P.eq("font.ok", fonts.ok(), 1))
        return P.finish();
    Canvas canvas(atlas);

    // ---- scene: pixels inside equal the plain frame's, outside the clear colour --------------------------------------
    for (const float dpi : { 1.0f, 2.0f })
        for (const int ss : { 1, 2 })
        {
            const std::string key = "scene.dpi" + std::to_string(static_cast<int>(dpi)) + ".ss" + std::to_string(ss);
            canvas.begin(frame(dpi));
            scene(canvas);
            const PrimList plain = canvas.end();
            canvas.begin(frame(dpi));
            canvas.pushClip(kClip);
            scene(canvas);
            canvas.popClip();
            const PrimList clipped = canvas.end();
            const Image a = funkgui::rasterise(plain, atlas, ss);
            const Image b = funkgui::rasterise(clipped, atlas, ss);
            P.eq(key + ".same_size", a.w == b.w && a.h == b.h, 1);
            if (a.w != b.w || a.h != b.h)
                continue;
            int inside = 0, off1 = 0, offMore = 0, outsideLit = 0, drawnInside = 0;
            for (int y = 0; y < b.h; ++y)
                for (int x = 0; x < b.w; ++x)
                {
                    const float lx = (static_cast<float>(x) + 0.5f) / dpi, ly = (static_cast<float>(y) + 0.5f) / dpi;
                    const uint8_t* pa = px(a, x, y);
                    const uint8_t* pb = px(b, x, y);
                    if (kClip.contains({ lx, ly }))
                    {
                        ++inside;
                        int d = 0;
                        for (int k = 0; k < 4; ++k)
                            d = std::max(d, std::abs(static_cast<int>(pa[k]) - static_cast<int>(pb[k])));
                        off1 += d == 1 ? 1 : 0;
                        offMore += d > 1 ? 1 : 0;
                        drawnInside += pa[0] != kClear.r || pa[1] != kClear.g || pa[2] != kClear.b ? 1 : 0;
                    }
                    else
                        outsideLit += pb[0] != kClear.r || pb[1] != kClear.g || pb[2] != kClear.b || pb[3] != kClear.a
                                          ? 1 : 0;
                }
            P.ge(key + ".scene_drawn_inside", drawnInside, inside / 4);   // the clip really cuts through shapes
            P.eq(key + ".inside_within_1", offMore, 0);
            P.le(key + ".inside_off_by_1", off1, inside / 1000);
            P.eq(key + ".outside_clear", outsideLit, 0);
        }

    // ---- prims: kept, dropped, cut ------------------------------------------------------------------------------------
    {
        canvas.begin(frame(1.0f));
        canvas.setLive(true);
        canvas.setTag(9);
        canvas.rrect(30.0f, 25.0f, 20.0f, 10.0f, 2.0f, Col{ 1, 2, 3, 255 });           // inside
        canvas.rrect(130.0f, 70.0f, 10.0f, 10.0f, 0.0f, Col{ 4, 5, 6, 255 });          // outside
        canvas.rrect(10.0f, 10.0f, 50.0f, 30.0f, 3.0f, Col{ 7, 8, 9, 255 }, 1.0f, Col{ 10, 11, 12, 255 });   // cut
        const PrimList before = canvas.end();                                          // a copy
        canvas.begin(frame(1.0f));
        canvas.setLive(true);
        canvas.setTag(9);
        canvas.pushClip(kClip);
        canvas.rrect(30.0f, 25.0f, 20.0f, 10.0f, 2.0f, Col{ 1, 2, 3, 255 });
        canvas.rrect(130.0f, 70.0f, 10.0f, 10.0f, 0.0f, Col{ 4, 5, 6, 255 });
        canvas.rrect(10.0f, 10.0f, 50.0f, 30.0f, 3.0f, Col{ 7, 8, 9, 255 }, 1.0f, Col{ 10, 11, 12, 255 });
        P.eq("prims.depth_open", canvas.clipDepth(), 1);
        canvas.popClip();
        const PrimList& after = canvas.end();
        P.eq("prims.count", static_cast<int64_t>(after.prims.size()), 2);
        if (before.prims.size() == 3 && after.prims.size() == 2)
        {
            P.eq("prims.inside_bit_for_bit", samePrim(before.prims[0], after.prims[0]), 1);
            const Prim& o = before.prims[2];
            const Prim& n = after.prims[1];
            P.eq("prims.cut_corners_on_clip", sameBits(n.x0, kClip.x) && sameBits(n.y0, kClip.y)
                                                  && sameBits(n.x1, o.x1) && sameBits(n.y1, o.y1), 1);
            const bool flat = n.c0 == o.c0 && n.c1 == o.c1 && sameBits(n.d0[2], o.d0[2]) && sameBits(n.d0[3], o.d0[3])
                              && std::memcmp(n.d1, o.d1, sizeof n.d1) == 0 && std::memcmp(n.d2, o.d2, sizeof n.d2) == 0
                              && n.tag == o.tag;
            P.eq("prims.cut_keeps_the_rest", flat, 1);
            // The local coordinates move with the corner: x0 went from o.x0 to 20, so d0.x moved by (20 - o.x0) in a
            // quad whose local x runs at one unit per px.
            P.near("prims.cut_local_x", n.d0[0], o.d0[0] + (kClip.x - o.x0), 1e-5);
            P.near("prims.cut_local_y", n.d0[1], o.d0[1] + (kClip.y - o.y0), 1e-5);
            P.eq("prims.cut_far_corner_kept", sameBits(n.e0[0], o.e0[0]) && sameBits(n.e0[1], o.e0[1]), 1);
        }
    }

    // ---- nest ------------------------------------------------------------------------------------------------------
    {
        canvas.begin(frame(1.0f));
        canvas.pushClip({ 10.0f, 10.0f, 100.0f, 50.0f });
        canvas.pushClip({ 50.0f, 0.0f, 100.0f, 30.0f });              // ∩ outer = { 50, 10, 60, 20 }
        canvas.rrect(0.0f, 0.0f, 160.0f, 96.0f, 0.0f, Col{ 1, 1, 1, 255 });
        canvas.popClip();
        canvas.rrect(0.0f, 0.0f, 160.0f, 96.0f, 0.0f, Col{ 2, 2, 2, 255 });   // the outer clip again
        canvas.popClip();
        canvas.rrect(0.0f, 0.0f, 160.0f, 96.0f, 0.0f, Col{ 3, 3, 3, 255 });   // no clip
        const PrimList& l = canvas.end();
        const bool three = l.prims.size() == 3;
        P.eq("nest.count", three, 1);
        if (three)
        {
            const Prim& a = l.prims[0];
            const Prim& b = l.prims[1];
            const Prim& c = l.prims[2];
            P.eq("nest.inner_intersected", a.x0 == 50.0f && a.y0 == 10.0f && a.x1 == 110.0f && a.y1 == 30.0f, 1);
            P.eq("nest.outer_restored", b.x0 == 10.0f && b.y0 == 10.0f && b.x1 == 110.0f && b.y1 == 60.0f, 1);
            P.eq("nest.after_unclipped", c.x0 < 0.0f && c.y0 < 0.0f && c.x1 > 160.0f && c.y1 > 96.0f, 1);
        }
    }

    // ---- depth and scopes ----------------------------------------------------------------------------------------------
    {
        canvas.begin(frame(1.0f));
        for (int i = 0; i < Canvas::kMaxClips + 3; ++i)
            canvas.pushClip({ 0.0f, 0.0f, 40.0f, 40.0f });
        P.eq("depth.counts_ignored", canvas.clipDepth(), Canvas::kMaxClips + 3);
        for (int i = 0; i < 3; ++i)
            canvas.popClip();                                          // the ignored ones
        canvas.rrect(0.0f, 0.0f, 160.0f, 96.0f, 0.0f, Col{ 1, 1, 1, 255 });
        for (int i = 0; i < Canvas::kMaxClips; ++i)
            canvas.popClip();
        canvas.popClip();                                              // unmatched
        P.eq("depth.balanced", canvas.clipDepth(), 0);
        canvas.pushClip({ 0.0f, 0.0f, 10.0f, 10.0f });                 // left open
        funkgui::AxisMap map{};
        canvas.axis(3, &map, nullptr);
        canvas.rrect(0.0f, 0.0f, 160.0f, 96.0f, 0.0f, Col{ 2, 2, 2, 255 });
        const PrimList& l = canvas.end();
        const bool two = l.prims.size() == 2;
        P.eq("depth.prims", two, 1);
        if (two)
        {
            P.eq("depth.deep_clip_applied", l.prims[0].x1 == 40.0f && l.prims[0].y1 == 40.0f, 1);
            P.eq("depth.end_closes", l.prims[1].x1 == 10.0f && l.prims[1].y1 == 10.0f, 1);
        }
        P.eq("depth.end_depth", canvas.clipDepth(), 0);
        P.eq("depth.axes_kept", static_cast<int64_t>(l.axes.size()), 1);

        canvas.begin(frame(1.0f));
        canvas.pushClip({ 0.0f, 0.0f, 10.0f, 10.0f });
        canvas.begin(frame(1.0f));                                     // forgets it
        canvas.rrect(0.0f, 0.0f, 160.0f, 96.0f, 0.0f, Col{ 2, 2, 2, 255 });
        const bool open = canvas.clipDepth() == 0;
        const PrimList& m = canvas.end();
        P.eq("depth.begin_forgets", open && m.prims.size() == 1 && m.prims[0].x1 > 160.0f, 1);

        canvas.begin(frame(1.0f));
        {
            const Canvas::ClipScope scope(canvas, { 5.0f, 5.0f, 20.0f, 20.0f });
            canvas.rrect(0.0f, 0.0f, 160.0f, 96.0f, 0.0f, Col{ 2, 2, 2, 255 });
            P.eq("depth.scope_pushes", canvas.clipDepth(), 1);
        }
        P.eq("depth.scope_pops", canvas.clipDepth(), 0);
        const PrimList& s = canvas.end();
        P.eq("depth.scope_clips", s.prims.size() == 1 && s.prims[0].x0 == 5.0f && s.prims[0].x1 == 25.0f, 1);
    }

    return P.finish();
}

// FUNKGUI_TEST name=fg.canvas.shader timeout=300 gpu=0
//
// fg.canvas.shader (v0.19.0, Canvas::shader; canvas/PrimList.h ShaderDraw): a shader draw is recorded beside the
// primitives at its place in the order, and everything that reads a PrimList keeps that place. Plain C++ with no GPU:
// it runs in every configuration (with and without JUCE, natively and under node). Spec rows only.
//
// - record.*: the position (how many primitives before it), the rect as given, the clip in force (the open clips
//   intersected; none outside a clip; the deepest one that took effect beyond kMaxClips), the tag and live flag, the
//   params padded with 0 and clamped to 16, nothing recorded for an empty, non-finite or wholly clipped rect, the
//   position moved by a popped clip that dropped primitives before it (nested too), and begin() clearing them.
// - dump.*: the exact "shader" line, its place among the "p" lines, a round trip bit for bit, a dump without shader
//   draws holding no shader line, and the parser refusing a wrong `at`, a v1 dump with one and malformed lines.
// - fingerprint.*: shader draws move the geometry hash (rect, clip, id, position), not the params; a live one is left
//   out and counted live; tags and extents count them; a frame without them hashes as it did.
// - steps.*: funkgui::drawSteps: one run without shader draws (none for an empty list), the runs split at each draw,
//   no empty run between two at one place, an `at` past the end drawn last.
// - pixels.* / expand.*: funkgui::shaderPixels (rect ∩ clip in device px, the pixel-centre rule, per-axis scale,
//   clamped) and funkgui::expandShader (the quad's corners, v_uv and v_px exactly).
// - raster.*: SoftRaster's placeholder at dpi 1 and 2, supersample 1 and 2: its border, stripe and fill colours at
//   known pixels, cut by the clip, over a primitive recorded before it and under one recorded after it.

#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/Expand.h>
#include <funkgui/canvas/Fingerprint.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/canvas/SoftRaster.h>
#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Col.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/test/Harness.h>
#include <funkgui/text/FontService.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace T = funkgui::test;
using funkgui::Canvas;
using funkgui::Col;
using funkgui::DrawStep;
using funkgui::FrameInfo;
using funkgui::Image;
using funkgui::PixelRect;
using funkgui::PrimList;
using funkgui::Rect;
using funkgui::ShaderDraw;

namespace
{
    constexpr int kW = 160, kH = 96;
    constexpr Col kClear{ 10, 12, 14, 255 };
    constexpr Col kUnder{ 200, 40, 40, 255 };
    constexpr Col kOver{ 40, 200, 40, 255 };

    static_assert(sizeof(ShaderDraw) == 108, "ShaderDraw has no padding (compared with memcmp below)");

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

    bool sameRect(const Rect& a, const Rect& b) { return std::memcmp(&a, &b, sizeof(Rect)) == 0; }
    bool sameDraw(const ShaderDraw& a, const ShaderDraw& b) { return std::memcmp(&a, &b, sizeof(ShaderDraw)) == 0; }

    std::string dumpOf(const PrimList& l, bool& ok)
    {
        char* buf = nullptr;
        size_t len = 0;
        std::FILE* f = open_memstream(&buf, &len);
        if (f == nullptr)
        {
            ok = false;
            return {};
        }
        ok = l.writeText(f);
        ok = std::fclose(f) == 0 && ok;
        std::string s(buf, len);
        std::free(buf);
        return s;
    }

    const uint8_t* px(const Image& img, int x, int y)
    {
        return &img.rgba[(static_cast<size_t>(y) * static_cast<size_t>(img.w) + static_cast<size_t>(x)) * 4u];
    }

    bool is(const Image& img, int x, int y, Col c)
    {
        if (x < 0 || y < 0 || x >= img.w || y >= img.h)
            return false;
        const uint8_t* p = px(img, x, y);
        return p[0] == c.r && p[1] == c.g && p[2] == c.b && p[3] == 255;
    }

    // The placeholder colour SoftRaster.h's rule gives the logical point (x, y) of a draw over r at dpi.
    Col expected(const Rect& r, float x, float y, float dpi)
    {
        namespace ph = funkgui::shaderPlaceholder;
        const float lx = x - r.x, ly = y - r.y, e = 1.0f / dpi;
        if (lx < e || ly < e || !(lx < r.w - e) || !(ly < r.h - e))
            return ph::border;
        return std::fmod(lx + ly, ph::stripePeriod) < ph::stripeWidth ? ph::stripe : ph::fill;
    }
}

int main(int argc, char** argv)
{
    const funkgui::HeadlessGuiScope gui;                 // JUCE's GUI side, when there is JUCE: the atlas bakes there
    T::Probe P("fg.canvas.shader", "", argc, argv);

    auto& fonts = funkgui::FontService::get();
    const funkgui::FontAtlasSdf& atlas = fonts.atlas();
    if (!P.eq("font.ok", fonts.ok(), 1))
        return P.finish();
    Canvas canvas(atlas);
    const float params3[3] = { 0.5f, 1.0f, -2.0f };
    float params20[20];
    for (int i = 0; i < 20; ++i)
        params20[i] = static_cast<float>(i + 1);

    // ---- record: order, rect, tag, live, params -----------------------------------------------------------------------
    {
        canvas.begin(frame(1.0f));
        canvas.rrect(4.0f, 4.0f, 20.0f, 10.0f, 2.0f, kUnder);
        canvas.setTag(funkgui::tags::hint);
        canvas.shader(7, { 10.0f, 20.0f, 30.0f, 40.0f }, params3, 3);
        canvas.setTag(0);
        canvas.rrect(4.0f, 30.0f, 20.0f, 10.0f, 2.0f, kUnder);
        canvas.setLive(true);
        canvas.shader(8, { 1.0f, 2.0f, 3.0f, 4.0f }, params20, 20);
        canvas.setLive(false);
        canvas.shader(9, { 1.0f, 2.0f, 3.0f, 4.0f }, nullptr, 5);
        canvas.shader(10, { 1.0f, 2.0f, 3.0f, 4.0f }, params3, -1);
        const PrimList& l = canvas.end();
        const bool four = l.shaders.size() == 4 && l.prims.size() == 2;
        P.eq("record.count", four, 1);
        if (four)
        {
            const ShaderDraw& a = l.shaders[0];
            P.eq("record.at_after_first_prim", a.at, 1);
            P.eq("record.at_after_second_prim", l.shaders[1].at, 2);
            P.eq("record.same_place_keeps_order", l.shaders[2].at == 2 && l.shaders[3].at == 2
                                                      && l.shaders[2].id == 9 && l.shaders[3].id == 10, 1);
            P.eq("record.id", a.id, 7);
            P.eq("record.rect_as_given", sameRect(a.rect, { 10.0f, 20.0f, 30.0f, 40.0f }), 1);
            P.eq("record.unclipped", !a.clipped && sameRect(a.clip, Rect{}), 1);
            P.eq("record.tag", a.tag, funkgui::tags::hint);
            P.eq("record.not_live", a.live, 0);
            P.eq("record.live", l.shaders[1].live, 1);
            P.eq("record.untagged", l.shaders[1].tag, 0);
            bool padded = a.params[0] == 0.5f && a.params[1] == 1.0f && a.params[2] == -2.0f;
            for (int i = 3; i < ShaderDraw::kMaxParams; ++i)
                padded = padded && a.params[i] == 0.0f;
            P.eq("record.params_padded", padded, 1);
            bool clamped = true;
            for (int i = 0; i < ShaderDraw::kMaxParams; ++i)
                clamped = clamped && l.shaders[1].params[i] == static_cast<float>(i + 1);
            P.eq("record.params_clamped_to_16", clamped, 1);
            bool none = true;
            for (int i = 0; i < ShaderDraw::kMaxParams; ++i)
                none = none && l.shaders[2].params[i] == 0.0f && l.shaders[3].params[i] == 0.0f;
            P.eq("record.null_or_negative_params_are_0", none, 1);
        }
        canvas.begin(frame(1.0f));
        P.eq("record.begin_clears", canvas.end().shaders.size(), 0);
    }

    // ---- record: what records nothing --------------------------------------------------------------------------------
    {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();
        canvas.begin(frame(1.0f));
        canvas.shader(1, { 10.0f, 10.0f, 0.0f, 10.0f });
        canvas.shader(1, { 10.0f, 10.0f, 10.0f, -1.0f });
        canvas.shader(1, { nan, 10.0f, 10.0f, 10.0f });
        canvas.shader(1, { 10.0f, 10.0f, inf, 10.0f });
        canvas.pushClip({ 0.0f, 0.0f, 10.0f, 10.0f });
        canvas.shader(1, { 20.0f, 20.0f, 5.0f, 5.0f });             // wholly outside the clip
        canvas.shader(1, { 10.0f, 0.0f, 5.0f, 5.0f });              // touching its right edge: nothing inside
        canvas.pushClip({ 50.0f, 50.0f, 10.0f, 10.0f });            // ∩ the outer one: empty
        canvas.shader(1, { 0.0f, 0.0f, 100.0f, 100.0f });
        canvas.popClip();
        canvas.popClip();
        P.eq("record.nothing_for_empty_nonfinite_or_clipped", canvas.end().shaders.size(), 0);
    }

    // ---- record: clips ------------------------------------------------------------------------------------------------
    {
        canvas.begin(frame(1.0f));
        canvas.pushClip({ 10.0f, 10.0f, 100.0f, 50.0f });
        canvas.pushClip({ 50.0f, 0.0f, 100.0f, 30.0f });            // ∩ = { 50, 10, 60, 20 }
        canvas.shader(1, { 40.0f, 5.0f, 30.0f, 30.0f });
        canvas.popClip();
        canvas.shader(2, { 0.0f, 0.0f, 20.0f, 20.0f });
        canvas.popClip();
        canvas.shader(3, { 0.0f, 0.0f, 20.0f, 20.0f });
        for (int i = 0; i < Canvas::kMaxClips + 2; ++i)
            canvas.pushClip({ 0.0f, 0.0f, 40.0f - static_cast<float>(i), 40.0f });
        canvas.shader(4, { 0.0f, 0.0f, 100.0f, 100.0f });           // the deepest clip that took effect
        const PrimList& l = canvas.end();
        const bool four = l.shaders.size() == 4;
        P.eq("record.clip_count", four, 1);
        if (four)
        {
            P.eq("record.clip_intersected", l.shaders[0].clipped
                                                && sameRect(l.shaders[0].clip, { 50.0f, 10.0f, 60.0f, 20.0f }), 1);
            P.eq("record.clip_rect_not_cut", sameRect(l.shaders[0].rect, { 40.0f, 5.0f, 30.0f, 30.0f }), 1);
            P.eq("record.clip_outer_restored", l.shaders[1].clipped
                                                   && sameRect(l.shaders[1].clip, { 10.0f, 10.0f, 100.0f, 50.0f }), 1);
            P.eq("record.clip_none_after_pop", l.shaders[2].clipped, 0);
            const float deepest = 40.0f - static_cast<float>(Canvas::kMaxClips - 1);
            P.eq("record.clip_beyond_max", l.shaders[3].clipped
                                               && sameRect(l.shaders[3].clip, { 0.0f, 0.0f, deepest, 40.0f }), 1);
        }
    }

    // ---- record: a popped clip moves the positions --------------------------------------------------------------------
    {
        canvas.begin(frame(1.0f));
        canvas.rrect(0.0f, 0.0f, 4.0f, 4.0f, 0.0f, kUnder);         // 0, kept (no clip)
        canvas.shader(1, { 0.0f, 0.0f, 10.0f, 10.0f });            // at 1
        canvas.pushClip({ 20.0f, 20.0f, 40.0f, 40.0f });
        canvas.shader(2, { 20.0f, 20.0f, 10.0f, 10.0f });          // at 1: before anything inside the clip
        canvas.rrect(100.0f, 0.0f, 4.0f, 4.0f, 0.0f, kUnder);       // 1, outside: dropped
        canvas.shader(3, { 20.0f, 20.0f, 10.0f, 10.0f });          // at 2 -> 1
        canvas.rrect(30.0f, 30.0f, 4.0f, 4.0f, 0.0f, kUnder);       // 2 -> 1, kept
        canvas.pushClip({ 20.0f, 20.0f, 10.0f, 10.0f });
        canvas.rrect(0.0f, 100.0f, 4.0f, 4.0f, 0.0f, kUnder);       // 3, outside both: dropped
        canvas.shader(4, { 20.0f, 20.0f, 10.0f, 10.0f });          // at 4 -> 3 -> 2
        canvas.popClip();
        canvas.rrect(130.0f, 0.0f, 4.0f, 4.0f, 0.0f, kUnder);       // dropped
        canvas.shader(5, { 20.0f, 20.0f, 10.0f, 10.0f });          // at the end -> 2
        canvas.popClip();
        canvas.rrect(0.0f, 0.0f, 4.0f, 4.0f, 0.0f, kUnder);         // 2
        canvas.shader(6, { 0.0f, 0.0f, 10.0f, 10.0f });            // at 3
        const PrimList& l = canvas.end();
        const bool ok = l.prims.size() == 3 && l.shaders.size() == 6;
        P.eq("record.pop_counts", ok, 1);
        if (ok)
        {
            const uint32_t want[6] = { 1, 1, 1, 2, 2, 3 };
            bool same = true;
            for (int i = 0; i < 6; ++i)
                same = same && l.shaders[static_cast<size_t>(i)].at == want[i]
                       && l.shaders[static_cast<size_t>(i)].id == static_cast<uint32_t>(i + 1);
            P.eq("record.pop_moves_positions", same, 1);
        }
    }

    // ---- dump ---------------------------------------------------------------------------------------------------------
    {
        canvas.begin(frame(2.0f));
        canvas.rrect(4.0f, 4.0f, 20.0f, 10.0f, 2.0f, kUnder);
        canvas.setTag(funkgui::tags::hint);
        canvas.shader(7, { 10.0f, 20.0f, 200.0f, 100.0f }, params3, 3);
        canvas.setTag(0);
        canvas.pushClip({ 0.0f, 0.0f, 120.5f, 64.0f });
        canvas.setLive(true);
        canvas.shader(4294967295u, { 0.25f, 1.0f, 3.0f, 4.0f }, params20, 16);
        canvas.setLive(false);
        canvas.popClip();
        canvas.rrect(4.0f, 30.0f, 20.0f, 10.0f, 2.0f, kOver);
        canvas.shader(1, { 1.0f, 2.0f, 3.0f, 4.0f });
        const PrimList l = canvas.end();                         // a copy
        bool written = false;
        const std::string text = dumpOf(l, written);
        P.eq("dump.written", written, 1);
        const std::string line1 = "shader 7 at 1  rect 10 20 200 100  clip -  params 0.5 1 -2 0 0 0 0 0 0 0 0 0 0 0 0 0"
                                  "  live 0  t HINT\n";
        const std::string line2 = "shader 4294967295 at 1  rect 0.25 1 3 4  clip 0 0 120.5 64  params 1 2 3 4 5 6 7 8 9 "
                                  "10 11 12 13 14 15 16  live 1\n";
        const std::string line3 = "shader 1 at 2  rect 1 2 3 4  clip -  params 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0  live 0\n";
        const size_t at1 = text.find(line1), at2 = text.find(line2), at3 = text.find(line3);
        P.eq("dump.lines_exact", at1 != std::string::npos && at2 != std::string::npos && at3 != std::string::npos, 1);
        // In drawing order: p (prim 0), the two shader lines, p (prim 1), the last shader line.
        const size_t p0 = text.find("\np "), p1 = text.find("\np ", p0 + 1);
        P.eq("dump.drawing_order", p0 != std::string::npos && p1 != std::string::npos && p0 < at1 && at1 < at2
                                       && at2 < p1 && p1 < at3 && text.find("\np ", p1 + 1) == std::string::npos, 1);

        PrimList back;
        const bool parsed = PrimList::parseText(text, back);
        P.eq("dump.parses", parsed, 1);
        bool same = parsed && back.shaders.size() == l.shaders.size() && back.prims.size() == l.prims.size();
        for (size_t i = 0; same && i < l.shaders.size(); ++i)
            same = sameDraw(back.shaders[i], l.shaders[i]);
        for (size_t i = 0; same && i < l.prims.size(); ++i)
            same = std::memcmp(&back.prims[i], &l.prims[i], sizeof(funkgui::Prim)) == 0;
        P.eq("dump.round_trip_bit_for_bit", same, 1);

        canvas.begin(frame(2.0f));
        canvas.rrect(4.0f, 4.0f, 20.0f, 10.0f, 2.0f, kUnder);
        bool plainWritten = false;
        const std::string plain = dumpOf(canvas.end(), plainWritten);
        P.eq("dump.no_shader_no_line", plainWritten && plain.find("shader") == std::string::npos, 1);

        const auto refuses = [&text](std::string_view from, std::string_view to)
        {
            std::string t = text;
            const size_t at = t.find(from);
            if (at == std::string::npos)
                return false;
            t.replace(at, from.size(), to);
            PrimList x;
            return !PrimList::parseText(t, x) && x.shaders.empty() && x.prims.empty();
        };
        P.eq("dump.refuses_wrong_at", refuses("shader 1 at 2", "shader 1 at 1"), 1);
        P.eq("dump.refuses_15_params", refuses(" 15 16  live 1", " 15  live 1"), 1);
        P.eq("dump.refuses_bad_live", refuses("live 0\n", "live 2\n"), 1);
        P.eq("dump.refuses_bad_clip", refuses("clip -  params 0 0", "clip 1 2  params 0 0"), 1);
        P.eq("dump.refuses_trailing", refuses("t HINT\n", "t HINT x\n"), 1);
        PrimList v1;
        const std::string v1Text = "clear 0a0c0e\nview 160 96 dpi 1\n";
        P.eq("dump.v1_reads", PrimList::parseText(v1Text, v1), 1);
        const std::string atZero = "shader 1 at 0  rect 1 2 3 4  clip -  params 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0  live 0\n";
        P.eq("dump.v2_alone_reads", PrimList::parseText("funkgui-dump 2\n" + v1Text + atZero, v1)
                                        && v1.shaders.size() == 1 && v1.shaders[0].at == 0, 1);
        P.eq("dump.refuses_in_v1", PrimList::parseText(v1Text + atZero, v1), 0);
    }

    // ---- fingerprint --------------------------------------------------------------------------------------------------
    {
        const auto record = [&canvas](bool withShader, Rect r, float param, bool live, bool clipped, uint32_t id)
        {
            canvas.begin(frame(1.0f));
            canvas.rrect(4.0f, 4.0f, 20.0f, 10.0f, 2.0f, kUnder);
            if (withShader)
            {
                if (clipped)
                    canvas.pushClip({ 0.0f, 0.0f, 100.0f, 50.0f });
                canvas.setTag(funkgui::tags::hint);
                canvas.setLive(live);
                canvas.shader(id, r, &param, 1);
                canvas.setLive(false);
                canvas.setTag(0);
                if (clipped)
                    canvas.popClip();
            }
            return funkgui::fingerprint(canvas.end());
        };
        const Rect r{ 10.0f, 20.0f, 150.0f, 100.0f };
        const funkgui::Fingerprint none = record(false, r, 0.0f, false, false, 1);
        const funkgui::Fingerprint with = record(true, r, 0.0f, false, false, 1);
        P.eq("fingerprint.none_counts_0", none.shaders, 0);
        P.eq("fingerprint.counted", with.shaders, 1);
        P.eq("fingerprint.statics_are_prims", with.statics == none.statics && with.rrects == none.rrects, 1);
        P.eq("fingerprint.moves_geometry", with.geometry != none.geometry, 1);
        P.eq("fingerprint.text_untouched", with.text == none.text, 1);
        P.eq("fingerprint.params_not_hashed", record(true, r, 0.75f, false, false, 1).geometry == with.geometry, 1);
        P.eq("fingerprint.rect_hashed",
             record(true, { 10.0f, 20.0f, 151.0f, 100.0f }, 0.0f, false, false, 1).geometry != with.geometry, 1);
        P.eq("fingerprint.id_hashed", record(true, r, 0.0f, false, false, 2).geometry != with.geometry, 1);
        P.eq("fingerprint.clip_hashed", record(true, r, 0.0f, false, true, 1).geometry != with.geometry, 1);
        const funkgui::Fingerprint live = record(true, r, 0.0f, true, false, 1);
        P.eq("fingerprint.live_left_out", live.geometry == none.geometry && live.shaders == 0 && live.live == 1, 1);
        funkgui::FingerprintOptions all;
        all.excludeLive = false;
        canvas.begin(frame(1.0f));
        canvas.setLive(true);
        canvas.shader(1, r);
        canvas.setLive(false);
        const funkgui::Fingerprint kept = funkgui::fingerprint(canvas.end(), all);
        P.eq("fingerprint.live_kept_when_asked", kept.shaders == 1 && kept.live == 1, 1);
        P.eq("fingerprint.extents", with.maxX == 160.0f && with.maxY == 120.0f, 1);
        bool tagged = false;
        for (const auto& [tag, count] : with.tagCounts)
            tagged = tagged || (tag == funkgui::tags::hint && count == 1);
        P.eq("fingerprint.tag_counted", tagged, 1);
        P.eq("fingerprint.repeatable", record(true, r, 0.0f, false, false, 1).geometry == with.geometry, 1);
    }

    // ---- steps --------------------------------------------------------------------------------------------------------
    {
        std::vector<DrawStep> steps;
        const auto stepsOf = [&steps](const PrimList& l) -> const std::vector<DrawStep>&
        {
            funkgui::drawSteps(l, steps);
            return steps;
        };
        const auto run = [](const DrawStep& s, uint32_t first, uint32_t count)
        { return s.kind == DrawStep::Kind::prims && s.first == first && s.count == count; };
        const auto shade = [](const DrawStep& s, uint32_t index)
        { return s.kind == DrawStep::Kind::shader && s.first == index && s.count == 1; };

        PrimList l;
        P.eq("steps.empty_list", stepsOf(l).size(), 0);
        l.prims.resize(5);
        P.eq("steps.no_shaders_one_run", stepsOf(l).size() == 1 && run(steps[0], 0, 5), 1);
        ShaderDraw s;
        s.at = 0;
        l.shaders.push_back(s);
        s.at = 2;
        l.shaders.push_back(s);
        l.shaders.push_back(s);
        s.at = 5;
        l.shaders.push_back(s);
        const auto& st = stepsOf(l);
        P.eq("steps.split", st.size() == 6 && shade(st[0], 0) && run(st[1], 0, 2) && shade(st[2], 1)
                                && shade(st[3], 2) && run(st[4], 2, 3) && shade(st[5], 3), 1);
        l.shaders.back().at = 99;
        P.eq("steps.past_the_end_drawn_last", stepsOf(l).size() == 6 && shade(steps[5], 3), 1);
        l.prims.clear();
        l.shaders.resize(1);
        P.eq("steps.shader_alone", stepsOf(l).size() == 1 && shade(steps[0], 0), 1);
    }

    // ---- pixels and expand ---------------------------------------------------------------------------------------------
    {
        ShaderDraw s;
        s.rect = { 10.0f, 20.0f, 30.0f, 40.0f };
        const auto pxIs = [](PixelRect p, int x, int y, int w, int h)
        { return p.x == x && p.y == y && p.w == w && p.h == h; };
        P.eq("pixels.dpi1", pxIs(funkgui::shaderPixels(s, frame(1.0f), kW, kH), 10, 20, 30, 40), 1);
        P.eq("pixels.dpi2", pxIs(funkgui::shaderPixels(s, frame(2.0f), 2 * kW, 2 * kH), 20, 40, 60, 80), 1);
        // Edges through device pixel centres (20.5, 41.5, 80.5, 121.5): the pixel whose centre is on a left or top edge
        // is in, the one on a right or bottom edge out (the half-open rule).
        s.rect = { 10.25f, 20.75f, 30.0f, 40.0f };
        P.eq("pixels.centre_rule", pxIs(funkgui::shaderPixels(s, frame(2.0f), 2 * kW, 2 * kH), 20, 41, 60, 80), 1);
        s.rect = { 10.0f, 20.0f, 30.0f, 40.0f };
        P.eq("pixels.per_axis_scale", pxIs(funkgui::shaderPixels(s, frame(1.0f), 2 * kW, kH), 20, 20, 60, 40), 1);
        s.clipped = true;
        s.clip = { 0.0f, 30.0f, 25.0f, 100.0f };
        P.eq("pixels.clipped", pxIs(funkgui::shaderPixels(s, frame(1.0f), kW, kH), 10, 30, 15, 30), 1);
        s.clipped = false;
        s.rect = { -10.0f, 80.0f, 200.0f, 40.0f };
        P.eq("pixels.clamped", pxIs(funkgui::shaderPixels(s, frame(1.0f), kW, kH), 0, 80, kW, 16), 1);
        s.rect = { 170.0f, 0.0f, 10.0f, 10.0f };
        const PixelRect off = funkgui::shaderPixels(s, frame(1.0f), kW, kH);
        P.eq("pixels.off_frame_empty", off.w == 0 || off.h == 0, 1);
        FrameInfo sizeless = frame(1.0f);
        sizeless.logicalW = 0;
        const PixelRect nothing = funkgui::shaderPixels(s, sizeless, kW, kH);
        P.eq("pixels.no_frame_empty", nothing.w == 0 && nothing.h == 0, 1);

        s.rect = { 10.0f, 20.0f, 30.0f, 40.0f };
        funkgui::Vtx v[6];
        funkgui::expandShader(s, v);
        const auto corner = [](const funkgui::Vtx& x, float px, float py, float u, float w, float pu, float pv)
        {
            return x.x == px && x.y == py && x.d0[0] == u && x.d0[1] == w && x.d0[2] == pu && x.d0[3] == pv
                   && x.c0 == 0 && x.c1 == 0 && x.d1[0] == 0.0f && x.d2[2] == 0.0f;
        };
        P.eq("expand.corners", corner(v[0], 10.0f, 20.0f, 0.0f, 0.0f, 0.0f, 0.0f)
                                   && corner(v[1], 40.0f, 20.0f, 1.0f, 0.0f, 30.0f, 0.0f)
                                   && corner(v[2], 40.0f, 60.0f, 1.0f, 1.0f, 30.0f, 40.0f)
                                   && corner(v[5], 10.0f, 60.0f, 0.0f, 1.0f, 0.0f, 40.0f)
                                   && std::memcmp(&v[3], &v[0], sizeof v[0]) == 0
                                   && std::memcmp(&v[4], &v[2], sizeof v[2]) == 0, 1);
        PrimList l;
        l.prims.resize(2);
        l.shaders.push_back(s);
        std::vector<funkgui::Vtx> out;
        funkgui::expand(l, out);
        P.eq("expand.prims_only", out.size(), 12);
    }

    // ---- raster: the placeholder, in order and clipped ------------------------------------------------------------------
    const Rect kRect{ 10.0f, 20.0f, 80.0f, 50.0f };
    const Rect kClip{ 20.0f, 10.0f, 100.0f, 60.0f };               // rect ∩ clip = x 20..90, y 20..70
    for (const float dpi : { 1.0f, 2.0f })
        for (const int ss : { 1, 2 })
        {
            const std::string key = "raster.dpi" + std::to_string(static_cast<int>(dpi)) + ".ss" + std::to_string(ss);
            canvas.begin(frame(dpi));
            canvas.rrect(-4.0f, -4.0f, kW + 8.0f, kH + 8.0f, 0.0f, kUnder);   // under everything
            canvas.pushClip(kClip);
            canvas.shader(1, kRect, params3, 3);
            canvas.popClip();
            canvas.rrect(60.0f, 40.0f, 40.0f, 20.0f, 0.0f, kOver);        // over part of it
            const Image img = funkgui::rasterise(canvas.end(), atlas, ss);
            P.eq(key + ".size", img.w == static_cast<int>(kW * dpi) && img.h == static_cast<int>(kH * dpi), 1);
            if (img.w != static_cast<int>(kW * dpi) || img.h != static_cast<int>(kH * dpi))
                continue;
            // A logical point's device pixel. Whole and half logical px: every sample of these pixels agrees.
            const auto at = [&img, dpi](float x, float y, Col c)
            { return is(img, static_cast<int>(x * dpi), static_cast<int>(y * dpi), c); };
            P.eq(key + ".border_top", at(50.0f, 20.0f, funkgui::shaderPlaceholder::border), 1);
            P.eq(key + ".border_bottom", at(50.0f, 70.0f - 1.0f / dpi, funkgui::shaderPlaceholder::border), 1);
            P.eq(key + ".border_right", at(90.0f - 1.0f / dpi, 30.0f, funkgui::shaderPlaceholder::border), 1);
            P.eq(key + ".fill", at(30.0f, 30.0f, expected(kRect, 30.25f, 30.25f, dpi))
                                    && expected(kRect, 30.25f, 30.25f, dpi).r == funkgui::shaderPlaceholder::fill.r, 1);
            P.eq(key + ".stripe", at(25.0f, 30.0f, funkgui::shaderPlaceholder::stripe), 1);
            P.eq(key + ".below_border_not_border", at(50.0f, 21.0f, expected(kRect, 50.25f, 21.25f, dpi)), 1);
            P.eq(key + ".clip_cuts_left", at(15.0f, 30.0f, kUnder), 1);     // in the rect, left of the clip
            P.eq(key + ".outside_rect", at(92.0f, 30.0f, kUnder), 1);       // in the clip, right of the rect
            P.eq(key + ".over_it", at(70.0f, 50.0f, kOver), 1);            // the primitive recorded after it
            P.eq(key + ".over_beside", at(95.0f, 50.0f, kOver), 1);
            // Every pixel of rect ∩ clip outside the later primitive is a placeholder colour.
            int wrong = 0;
            for (int y = static_cast<int>(20 * dpi); y < static_cast<int>(70 * dpi); ++y)
                for (int x = static_cast<int>(20 * dpi); x < static_cast<int>(90 * dpi); ++x)
                {
                    const float lx = (static_cast<float>(x) + 0.5f) / dpi, ly = (static_cast<float>(y) + 0.5f) / dpi;
                    if (lx >= 58.0f && lx < 102.0f && ly >= 38.0f && ly < 62.0f)
                        continue;                                           // the later rrect and its apron
                    const uint8_t* p = px(img, x, y);
                    const Col e = expected(kRect, lx, ly, dpi);
                    // Pixels across a stripe edge average two colours at ss 2: within the two colours' range.
                    const auto inRange = [](int v, int a, int b) { return v >= std::min(a, b) && v <= std::max(a, b); };
                    namespace ph = funkgui::shaderPlaceholder;
                    const bool ok = ss == 1 ? (p[0] == e.r && p[1] == e.g && p[2] == e.b)
                                            : (inRange(p[0], ph::fill.r, ph::border.r) && p[3] == 255);
                    wrong += ok ? 0 : 1;
                }
            P.eq(key + ".placeholder_everywhere", wrong, 0);
        }

    return P.finish();
}

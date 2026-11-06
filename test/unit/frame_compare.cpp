// FUNKGUI_TEST name=fg.canvas.compare timeout=120 gpu=0
//
// fg.canvas.compare (0.25.0; canvas/FrameCompare.h): sameDrawing, the safety net EditorHost uses to leave the last
// picture up when a frame records what the last one drew. Plain C++ with no GPU: it runs in every configuration (with
// and without JUCE, natively and under node). Spec rows only.
//   same.*      equal: two empty frames; the same calls recorded twice (primitives, text, a clip, shader and image
//               draws); frames that differ only where no sink looks (frame, dt, fps, clock, rate, theme index,
//               overflows, axes, the missing-glyph and missing-image counts, a shader or image draw's tag and live
//               flag); seconds without a shader draw; a NaN param against itself; the comparison both ways round
//   differs.*   not equal: each compared FrameInfo field (logical size, dpi, clear, text gamma), seconds when either
//               frame has a shader draw, a primitive moved, recoloured, re-tagged, added or removed, -0 against 0, a
//               shader draw's id, position, rect, clip and a param, an image draw's id, position, shader position,
//               dst, uv, radius, opacity and clip, and a draw added

#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/FrameCompare.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Col.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/test/Harness.h>
#include <funkgui/text/FontService.h>
#include <funkgui/text/TextStyle.h>

#include <cstdint>
#include <functional>
#include <limits>
#include <string>

namespace T = funkgui::test;
using funkgui::Canvas;
using funkgui::Col;
using funkgui::FrameInfo;
using funkgui::ImageDraw;
using funkgui::PrimList;
using funkgui::Rect;
using funkgui::ShaderDraw;

namespace
{
    constexpr Col kInk{ 220, 222, 226, 255 };
    constexpr Col kFill{ 40, 44, 52, 255 };

    FrameInfo frame()
    {
        FrameInfo f;
        f.logicalW = 240;
        f.logicalH = 160;
        f.dpi = 2.0f;
        f.clear = Col{ 10, 12, 14, 255 };
        f.textGamma = 0.714285731f;
        f.seconds = 1.25f;
        f.frame = 75;
        f.dt = 1.0f / 60.0f;
        f.fps = 59.5f;
        return f;
    }

    // One frame of every kind of call, as a Panel would record it.
    PrimList record(Canvas& c, const FrameInfo& info, bool withShader, const char* word = "SAME")
    {
        c.begin(info);
        c.rrect(8.0f, 8.0f, 224.0f, 144.0f, 6.0f, kFill, 1.0f, kInk);
        c.text(word, 20.0f, 30.0f, funkgui::TextStyle{}, kInk);
        c.segment(20.0f, 60.0f, 200.0f, 60.0f, 1.0f, kInk);
        {
            const Canvas::ClipScope clip(c, Rect{ 16.0f, 70.0f, 120.0f, 40.0f });
            c.rrect(10.0f, 64.0f, 200.0f, 30.0f, 4.0f, kInk);
        }
        if (withShader)
        {
            const float params[3] = { 0.25f, 0.5f, 1.0f };
            c.shader(7, Rect{ 140.0f, 80.0f, 80.0f, 60.0f }, params, 3);
        }
        PrimList l = c.end();                        // a copy: the next begin() clears the canvas's own
        ImageDraw img;                               // by hand: Canvas::image needs a registered picture
        img.id = 3;
        img.at = static_cast<uint32_t>(l.prims.size());
        img.shaderAt = static_cast<uint32_t>(l.shaders.size());
        img.dst = Rect{ 20.0f, 100.0f, 48.0f, 32.0f };
        img.radius = 4.0f;
        l.images.push_back(img);
        return l;
    }

    // Whether sameDrawing says `want` both ways round.
    bool both(const PrimList& a, const PrimList& b, bool want)
    {
        return funkgui::sameDrawing(a, b) == want && funkgui::sameDrawing(b, a) == want;
    }
}

int main(int argc, char** argv)
{
    const funkgui::HeadlessGuiScope gui;                 // JUCE's GUI side, when there is JUCE: the atlas bakes there
    T::Probe P("fg.canvas.compare", "", argc, argv);
    auto& fonts = funkgui::FontService::get();
    const funkgui::FontAtlasSdf& atlas = fonts.atlas();      // baked here, at the first call
    if (!P.eq("font.ok", fonts.ok(), 1))
        return P.finish();
    Canvas canvas(atlas);

    const PrimList base = record(canvas, frame(), true);
    P.eq("base.has_everything", base.prims.size() >= 4 && base.shaders.size() == 1 && base.images.size() == 1, 1);

    // ---- same ----------------------------------------------------------------------------------------------------
    P.eq("same.empty", both(PrimList{}, PrimList{}, true), 1);
    P.eq("same.recorded_twice", both(base, record(canvas, frame(), true), true), 1);
    {
        PrimList l = base;
        l.info.frame = 900;
        l.info.dt = 0.1f;
        l.info.fps = 12.0f;
        l.info.fixedClock = true;
        l.info.displayLinked = true;
        l.info.fullRate = true;
        l.info.theme = 1;
        l.info.overflows = 3;
        l.axes.push_back({});
        l.missingGlyphs = 2;
        l.missingFirst[0] = 0x1F600;
        l.missingImages = 1;
        l.shaders[0].tag = funkgui::tags::hint;
        l.shaders[0].live = true;
        l.images[0].tag = funkgui::tags::hint;
        l.images[0].live = true;
        P.eq("same.where_no_sink_looks", both(base, l, true), 1);
    }
    {
        FrameInfo later = frame();
        later.seconds = 9.5f;
        P.eq("same.seconds_without_shader",
             both(record(canvas, frame(), false), record(canvas, later, false), true), 1);
    }
    {
        PrimList a = base, b = base;
        a.shaders[0].params[5] = std::numeric_limits<float>::quiet_NaN();
        b.shaders[0].params[5] = a.shaders[0].params[5];
        P.eq("same.nan_equals_itself", both(a, b, true), 1);
    }

    // ---- differs: FrameInfo -----------------------------------------------------------------------------------------
    const auto infoRow = [&](const std::string& key, const std::function<void(FrameInfo&)>& change)
    {
        PrimList l = base;
        change(l.info);
        P.eq("differs." + key, both(base, l, false), 1);
    };
    infoRow("logical_width", [](FrameInfo& f) { f.logicalW += 1; });
    infoRow("logical_height", [](FrameInfo& f) { f.logicalH += 1; });
    infoRow("dpi", [](FrameInfo& f) { f.dpi = 1.0f; });
    infoRow("clear", [](FrameInfo& f) { f.clear.g = 13; });
    infoRow("clear_alpha", [](FrameInfo& f) { f.clear.a = 254; });
    infoRow("text_gamma", [](FrameInfo& f) { f.textGamma = 1.0f; });
    infoRow("seconds_with_shader", [](FrameInfo& f) { f.seconds += 1.0f / 60.0f; });
    {
        PrimList withShader = record(canvas, frame(), false);
        PrimList l = withShader;
        l.info.seconds += 1.0f;
        l.shaders.push_back(base.shaders[0]);
        withShader.shaders.push_back(base.shaders[0]);
        P.eq("differs.seconds_either_shader", both(withShader, l, false), 1);
    }

    // ---- differs: primitives ----------------------------------------------------------------------------------------
    const auto primRow = [&](const std::string& key, const std::function<void(PrimList&)>& change)
    {
        PrimList l = base;
        change(l);
        P.eq("differs." + key, both(base, l, false), 1);
    };
    primRow("prim_moved", [](PrimList& l) { l.prims[0].x0 += 0.5f; });
    primRow("prim_recoloured", [](PrimList& l) { l.prims[1].c0 ^= 0x01u; });
    primRow("prim_retagged", [](PrimList& l) { l.prims[2].tag = funkgui::tags::hint; });
    primRow("prim_added", [](PrimList& l) { l.prims.push_back(l.prims.back()); });
    primRow("prim_removed", [](PrimList& l) { l.prims.pop_back(); });
    {
        PrimList a, b;
        a.prims.push_back(base.prims[0]);
        b.prims.push_back(base.prims[0]);
        a.prims[0].x0 = 0.0f;
        b.prims[0].x0 = -0.0f;
        P.eq("differs.negative_zero", both(a, b, false), 1);
    }
    {
        const PrimList other = record(canvas, frame(), true, "SAMF");
        P.eq("differs.other_text", other.prims.size() == base.prims.size() && both(base, other, false), 1);
    }

    // ---- differs: shader and image draws ----------------------------------------------------------------------------
    primRow("shader_id", [](PrimList& l) { l.shaders[0].id = 8; });
    primRow("shader_at", [](PrimList& l) { l.shaders[0].at -= 1; });
    primRow("shader_rect", [](PrimList& l) { l.shaders[0].rect.w += 1.0f; });
    primRow("shader_clip", [](PrimList& l) { l.shaders[0].clip.x = 1.0f; });
    primRow("shader_clipped", [](PrimList& l) { l.shaders[0].clipped = !l.shaders[0].clipped; });
    primRow("shader_param", [](PrimList& l) { l.shaders[0].params[15] = 1.0f; });
    primRow("shader_added", [](PrimList& l) { l.shaders.push_back(l.shaders[0]); });
    primRow("image_id", [](PrimList& l) { l.images[0].id = 4; });
    primRow("image_at", [](PrimList& l) { l.images[0].at -= 1; });
    primRow("image_shader_at", [](PrimList& l) { l.images[0].shaderAt = 0; });
    primRow("image_dst", [](PrimList& l) { l.images[0].dst.y += 1.0f; });
    primRow("image_uv", [](PrimList& l) { l.images[0].uv.h = 0.5f; });
    primRow("image_radius", [](PrimList& l) { l.images[0].radius = 0.0f; });
    primRow("image_opacity", [](PrimList& l) { l.images[0].opacity = 0.5f; });
    primRow("image_clip", [](PrimList& l) { l.images[0].clip.w = 10.0f; });
    primRow("image_clipped", [](PrimList& l) { l.images[0].clipped = true; });
    primRow("image_removed", [](PrimList& l) { l.images.clear(); });
    return P.finish();
}

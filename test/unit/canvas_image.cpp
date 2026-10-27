// FUNKGUI_TEST name=fg.canvas.image timeout=300 gpu=0
//
// fg.canvas.image (v0.20.0, Canvas::image; canvas/PrimList.h ImageDraw): an image draw is recorded beside the
// primitives and the shader draws at its place in the order, everything that reads a PrimList keeps that place, and
// SoftRaster draws the picture by the stated rule. Plain C++ with no GPU: every configuration (with and without
// JUCE, natively and under node). Spec rows only.
//
// - record.*: the positions (prims and shader draws before it), the fields as given, radius, opacity and uv sanitised,
//   nothing recorded for an empty or non-finite dst or an empty uv, a missing id counted (inside a clip or not) and
//   recording nothing, the clip in force (intersected; the dst not cut), a popped clip moving the position, begin()
//   clearing all of it.
// - dump.*: the exact "image" and "images missing" lines, their place among the "p" and "shader" lines, a round trip
//   bit for bit, a dump without them unchanged, the parser refusing a wrong position and what Canvas never records.
// - fingerprint.*: image draws move the geometry hash (dst, uv, radius, id, clip), not the opacity; live ones left out;
//   counted, tags and extents; a frame without them hashes as it did.
// - steps.*: drawSteps with image draws: split runs, recording order with shader draws at one place, images alone.
// - expand.* / pixels.* / level.* / sampling.*: expandImage's corners and varyings, imagePixels (the apron, the clip,
//   the centre rule), imageLevel (each power of two, the larger axis, uv, dpi, the last level, a 1 x 1 picture) and
//   imageSampling (the half-texel bounds, a uv narrower than a texel).
// - raster.*: SoftRaster at known pixels (dpi 1, one sample): a 2 x 2 checker scaled up (pure quadrants, the bilinear
//   midpoints), a rounded corner (transparent outside, partial on the arc, the rrect's edge exactly), opacity, a
//   half-transparent picture, a uv crop with no bleed, a clip's hard edges, the order with primitives, a missing or
//   removed picture drawing nothing, a 64 x 64 one-texel checker shown at 8 x 8 sampling its grey level 3; then at
//   dpi 1 and 2 with 1 and 2 samples, every pixel of a fractional, rounded, translucent draw against a reference
//   written from the rule in double (within 1 / 255).
// - host.*: HeadlessHost::draw()'s statistics: images drawn (images.size()) and missing ids (missingImages).
// The known-pixels frame is written to canvas-image.png in the test's sandbox where there is a PNG encoder (JUCE).

#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/Expand.h>
#include <funkgui/canvas/Fingerprint.h>
#include <funkgui/canvas/Images.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/canvas/SoftRaster.h>
#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Col.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/core/Theme.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/panel/Panel.h>
#include <funkgui/test/Harness.h>
#include <funkgui/text/FontService.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace T = funkgui::test;
namespace images = funkgui::images;
using funkgui::Canvas;
using funkgui::Col;
using funkgui::DrawStep;
using funkgui::FrameInfo;
using funkgui::Image;
using funkgui::ImageDraw;
using funkgui::PixelRect;
using funkgui::PrimList;
using funkgui::Rect;
using funkgui::ShaderDraw;

namespace
{
    constexpr int kW = 240, kH = 160;
    constexpr Col kClear{ 10, 12, 14, 255 };
    constexpr Col kUnder{ 40, 60, 80, 255 };
    constexpr Col kOver{ 40, 200, 40, 255 };

    // The pictures: a 2 x 2 checker, a row of four colours, a white texel, a half-transparent white texel, a 64 x 64
    // checker of single texels.
    constexpr uint32_t kChecker = 1, kRow = 2, kWhite = 3, kHalfWhite = 4, kFine = 5, kMissing = 99;
    constexpr Col kRed{ 255, 0, 0, 255 }, kGreen{ 0, 255, 0, 255 }, kBlue{ 0, 0, 255, 255 };
    constexpr Col kYellow{ 255, 255, 0, 255 };

    static_assert(sizeof(ImageDraw) == 72, "ImageDraw has no padding (compared with memcmp below)");

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

    void put(std::vector<uint8_t>& v, int i, Col c)
    {
        v[static_cast<size_t>(i) * 4u] = c.r;
        v[static_cast<size_t>(i) * 4u + 1u] = c.g;
        v[static_cast<size_t>(i) * 4u + 2u] = c.b;
        v[static_cast<size_t>(i) * 4u + 3u] = c.a;
    }

    void registerPictures()
    {
        std::vector<uint8_t> v(16);
        put(v, 0, kRed);
        put(v, 1, kGreen);
        put(v, 2, kBlue);
        put(v, 3, Col{ 255, 255, 255, 255 });
        images::set(kChecker, 2, 2, v.data());
        put(v, 0, kRed);
        put(v, 1, kGreen);
        put(v, 2, kBlue);
        put(v, 3, kYellow);
        images::set(kRow, 4, 1, v.data());
        const uint8_t white[4] = { 255, 255, 255, 255 }, half[4] = { 255, 255, 255, 128 };
        images::set(kWhite, 1, 1, white);
        images::set(kHalfWhite, 1, 1, half);
        std::vector<uint8_t> fine(64u * 64u * 4u);
        for (int i = 0; i < 64 * 64; ++i)
        {
            const uint8_t g = ((i % 64) + (i / 64)) % 2 == 0 ? 0 : 255;
            put(fine, i, Col{ g, g, g, 255 });
        }
        images::set(kFine, 64, 64, fine.data());
        images::remove(kMissing);
    }

    bool sameRect(const Rect& a, const Rect& b) { return std::memcmp(&a, &b, sizeof(Rect)) == 0; }
    bool sameDraw(const ImageDraw& a, const ImageDraw& b) { return std::memcmp(&a, &b, sizeof(ImageDraw)) == 0; }

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

    // The pixel within tol of (r, g, b) (0..255), opaque.
    bool near(const Image& img, int x, int y, double r, double g, double b, double tol = 1.0)
    {
        if (x < 0 || y < 0 || x >= img.w || y >= img.h)
            return false;
        const uint8_t* p = px(img, x, y);
        const bool ok = std::abs(p[0] - r) <= tol && std::abs(p[1] - g) <= tol && std::abs(p[2] - b) <= tol
                        && p[3] == 255;
        if (!ok)
            std::printf("INFO     pixel (%d, %d) = %u %u %u %u, want %.2f %.2f %.2f\n", x, y, p[0], p[1], p[2], p[3], r,
                        g, b);
        return ok;
    }
    bool is(const Image& img, int x, int y, Col c) { return near(img, x, y, c.r, c.g, c.b, 0.0); }

    // a over b at alpha t, per channel, 0..255.
    double over(double a, double b, double t) { return a * t + b * (1.0 - t); }

    double smoothstepD(double e0, double e1, double x)
    {
        const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
        return t * t * (3.0 - 2.0 * t);
    }

    // The rule of canvas/PrimList.h ImageDraw, in double, independently of SoftRaster's code: the colour of an opaque
    // `under` after the draw m of picture p, at the sample (sx, sy) in logical px, at dpi. Returns false where the
    // draw covers nothing (outside its quad), with out = under.
    void referenceSample(const images::Picture& p, const ImageDraw& m, Col under, double sx, double sy, double dpi,
                         double out[3])
    {
        out[0] = under.r;
        out[1] = under.g;
        out[2] = under.b;
        const double e = funkgui::kImageApron;
        if (sx < m.dst.x - e || !(sx < m.dst.x + m.dst.w + e) || sy < m.dst.y - e || !(sy < m.dst.y + m.dst.h + e))
            return;
        const double lx = sx - m.dst.x, ly = sy - m.dst.y;
        const double bx = m.dst.w * 0.5, by = m.dst.h * 0.5;
        const double r = std::min<double>(m.radius, std::min(bx, by));
        const double qx = std::abs(lx - bx) - bx + r, qy = std::abs(ly - by) - by + r;
        const double d = std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) + std::min(std::max(qx, qy), 0.0) - r;
        const double aa = 1.0 / dpi;
        const double cover = (1.0 - smoothstepD(-aa, aa, d)) * m.opacity;
        // The level and its texels, sampled at the clamped point.
        const int level = funkgui::imageLevel(m, static_cast<float>(dpi), p.w, p.h);
        const images::Level& lv = p.levels[static_cast<size_t>(level)];
        const auto bound = [](double start, double size, int texels, double v)
        {
            const double half = 0.5 / texels;
            double lo = start + half, hi = start + size - half;
            if (hi < lo)
                lo = hi = start + size * 0.5;
            return std::clamp(v, lo, hi);
        };
        const double su = bound(m.uv.x, m.uv.w, lv.w, m.uv.x + lx / m.dst.w * m.uv.w);
        const double sv = bound(m.uv.y, m.uv.h, lv.h, m.uv.y + ly / m.dst.h * m.uv.h);
        const double fx = std::clamp(su * lv.w - 0.5, 0.0, lv.w - 1.0);
        const double fy = std::clamp(sv * lv.h - 0.5, 0.0, lv.h - 1.0);
        const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
        const int x1 = std::min(x0 + 1, lv.w - 1), y1 = std::min(y0 + 1, lv.h - 1);
        const double tx = fx - x0, ty = fy - y0;
        double t[4];
        for (int c = 0; c < 4; ++c)
        {
            const auto at = [&](int x, int y)
            { return p.level(level)[(static_cast<size_t>(y) * static_cast<size_t>(lv.w) + static_cast<size_t>(x)) * 4u
                                    + static_cast<size_t>(c)] / 255.0; };
            const double top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * tx;
            const double bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * tx;
            t[c] = top + (bottom - top) * ty;
        }
        const double a = std::clamp(t[3] * cover, 0.0, 1.0);
        for (int c = 0; c < 3; ++c)
            out[c] = over(std::min(t[c] / std::max(t[3], 1e-5), 1.0) * 255.0, out[c], a);
    }

    // The largest difference between the image's pixels over the draw's quad and the reference, ss x ss samples per
    // pixel, everything else under `under`.
    double referenceDiff(const Image& img, const images::Picture& p, const ImageDraw& m, Col under, float dpi, int ss)
    {
        double worst = 0.0;
        const int x0 = static_cast<int>(std::floor((m.dst.x - 2.0f) * dpi));
        const int y0 = static_cast<int>(std::floor((m.dst.y - 2.0f) * dpi));
        const int x1 = static_cast<int>(std::ceil((m.dst.x + m.dst.w + 2.0f) * dpi));
        const int y1 = static_cast<int>(std::ceil((m.dst.y + m.dst.h + 2.0f) * dpi));
        for (int y = std::max(0, y0); y < std::min(img.h, y1); ++y)
            for (int x = std::max(0, x0); x < std::min(img.w, x1); ++x)
            {
                double acc[3] = { 0.0, 0.0, 0.0 };
                for (int j = 0; j < ss; ++j)
                    for (int i = 0; i < ss; ++i)
                    {
                        const double sx = (x * ss + i + 0.5) / (static_cast<double>(dpi) * ss);
                        const double sy = (y * ss + j + 0.5) / (static_cast<double>(dpi) * ss);
                        double o[3];
                        referenceSample(p, m, under, sx, sy, dpi, o);
                        for (int c = 0; c < 3; ++c)
                            acc[c] += o[c];
                    }
                const uint8_t* q = px(img, x, y);
                for (int c = 0; c < 3; ++c)
                    worst = std::max(worst, std::abs(q[c] - acc[c] / (ss * ss)));
            }
        return worst;
    }

    // A Panel that draws two registered pictures and a missing one (host.*).
    class Gallery final : public funkgui::Panel
    {
    public:
        void attach(funkgui::HostServices&) override {}
        int  width() const override { return 120; }
        int  height() const override { return 80; }
        void tick(float) override {}
        void draw(Canvas& c, const funkgui::Theme&) override
        {
            c.rrect(0.0f, 0.0f, 120.0f, 80.0f, 0.0f, kUnder);
            c.image(kChecker, { 10.0f, 10.0f, 40.0f, 40.0f }, 6.0f);
            c.image(kMissing, { 60.0f, 10.0f, 40.0f, 40.0f });
            c.image(kWhite, { 60.0f, 60.0f, 10.0f, 10.0f }, 0.0f, 0.5f);
        }
        bool wantsFullRate() const override { return false; }
        void accessibility(std::vector<funkgui::A11yItem>&) const override {}
        uint32_t a11yRevision() const override { return 0; }
        void a11yAction(uint32_t, funkgui::A11yAction, double) override {}
        void closeGestures() override {}
    };
}

int main(int argc, char** argv)
{
    const funkgui::HeadlessGuiScope gui;                 // JUCE's GUI side, when there is JUCE: the atlas bakes there
    T::Probe P("fg.canvas.image", "", argc, argv);

    auto& fonts = funkgui::FontService::get();
    const funkgui::FontAtlasSdf& atlas = fonts.atlas();
    if (!P.eq("font.ok", fonts.ok(), 1))
        return P.finish();
    registerPictures();
    Canvas canvas(atlas);

    // ---- record: order and fields -----------------------------------------------------------------------------------
    {
        canvas.begin(frame(1.0f));
        canvas.rrect(4.0f, 4.0f, 20.0f, 10.0f, 2.0f, kUnder);              // prim 0
        canvas.shader(7, { 0.0f, 0.0f, 10.0f, 10.0f });                    // shader 0 at 1
        canvas.setTag(funkgui::tags::hint);
        canvas.image(kChecker, { 10.0f, 20.0f, 30.0f, 40.0f }, 4.0f, 0.75f, { 0.25f, 0.0f, 0.5f, 1.0f });
        canvas.setTag(0);
        canvas.shader(8, { 0.0f, 0.0f, 10.0f, 10.0f });                    // shader 1 at 1
        canvas.setLive(true);
        canvas.image(kRow, { 1.0f, 2.0f, 3.0f, 4.0f });                     // at 1, after both shaders
        canvas.setLive(false);
        canvas.rrect(4.0f, 30.0f, 20.0f, 10.0f, 2.0f, kUnder);             // prim 1
        canvas.image(kWhite, { 1.0f, 2.0f, 3.0f, 4.0f });                   // at 2
        const PrimList& l = canvas.end();
        const bool three = l.images.size() == 3 && l.prims.size() == 2 && l.shaders.size() == 2;
        P.eq("record.count", three, 1);
        if (three)
        {
            const ImageDraw& a = l.images[0];
            P.eq("record.at", a.at == 1 && l.images[1].at == 1 && l.images[2].at == 2, 1);
            P.eq("record.shader_at", a.shaderAt == 1 && l.images[1].shaderAt == 2 && l.images[2].shaderAt == 2, 1);
            P.eq("record.id", a.id == kChecker && l.images[1].id == kRow && l.images[2].id == kWhite, 1);
            P.eq("record.dst_as_given", sameRect(a.dst, { 10.0f, 20.0f, 30.0f, 40.0f }), 1);
            P.eq("record.uv_as_given", sameRect(a.uv, { 0.25f, 0.0f, 0.5f, 1.0f }), 1);
            P.eq("record.radius_opacity", a.radius == 4.0f && a.opacity == 0.75f, 1);
            P.eq("record.defaults", l.images[2].radius == 0.0f && l.images[2].opacity == 1.0f
                                        && sameRect(l.images[2].uv, { 0.0f, 0.0f, 1.0f, 1.0f }), 1);
            P.eq("record.unclipped", !a.clipped && sameRect(a.clip, Rect{}), 1);
            P.eq("record.tag", a.tag == funkgui::tags::hint && l.images[1].tag == 0, 1);
            P.eq("record.live", a.live == false && l.images[1].live == true, 1);
            P.eq("record.nothing_missing", l.missingImages, 0);
        }
        canvas.begin(frame(1.0f));
        canvas.image(kMissing, { 1.0f, 2.0f, 3.0f, 4.0f });
        canvas.end();
        canvas.begin(frame(1.0f));
        const PrimList& cleared = canvas.end();
        P.eq("record.begin_clears", cleared.images.empty() && cleared.missingImages == 0, 1);
    }

    // ---- record: sanitised arguments, and what records nothing ------------------------------------------------------
    {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();
        const Rect r{ 10.0f, 10.0f, 20.0f, 20.0f };
        canvas.begin(frame(1.0f));
        canvas.image(kWhite, r, -3.0f, 1.5f, { -0.5f, 0.25f, 1.0f, 2.0f });  // 0: radius 0, opacity 1, uv clamped
        canvas.image(kWhite, r, nan, -1.0f, { nan, 0.0f, 1.0f, 1.0f });     // 1: radius 0, opacity 0, whole uv
        canvas.image(kWhite, r, inf, nan);                                  // 2: radius 0, opacity 0
        canvas.image(kWhite, r, 0.0f, 1.0f, { 0.5f, 0.0f, 0.0f, 1.0f });    // empty uv: nothing
        canvas.image(kWhite, r, 0.0f, 1.0f, { 1.5f, 0.0f, 1.0f, 1.0f });    // uv outside: empty, nothing
        canvas.image(kWhite, { 10.0f, 10.0f, 0.0f, 10.0f });                // empty dst: nothing
        canvas.image(kWhite, { nan, 10.0f, 10.0f, 10.0f });
        canvas.image(kWhite, { 10.0f, 10.0f, inf, 10.0f });
        canvas.image(kMissing, { 10.0f, 10.0f, -1.0f, 10.0f });             // invalid dst: not even counted
        const PrimList& l = canvas.end();
        const bool three = l.images.size() == 3;
        P.eq("record.sanitised_count", three, 1);
        if (three)
        {
            P.eq("record.radius_negative_is_0", l.images[0].radius == 0.0f, 1);
            P.eq("record.opacity_clamped", l.images[0].opacity == 1.0f && l.images[1].opacity == 0.0f, 1);
            P.eq("record.uv_clamped", sameRect(l.images[0].uv, { 0.0f, 0.25f, 0.5f, 0.75f }), 1);
            P.eq("record.uv_nonfinite_is_whole", sameRect(l.images[1].uv, { 0.0f, 0.0f, 1.0f, 1.0f }), 1);
            P.eq("record.nonfinite_radius_opacity", l.images[1].radius == 0.0f && l.images[2].radius == 0.0f
                                                        && l.images[2].opacity == 0.0f, 1);
        }
        P.eq("record.invalid_not_counted", l.missingImages, 0);
    }

    // ---- record: missing ids, clips ---------------------------------------------------------------------------------
    {
        canvas.begin(frame(1.0f));
        canvas.image(kMissing, { 10.0f, 10.0f, 20.0f, 20.0f });
        canvas.pushClip({ 0.0f, 0.0f, 5.0f, 5.0f });
        canvas.image(kMissing, { 50.0f, 50.0f, 20.0f, 20.0f });              // outside the clip: still counted
        canvas.image(kWhite, { 50.0f, 50.0f, 20.0f, 20.0f });                // registered, clipped away: nothing
        canvas.image(kWhite, { 5.0f, 0.0f, 5.0f, 5.0f });                    // touching the right edge: nothing
        canvas.popClip();
        const PrimList& l = canvas.end();
        P.eq("record.missing_counted", l.missingImages, 2);
        P.eq("record.missing_and_clipped_record_nothing", l.images.size(), 0);

        canvas.begin(frame(1.0f));
        canvas.pushClip({ 10.0f, 10.0f, 100.0f, 50.0f });
        canvas.pushClip({ 50.0f, 0.0f, 100.0f, 30.0f });                   // ∩ = { 50, 10, 60, 20 }
        canvas.image(kWhite, { 40.0f, 5.0f, 30.0f, 30.0f });
        canvas.popClip();
        canvas.image(kWhite, { 0.0f, 0.0f, 20.0f, 20.0f });
        canvas.popClip();
        canvas.image(kWhite, { 0.0f, 0.0f, 20.0f, 20.0f });
        const PrimList& c = canvas.end();
        const bool three = c.images.size() == 3;
        P.eq("record.clip_count", three, 1);
        if (three)
        {
            P.eq("record.clip_intersected", c.images[0].clipped
                                                && sameRect(c.images[0].clip, { 50.0f, 10.0f, 60.0f, 20.0f }), 1);
            P.eq("record.clip_dst_not_cut", sameRect(c.images[0].dst, { 40.0f, 5.0f, 30.0f, 30.0f }), 1);
            P.eq("record.clip_outer_restored", c.images[1].clipped
                                                   && sameRect(c.images[1].clip, { 10.0f, 10.0f, 100.0f, 50.0f }), 1);
            P.eq("record.clip_none_after_pop", c.images[2].clipped, 0);
        }

        // A popped clip that drops primitives moves the positions, as for shader draws.
        canvas.begin(frame(1.0f));
        canvas.rrect(0.0f, 0.0f, 4.0f, 4.0f, 0.0f, kUnder);                // 0, kept
        canvas.pushClip({ 20.0f, 20.0f, 40.0f, 40.0f });
        canvas.image(kWhite, { 20.0f, 20.0f, 10.0f, 10.0f });              // at 1
        canvas.rrect(100.0f, 0.0f, 4.0f, 4.0f, 0.0f, kUnder);              // dropped
        canvas.image(kWhite, { 20.0f, 20.0f, 10.0f, 10.0f });              // at 2 -> 1
        canvas.rrect(30.0f, 30.0f, 4.0f, 4.0f, 0.0f, kUnder);              // 2 -> 1, kept
        canvas.rrect(0.0f, 100.0f, 4.0f, 4.0f, 0.0f, kUnder);              // dropped
        canvas.image(kWhite, { 20.0f, 20.0f, 10.0f, 10.0f });              // at 4 -> 2
        canvas.popClip();
        canvas.image(kWhite, { 0.0f, 0.0f, 10.0f, 10.0f });                // at 2
        const PrimList& m = canvas.end();
        const bool ok = m.prims.size() == 2 && m.images.size() == 4;
        P.eq("record.pop_counts", ok, 1);
        if (ok)
            P.eq("record.pop_moves_positions", m.images[0].at == 1 && m.images[1].at == 1 && m.images[2].at == 2
                                                   && m.images[3].at == 2, 1);
    }

    // ---- dump -------------------------------------------------------------------------------------------------------
    {
        canvas.begin(frame(2.0f));
        canvas.rrect(4.0f, 4.0f, 20.0f, 10.0f, 2.0f, kUnder);
        canvas.shader(7, { 1.0f, 2.0f, 3.0f, 4.0f });
        canvas.setTag(funkgui::tags::hint);
        canvas.image(kChecker, { 10.0f, 20.0f, 64.0f, 48.0f }, 6.0f, 1.0f, { 0.0f, 0.125f, 1.0f, 0.75f });
        canvas.setTag(0);
        canvas.pushClip({ 0.0f, 0.0f, 120.5f, 64.0f });
        canvas.setLive(true);
        canvas.image(kRow, { 0.25f, 1.0f, 3.0f, 4.0f }, 0.5f, 0.25f);
        canvas.setLive(false);
        canvas.popClip();
        canvas.image(kMissing, { 1.0f, 2.0f, 3.0f, 4.0f });
        canvas.image(kMissing, { 1.0f, 2.0f, 3.0f, 4.0f });
        canvas.rrect(4.0f, 30.0f, 20.0f, 10.0f, 2.0f, kOver);
        canvas.image(kWhite, { 1.0f, 2.0f, 3.0f, 4.0f });
        const PrimList l = canvas.end();                                   // a copy
        bool written = false;
        const std::string text = dumpOf(l, written);
        P.eq("dump.written", written, 1);
        const std::string line1 = "image 1 at 1 shader 1  dst 10 20 64 48  uv 0 0.125 1 0.75  radius 6  opacity 1  "
                                  "clip -  live 0  t HINT\n";
        const std::string line2 = "image 2 at 1 shader 1  dst 0.25 1 3 4  uv 0 0 1 1  radius 0.5  opacity 0.25  "
                                  "clip 0 0 120.5 64  live 1\n";
        const std::string line3 = "image 3 at 2 shader 1  dst 1 2 3 4  uv 0 0 1 1  radius 0  opacity 1  clip -  "
                                  "live 0\n";
        const size_t at1 = text.find(line1), at2 = text.find(line2), at3 = text.find(line3);
        P.eq("dump.lines_exact", at1 != std::string::npos && at2 != std::string::npos && at3 != std::string::npos, 1);
        const size_t missing = text.find("\nimages missing 2\n"), glyphs = text.find("\nglyphs missing");
        P.eq("dump.missing_line", missing != std::string::npos && glyphs < missing, 1);
        // Drawing order: p (prim 0), the shader line, the two image lines, p (prim 1), the last image line.
        const size_t p0 = text.find("\np "), p1 = text.find("\np ", p0 + 1), sh = text.find("\nshader 7 at 1");
        P.eq("dump.drawing_order", p0 != std::string::npos && p1 != std::string::npos && sh != std::string::npos
                                       && p0 < sh && sh < at1 && at1 < at2 && at2 < p1 && p1 < at3
                                       && text.find("\np ", p1 + 1) == std::string::npos, 1);

        PrimList back;
        const bool parsed = PrimList::parseText(text, back);
        P.eq("dump.parses", parsed, 1);
        bool same = parsed && back.images.size() == l.images.size() && back.prims.size() == l.prims.size()
                    && back.shaders.size() == l.shaders.size() && back.missingImages == 2;
        for (size_t i = 0; same && i < l.images.size(); ++i)
            same = sameDraw(back.images[i], l.images[i]);
        P.eq("dump.round_trip_bit_for_bit", same, 1);

        canvas.begin(frame(2.0f));
        canvas.rrect(4.0f, 4.0f, 20.0f, 10.0f, 2.0f, kUnder);
        bool plainWritten = false;
        const std::string plain = dumpOf(canvas.end(), plainWritten);
        P.eq("dump.no_image_no_line", plainWritten && plain.find("image") == std::string::npos, 1);

        const auto refuses = [&text](std::string_view from, std::string_view to)
        {
            std::string t = text;
            const size_t at = t.find(from);
            if (at == std::string::npos)
                return false;
            t.replace(at, from.size(), to);
            PrimList x;
            return !PrimList::parseText(t, x) && x.images.empty() && x.prims.empty();
        };
        P.eq("dump.refuses_wrong_at", refuses("image 3 at 2", "image 3 at 1"), 1);
        P.eq("dump.refuses_wrong_shader", refuses("image 1 at 1 shader 1", "image 1 at 1 shader 0"), 1);
        P.eq("dump.refuses_uv_outside", refuses("uv 0 0.125 1 0.75", "uv 0.5 0.125 1 0.75"), 1);
        P.eq("dump.refuses_empty_dst", refuses("dst 1 2 3 4", "dst 1 2 0 4"), 1);
        P.eq("dump.refuses_negative_radius", refuses("radius 6", "radius -6"), 1);
        P.eq("dump.refuses_opacity_above_1", refuses("opacity 0.25", "opacity 1.25"), 1);
        P.eq("dump.refuses_nan", refuses("radius 6", "radius nan"), 1);
        P.eq("dump.refuses_bad_live", refuses("live 1\n", "live 2\n"), 1);
        P.eq("dump.refuses_missing_0", refuses("images missing 2", "images missing 0"), 1);
        P.eq("dump.refuses_missing_twice", refuses("images missing 2\n", "images missing 2\nimages missing 2\n"), 1);
        PrimList v1;
        const std::string v1Text = "clear 0a0c0e\nview 160 96 dpi 1\n";
        const std::string atZero = "image 1 at 0 shader 0  dst 1 2 3 4  uv 0 0 1 1  radius 0  opacity 1  clip -  "
                                   "live 0\n";
        P.eq("dump.v2_alone_reads", PrimList::parseText("funkgui-dump 2\n" + v1Text + atZero, v1)
                                        && v1.images.size() == 1 && v1.images[0].at == 0, 1);
        P.eq("dump.refuses_in_v1", PrimList::parseText(v1Text + atZero, v1), 0);
    }

    // ---- fingerprint ------------------------------------------------------------------------------------------------
    {
        struct Args
        {
            bool  with = true;
            Rect  dst{ 10.0f, 20.0f, 150.0f, 100.0f };
            Rect  uv{ 0.0f, 0.0f, 1.0f, 1.0f };
            float radius = 4.0f, opacity = 1.0f;
            bool  live = false, clipped = false;
            uint32_t id = kChecker;
        };
        const auto record = [&canvas](const Args& a, const funkgui::FingerprintOptions& o = {})
        {
            canvas.begin(frame(1.0f));
            canvas.rrect(4.0f, 4.0f, 20.0f, 10.0f, 2.0f, kUnder);
            if (a.with)
            {
                if (a.clipped)
                    canvas.pushClip({ 0.0f, 0.0f, 100.0f, 50.0f });
                canvas.setTag(funkgui::tags::hint);
                canvas.setLive(a.live);
                canvas.image(a.id, a.dst, a.radius, a.opacity, a.uv);
                canvas.setLive(false);
                canvas.setTag(0);
                if (a.clipped)
                    canvas.popClip();
            }
            return funkgui::fingerprint(canvas.end(), o);
        };
        Args none;
        none.with = false;
        const funkgui::Fingerprint plain = record(none);
        const funkgui::Fingerprint with = record(Args{});
        P.eq("fingerprint.none_counts_0", plain.images, 0);
        P.eq("fingerprint.counted", with.images, 1);
        P.eq("fingerprint.statics_are_prims", with.statics == plain.statics && with.rrects == plain.rrects, 1);
        P.eq("fingerprint.moves_geometry", with.geometry != plain.geometry, 1);
        P.eq("fingerprint.text_untouched", with.text == plain.text, 1);
        Args a;
        a.opacity = 0.5f;
        P.eq("fingerprint.opacity_not_hashed", record(a).geometry == with.geometry, 1);
        a = Args{};
        a.dst.w = 151.0f;
        P.eq("fingerprint.dst_hashed", record(a).geometry != with.geometry, 1);
        a = Args{};
        a.uv = { 0.0f, 0.0f, 0.5f, 1.0f };
        P.eq("fingerprint.uv_hashed", record(a).geometry != with.geometry, 1);
        a = Args{};
        a.radius = 5.0f;
        P.eq("fingerprint.radius_hashed", record(a).geometry != with.geometry, 1);
        a = Args{};
        a.id = kWhite;
        P.eq("fingerprint.id_hashed", record(a).geometry != with.geometry, 1);
        a = Args{};
        a.clipped = true;
        P.eq("fingerprint.clip_hashed", record(a).geometry != with.geometry, 1);
        a = Args{};
        a.live = true;
        const funkgui::Fingerprint live = record(a);
        P.eq("fingerprint.live_left_out", live.geometry == plain.geometry && live.images == 0 && live.live == 1, 1);
        funkgui::FingerprintOptions all;
        all.excludeLive = false;
        const funkgui::Fingerprint kept = record(a, all);
        P.eq("fingerprint.live_kept_when_asked", kept.images == 1 && kept.live == 1, 1);
        P.eq("fingerprint.extents", with.maxX == 160.0f && with.maxY == 120.0f, 1);
        bool tagged = false;
        for (const auto& [tag, count] : with.tagCounts)
            tagged = tagged || (tag == funkgui::tags::hint && count == 1);
        P.eq("fingerprint.tag_counted", tagged, 1);
        P.eq("fingerprint.repeatable", record(Args{}).geometry == with.geometry, 1);
        // The registry does not enter the hash: a frame recorded while the picture is registered hashes the same
        // whatever happens to the picture afterwards.
        canvas.begin(frame(1.0f));
        canvas.image(kChecker, Args{}.dst, 4.0f);
        const PrimList copy = canvas.end();
        const uint64_t before = funkgui::fingerprint(copy).geometry;
        images::remove(kChecker);
        const uint64_t after = funkgui::fingerprint(copy).geometry;
        registerPictures();
        P.eq("fingerprint.pixels_not_hashed", before == after, 1);
    }

    // ---- steps ------------------------------------------------------------------------------------------------------
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
        const auto img = [](const DrawStep& s, uint32_t index)
        { return s.kind == DrawStep::Kind::image && s.first == index && s.count == 1; };

        PrimList l;
        l.prims.resize(4);
        ShaderDraw s;
        ImageDraw m;
        m.at = 0;
        m.shaderAt = 0;
        l.images.push_back(m);                       // image 0 at 0
        s.at = 2;
        l.shaders.push_back(s);                      // shader 0 at 2
        m.at = 2;
        m.shaderAt = 0;
        l.images.push_back(m);                       // image 1 at 2, before shader 0
        m.shaderAt = 1;
        l.images.push_back(m);                       // image 2 at 2, after shader 0
        s.at = 2;
        l.shaders.push_back(s);                      // shader 1 at 2, after image 2
        m.at = 4;
        m.shaderAt = 2;
        l.images.push_back(m);                       // image 3 after everything
        const auto& st = stepsOf(l);
        P.eq("steps.merged", st.size() == 8 && img(st[0], 0) && run(st[1], 0, 2) && img(st[2], 1) && shade(st[3], 0)
                                 && img(st[4], 2) && shade(st[5], 1) && run(st[6], 2, 2) && img(st[7], 3), 1);
        PrimList alone;
        alone.images.resize(2);
        P.eq("steps.images_alone", stepsOf(alone).size() == 2 && img(steps[0], 0) && img(steps[1], 1), 1);
        PrimList plain;
        plain.prims.resize(3);
        P.eq("steps.no_images_one_run", stepsOf(plain).size() == 1 && run(steps[0], 0, 3), 1);
        l.images.back().at = 99;
        P.eq("steps.past_the_end_drawn_last", stepsOf(l).size() == 8 && img(steps[7], 3), 1);
    }

    // ---- expand, pixels, level, sampling ----------------------------------------------------------------------------
    {
        ImageDraw m;
        m.dst = { 10.0f, 20.0f, 30.0f, 40.0f };
        funkgui::Vtx v[6];
        funkgui::expandImage(m, v);
        const auto corner = [](const funkgui::Vtx& x, float px_, float py_, float lx, float ly)
        {
            return x.x == px_ && x.y == py_ && x.d0[2] == lx && x.d0[3] == ly && x.d0[0] == lx / 30.0f
                   && x.d0[1] == ly / 40.0f && x.c0 == 0 && x.c1 == 0 && x.d1[0] == 0.0f && x.d2[2] == 0.0f;
        };
        const float e = funkgui::kImageApron;
        P.eq("expand.corners", corner(v[0], 10.0f - e, 20.0f - e, -e, -e)
                                   && corner(v[1], 40.0f + e, 20.0f - e, 30.0f + e, -e)
                                   && corner(v[2], 40.0f + e, 60.0f + e, 30.0f + e, 40.0f + e)
                                   && corner(v[5], 10.0f - e, 60.0f + e, -e, 40.0f + e)
                                   && std::memcmp(&v[3], &v[0], sizeof v[0]) == 0
                                   && std::memcmp(&v[4], &v[2], sizeof v[2]) == 0, 1);
        PrimList l;
        l.prims.resize(2);
        l.images.push_back(m);
        std::vector<funkgui::Vtx> out;
        funkgui::expand(l, out);
        P.eq("expand.prims_only", out.size(), 12);

        const auto pxIs = [](PixelRect p, int x, int y, int w, int h)
        { return p.x == x && p.y == y && p.w == w && p.h == h; };
        // The quad 8.5 .. 41.5 x 18.5 .. 61.5: centres from 8.5 (pixel 8) to below 41.5 (pixel 40).
        P.eq("pixels.apron_dpi1", pxIs(funkgui::imagePixels(m, frame(1.0f), kW, kH), 8, 18, 33, 43), 1);
        P.eq("pixels.apron_dpi2", pxIs(funkgui::imagePixels(m, frame(2.0f), 2 * kW, 2 * kH), 17, 37, 66, 86), 1);
        m.clipped = true;
        m.clip = { 0.0f, 30.0f, 25.0f, 100.0f };
        P.eq("pixels.clipped", pxIs(funkgui::imagePixels(m, frame(1.0f), kW, kH), 8, 30, 17, 31), 1);
        m.clipped = false;
        m.dst = { 250.0f, 0.0f, 10.0f, 10.0f };
        const PixelRect off = funkgui::imagePixels(m, frame(1.0f), kW, kH);
        P.eq("pixels.off_frame_empty", off.w == 0 || off.h == 0, 1);

        ImageDraw n;
        const auto level = [&n](float w, float h, float dpi, int pw, int ph)
        {
            n.dst = { 0.0f, 0.0f, w, h };
            return funkgui::imageLevel(n, dpi, pw, ph);
        };
        P.eq("level.same_size", level(64.0f, 64.0f, 1.0f, 64, 64), 0);
        P.eq("level.larger", level(200.0f, 200.0f, 1.0f, 64, 64), 0);
        P.eq("level.just_under_half", level(33.0f, 33.0f, 1.0f, 64, 64), 0);
        P.eq("level.half", level(32.0f, 32.0f, 1.0f, 64, 64), 1);
        P.eq("level.just_over_half", level(31.0f, 31.0f, 1.0f, 64, 64), 1);
        P.eq("level.quarter", level(16.0f, 16.0f, 1.0f, 64, 64), 2);
        P.eq("level.eighth", level(8.0f, 8.0f, 1.0f, 64, 64), 3);
        P.eq("level.last", level(1.0f, 1.0f, 1.0f, 64, 64), 6);
        P.eq("level.capped_at_last", level(0.25f, 0.25f, 1.0f, 64, 64), 6);
        P.eq("level.dpi", level(32.0f, 32.0f, 2.0f, 64, 64), 0);
        P.eq("level.larger_axis", level(64.0f, 16.0f, 1.0f, 64, 64), 2);
        P.eq("level.one_texel", level(0.25f, 0.25f, 1.0f, 1, 1), 0);
        n.uv = { 0.0f, 0.0f, 0.5f, 0.5f };
        P.eq("level.uv", level(32.0f, 32.0f, 1.0f, 64, 64), 0);
        n.uv = { 0.0f, 0.0f, 1.0f, 1.0f };
        P.eq("level.not_finite", level(0.0f, 0.0f, 1.0f, 64, 64), 0);

        ImageDraw c;
        c.uv = { 0.5f, 0.0f, 0.5f, 1.0f };
        const funkgui::ImageSampling a = funkgui::imageSampling(c, 0, 4, 1);
        P.eq("sampling.uv", a.u0 == 0.5f && a.v0 == 0.0f && a.du == 0.5f && a.dv == 1.0f, 1);
        P.eq("sampling.half_texel", a.loU == 0.625f && a.hiU == 0.875f, 1);
        P.eq("sampling.one_texel_tall", a.loV == 0.5f && a.hiV == 0.5f, 1);
        const funkgui::ImageSampling b = funkgui::imageSampling(c, 1, 4, 1);      // level 1: 2 x 1
        P.eq("sampling.level", b.loU == 0.75f && b.hiU == 0.75f, 1);
        c.uv = { 0.3f, 0.0f, 0.1f, 1.0f };
        const funkgui::ImageSampling d = funkgui::imageSampling(c, 0, 4, 1);
        P.eq("sampling.narrower_than_a_texel", d.loU == d.hiU && std::abs(d.loU - 0.35f) < 1e-6f, 1);
    }

    // ---- raster: known pixels at dpi 1, one sample per pixel --------------------------------------------------------
    {
        canvas.begin(frame(1.0f));
        canvas.rrect(-4.0f, -4.0f, kW + 8.0f, kH + 8.0f, 0.0f, kUnder);      // under everything
        canvas.image(kChecker, { 16.5f, 16.5f, 64.0f, 64.0f });               // a: the checker
        canvas.rrect(60.0f, 60.0f, 10.0f, 10.0f, 0.0f, kOver);               // over part of it
        canvas.image(kWhite, { 100.0f, 16.0f, 40.0f, 40.0f }, 12.0f);         // b: a rounded corner
        canvas.image(kWhite, { 150.0f, 16.0f, 20.0f, 20.0f }, 0.0f, 0.5f);    // c: opacity
        canvas.image(kHalfWhite, { 180.0f, 16.0f, 20.0f, 20.0f });            // d: a half-transparent picture
        canvas.image(kRow, { 16.0f, 100.0f, 64.0f, 16.0f }, 0.0f, 1.0f, { 0.5f, 0.0f, 0.5f, 1.0f });   // e: crop
        canvas.image(kRow, { 16.0f, 130.0f, 64.0f, 16.0f });                  // f: the whole row
        canvas.pushClip({ 120.0f, 90.0f, 100.0f, 25.0f });
        canvas.image(kWhite, { 100.0f, 80.0f, 60.0f, 60.0f });                // g: clipped
        canvas.popClip();
        canvas.image(kFine, { 200.0f, 130.0f, 8.0f, 8.0f });                  // h: shown far smaller than it is
        canvas.image(kWhite, { 220.0f, 130.0f, 8.0f, 8.0f });                 // i: removed before rasterising
        const PrimList l = canvas.end();
        images::remove(kWhite);
        const Image gone = funkgui::rasterise(l, atlas, 1);
        registerPictures();
        const Image img = funkgui::rasterise(l, atlas, 1);
        if (funkgui::writePng(img, "canvas-image.png"))     // the test's sandbox (without JUCE there is no encoder)
            std::printf("INFO     wrote canvas-image.png\n");
        P.eq("raster.size", img.w == kW && img.h == kH, 1);
        if (img.w == kW && img.h == kH)
        {
            const double r0 = kRed.r, g1 = kGreen.g;
            P.eq("raster.checker_quadrant_red", is(img, 24, 24, kRed), 1);
            P.eq("raster.checker_quadrant_green", is(img, 72, 24, kGreen), 1);
            P.eq("raster.checker_quadrant_blue", is(img, 24, 72, kBlue), 1);
            P.eq("raster.checker_quadrant_white", is(img, 74, 74, Col{ 255, 255, 255, 255 }), 1);
            // Pixel 48's centre is dst + 32: the middle between the texel centres, so half of each.
            P.eq("raster.checker_mid_top", near(img, 48, 24, r0 * 0.5, g1 * 0.5, 0.0), 1);
            P.eq("raster.checker_mid_left", near(img, 24, 48, 127.5, 0.0, 127.5), 1);
            P.eq("raster.checker_centre", near(img, 48, 48, 127.5, 127.5, 127.5), 1);
            // The left edge: pixel 16's centre lies on dst's edge (d = 0: half covered), pixel 15's one px out.
            P.eq("raster.edge_half", near(img, 16, 30, over(255, kUnder.r, 0.5), over(0, kUnder.g, 0.5),
                                          over(0, kUnder.b, 0.5)), 1);
            P.eq("raster.edge_outside", is(img, 15, 30, kUnder), 1);
            P.eq("raster.over_it", is(img, 64, 64, kOver), 1);

            P.eq("raster.corner_transparent", is(img, 100, 16, kUnder), 1);
            P.eq("raster.corner_inside", is(img, 104, 20, Col{ 255, 255, 255, 255 }), 1);
            const uint8_t* arc = px(img, 103, 19);
            P.eq("raster.corner_arc_partial", arc[0] > kUnder.r + 20 && arc[0] < 235 && arc[2] > kUnder.b + 20, 1);

            P.eq("raster.opacity", near(img, 160, 26, over(255, kUnder.r, 0.5), over(255, kUnder.g, 0.5),
                                        over(255, kUnder.b, 0.5)), 1);
            const double half = 128.0 / 255.0;
            P.eq("raster.translucent_picture", near(img, 190, 26, over(255, kUnder.r, half),
                                                    over(255, kUnder.g, half), over(255, kUnder.b, half)), 1);

            // The crop shows texels 2 and 3 only: blue to the left, yellow to the right, never green or red.
            P.eq("raster.crop_left_blue", is(img, 18, 108, kBlue), 1);
            P.eq("raster.crop_right_yellow", is(img, 77, 108, kYellow), 1);
            // Pixel 47: u = 31.5 / 64, so 0.484375 of the way from texel 2's centre (blue) to texel 3's (yellow).
            P.eq("raster.crop_middle", near(img, 47, 108, 255.0 * 0.484375, 255.0 * 0.484375,
                                            255.0 * (1.0 - 0.484375)), 1);
            P.eq("raster.whole_row_left_red", is(img, 18, 138, kRed), 1);

            P.eq("raster.clip_in", is(img, 121, 100, Col{ 255, 255, 255, 255 }), 1);
            P.eq("raster.clip_left_out", is(img, 119, 100, kUnder), 1);
            P.eq("raster.clip_top_in", is(img, 130, 90, Col{ 255, 255, 255, 255 }), 1);
            P.eq("raster.clip_top_out", is(img, 130, 89, kUnder), 1);
            P.eq("raster.clip_bottom_out", is(img, 130, 115, kUnder), 1);

            int notGrey = 0;
            for (int y = 132; y < 136; ++y)
                for (int x = 202; x < 206; ++x)
                    notGrey += near(img, x, y, 128.0, 128.0, 128.0) ? 0 : 1;
            P.eq("raster.downscaled_level_3", funkgui::imageLevel(l.images[7], 1.0f, 64, 64), 3);
            P.eq("raster.downscaled_grey", notGrey, 0);
            P.eq("raster.registered_draws", is(img, 224, 134, Col{ 255, 255, 255, 255 }), 1);
            P.eq("raster.removed_draws_nothing", gone.w == kW && is(gone, 224, 134, kUnder)
                                                     && is(gone, 104, 20, kUnder) && is(gone, 24, 24, kRed), 1);
        }
    }

    // ---- raster: the rounded edge is an rrect's ---------------------------------------------------------------------
    {
        const Rect r{ 20.25f, 10.75f, 50.5f, 30.0f };
        canvas.begin(frame(2.0f));
        canvas.rrect(-4.0f, -4.0f, kW + 8.0f, kH + 8.0f, 0.0f, kUnder);
        canvas.image(kWhite, r, 9.0f);
        const Image a = funkgui::rasterise(canvas.end(), atlas, 2);
        canvas.begin(frame(2.0f));
        canvas.rrect(-4.0f, -4.0f, kW + 8.0f, kH + 8.0f, 0.0f, kUnder);
        canvas.rrect(r.x, r.y, r.w, r.h, 9.0f, Col{ 255, 255, 255, 255 });
        const Image b = funkgui::rasterise(canvas.end(), atlas, 2);
        int worst = 0;
        for (size_t i = 0; i < a.rgba.size() && a.rgba.size() == b.rgba.size(); ++i)
            worst = std::max(worst, std::abs(static_cast<int>(a.rgba[i]) - static_cast<int>(b.rgba[i])));
        P.eq("raster.mask_is_rrect_edge", a.rgba.size() == b.rgba.size() && worst <= 1, 1);
        if (worst > 1)
            std::printf("INFO     image vs rrect: largest difference %d\n", worst);
    }

    // ---- raster: against the reference, every pixel -----------------------------------------------------------------
    for (const float dpi : { 1.0f, 2.0f })
        for (const int ss : { 1, 2 })
        {
            const std::string key = "raster.reference.dpi" + std::to_string(static_cast<int>(dpi)) + ".ss"
                                    + std::to_string(ss);
            canvas.begin(frame(dpi));
            canvas.rrect(-4.0f, -4.0f, kW + 8.0f, kH + 8.0f, 0.0f, kUnder);
            canvas.image(kChecker, { 30.3f, 20.6f, 57.7f, 41.2f }, 10.0f, 0.8f, { 0.1f, 0.2f, 0.7f, 0.6f });
            canvas.image(kFine, { 120.4f, 30.2f, 20.6f, 13.3f }, 3.0f);
            canvas.image(kRow, { 150.0f, 80.0f, 70.0f, 30.0f }, 40.0f, 1.0f, { 0.25f, 0.0f, 0.5f, 1.0f });
            const PrimList l = canvas.end();
            const Image img = funkgui::rasterise(l, atlas, ss);
            const std::shared_ptr<const images::Picture> checker = images::picture(kChecker);
            const std::shared_ptr<const images::Picture> fine = images::picture(kFine);
            const std::shared_ptr<const images::Picture> row = images::picture(kRow);
            if (!P.eq(key + ".drawn", img.w > 0 && checker && fine && row && l.images.size() == 3, 1))
                continue;
            const double d0 = referenceDiff(img, *checker, l.images[0], kUnder, dpi, ss);
            const double d1 = referenceDiff(img, *fine, l.images[1], kUnder, dpi, ss);
            const double d2 = referenceDiff(img, *row, l.images[2], kUnder, dpi, ss);
            std::printf("INFO     %s: largest differences %.3f %.3f %.3f\n", key.c_str(), d0, d1, d2);
            P.eq(key + ".checker", d0 <= 1.0, 1);
            P.eq(key + ".downscaled", d1 <= 1.0, 1);
            P.eq(key + ".pill_crop", d2 <= 1.0, 1);
        }

    // ---- host: HeadlessHost::draw()'s statistics --------------------------------------------------------------------
    {
        Gallery panel;
        funkgui::HeadlessHost host(panel, 0, 2.0f);
        const PrimList& l = host.draw();
        P.eq("host.images_drawn", l.images.size(), 2);
        P.eq("host.missing_images", l.missingImages, 1);
        const Image img = funkgui::rasterise(l, atlas, 2);
        // Logical (40, 40) is in the checker's white quadrant, (15, 15) in its red one.
        P.eq("host.raster_draws_pictures", img.w == 240 && near(img, 80, 80, 255.0, 255.0, 255.0, 0.0)
                                               && near(img, 30, 30, 255.0, 0.0, 0.0, 0.0), 1);
        images::remove(kChecker);
        const PrimList& again = host.draw();
        P.eq("host.removed_counts_missing", again.images.size() == 1 && again.missingImages == 2, 1);
        registerPictures();
    }

    for (const uint32_t id : { kChecker, kRow, kWhite, kHalfWhite, kFine })
        images::remove(id);
    return P.finish();
}

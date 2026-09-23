// FUNKGUI_TEST name=fg.softraster.area timeout=300 gpu=0
//
// fg.softraster.area (FCompressor docs/design/02-funkgui-and-ui.md §4.1–§4.4, §5.10; A §2.6 #4; G4): the AREA kind on
// both sides of the shader/mirror pair, and the new recorder shapes. Spec rows only.
//
// - Shader: shaders/fs_ui.sc (read from the source tree) defines KIND_AREA 3.0 and classifies kinds by range in the
//   order < 0.5, < 1.5, < 2.5, else, with no "> 1.5" test left; its AREA flag decode, evaluated here from the
//   constants in its own text, gives for every flags value 0..7 the bits SoftRaster reads as int(d2[3] + 0.5)
//   (bit 1 bottom, bit 2 both), and SoftRaster, rasterising one stroke-only column per flags value, strokes exactly
//   the edges that decode names; bit 0 (live) leaves the image unchanged.
// - Mirror: an AREA strip with fractional column edges tiles with no seam (every interior pixel is the single-blend
//   colour, at dpi 1 and 2, supersample 1 and 2); a pixel centre on an edge gets half coverage; kinds 0..2 match the
//   v0.3.0 FrameRender rasteriser (HR's maths, re-derived here) within 1/255 per channel.
// - Recorder: area() lays out 02 §4.1's Prim (no x apron, the y apron, centre-relative edges, the flags with live),
//   areaStrip, polyline, disc, dotted and axis record what CanvasShapes.cpp says, and degenerate calls record nothing.
// - HeadlessHost::writePng writes the last frame through SoftRaster at its physical size (false before a frame).

#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/Prim.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/canvas/SoftRaster.h>
#include <funkgui/core/Col.h>
#include <funkgui/core/Theme.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/panel/Panel.h>
#include <funkgui/test/Harness.h>
#include <funkgui/text/FontService.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace T = funkgui::test;
using funkgui::AreaEdge;
using funkgui::Canvas;
using funkgui::Col;
using funkgui::FrameInfo;
using funkgui::Image;
using funkgui::Prim;
using funkgui::PrimKind;
using funkgui::PrimList;

namespace
{
    bool sameBits(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }

    FrameInfo frame(int w, int h, float dpi, Col clear = Col{ 0, 0, 0, 255 })
    {
        FrameInfo f;
        f.logicalW = w;
        f.logicalH = h;
        f.dpi = dpi;
        f.clear = clear;
        f.textGamma = 1.0f;
        f.fixedClock = true;
        return f;
    }

    const uint8_t* px(const Image& img, int x, int y)
    {
        return &img.rgba[(static_cast<size_t>(y) * static_cast<size_t>(img.w) + static_cast<size_t>(x)) * 4u];
    }

    // ---- the shader's own text --------------------------------------------------------------------------------------

    std::string readText(const std::filesystem::path& p)
    {
        std::ifstream f(p, std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        return f ? ss.str() : std::string{};
    }

    // "float <name> = mod(floor(fl * A + B), C);" -> A, B, C.
    bool decodeConstants(const std::string& src, const char* name, double& a, double& b, double& c)
    {
        const std::regex re(std::string("float\\s+") + name
                            + "\\s*=\\s*mod\\(\\s*floor\\(\\s*fl\\s*\\*\\s*([0-9.]+)\\s*\\+\\s*([0-9.]+)\\s*\\)\\s*,"
                              "\\s*([0-9.]+)\\s*\\)\\s*;");
        std::smatch m;
        if (!std::regex_search(src, m, re))
            return false;
        a = std::stod(m[1].str());
        b = std::stod(m[2].str());
        c = std::stod(m[3].str());
        return true;
    }

    // GLSL mod(x, y) = x - y * floor(x / y), in float as the GPU evaluates it.
    float glslMod(float x, float y) { return x - y * std::floor(x / y); }

    // ---- HR's v0.3.0 FrameRender maths for kinds 0..2 (the reference the mirror must not have moved) ----------------

    float refSmoothstep(float e0, float e1, float x)
    {
        const float t = std::clamp((x - e0) / std::max(e1 - e0, 1.0e-9f), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    float lerp(float t, float a, float b) { return a + (b - a) * t; }

    // HR's frame at dpi 1 and ss 1 (its sampling grid equals SoftRaster's there): RGB bytes, alpha ignored.
    std::vector<uint8_t> hrReference(const PrimList& l, const funkgui::FontAtlasSdf& atlas)
    {
        const int W = l.info.logicalW, H = l.info.logicalH;
        struct Rgb { float r, g, b; };
        std::vector<Rgb> fb(static_cast<size_t>(W) * static_cast<size_t>(H),
                            Rgb{ l.info.clear.r / 255.0f, l.info.clear.g / 255.0f, l.info.clear.b / 255.0f });
        const auto unpack = [](uint32_t c) {
            return std::array<float, 4>{ static_cast<float>(c & 0xFFu) / 255.0f,
                                         static_cast<float>((c >> 8) & 0xFFu) / 255.0f,
                                         static_cast<float>((c >> 16) & 0xFFu) / 255.0f,
                                         static_cast<float>((c >> 24) & 0xFFu) / 255.0f };
        };
        const auto& field = atlas.pixels();
        constexpr int AW = funkgui::FontAtlasSdf::kAtlasW, AH = funkgui::FontAtlasSdf::kAtlasH;
        const auto sampleAtlas = [&](float u, float v) {
            const float fx = std::clamp(u * AW - 0.5f, 0.0f, static_cast<float>(AW) - 1.0f);
            const float fy = std::clamp(v * AH - 0.5f, 0.0f, static_cast<float>(AH) - 1.0f);
            const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
            const int x1 = std::min(x0 + 1, AW - 1), y1 = std::min(y0 + 1, AH - 1);
            const float tx = fx - static_cast<float>(x0), ty = fy - static_cast<float>(y0);
            const auto at = [&](int x, int y) {
                return field[static_cast<size_t>(y) * AW + static_cast<size_t>(x)] / 255.0f;
            };
            return lerp(ty, lerp(tx, at(x0, y0), at(x1, y0)), lerp(tx, at(x0, y1), at(x1, y1)));
        };
        const float aaGeom = 1.0f / l.info.dpi;
        for (const Prim& p : l.prims)
        {
            const int px0 = std::max(0, static_cast<int>(std::floor(p.x0)));
            const int py0 = std::max(0, static_cast<int>(std::floor(p.y0)));
            const int px1 = std::min(W, static_cast<int>(std::ceil(p.x1)));
            const int py1 = std::min(H, static_cast<int>(std::ceil(p.y1)));
            const bool isText = p.d2[2] > 0.5f && p.d2[2] < 1.5f;
            const bool isSeg = p.d2[2] > 1.5f;
            const auto c0 = unpack(p.c0), c1 = unpack(p.c1);
            for (int y = py0; y < py1; ++y)
                for (int x = px0; x < px1; ++x)
                {
                    const float fx = static_cast<float>(x) + 0.5f, fy = static_cast<float>(y) + 0.5f;
                    const float tx = (fx - p.x0) / std::max(p.x1 - p.x0, 1e-6f);
                    const float ty = (fy - p.y0) / std::max(p.y1 - p.y0, 1e-6f);
                    if (tx < 0.0f || tx > 1.0f || ty < 0.0f || ty > 1.0f)
                        continue;
                    std::array<float, 3> src{};
                    float alpha = 0.0f;
                    if (isSeg)
                    {
                        const float hw = p.d0[2], hh = p.d0[3];
                        const float padX = (p.x1 - p.x0) * 0.5f - hw, padY = (p.y1 - p.y0) * 0.5f - hh;
                        const float lx = lerp(tx, -hw - padX, hw + padX), ly = lerp(ty, -hh - padY, hh + padY);
                        const float ax = p.d1[0], ay = p.d1[1], bax = p.d1[2] - ax, bay = p.d1[3] - ay;
                        const float den = std::max(bax * bax + bay * bay, 1e-6f);
                        const float t = std::clamp(((lx - ax) * bax + (ly - ay) * bay) / den, 0.0f, 1.0f);
                        const float dx = lx - ax - bax * t, dy = ly - ay - bay * t;
                        const float d = std::sqrt(dx * dx + dy * dy) - p.d2[0];
                        const float fw = std::max(aaGeom, p.d2[1]);
                        alpha = (1.0f - refSmoothstep(-fw, fw, d)) * c0[3];
                        src = { c0[0], c0[1], c0[2] };
                    }
                    else if (!isText)
                    {
                        const float hw = p.d0[2], hh = p.d0[3];
                        const float padX = (p.x1 - p.x0) * 0.5f - hw, padY = (p.y1 - p.y0) * 0.5f - hh;
                        const float lx = lerp(tx, -hw - padX, hw + padX), ly = lerp(ty, -hh - padY, hh + padY);
                        const float rTop = lx > 0.0f ? p.d1[1] : p.d1[0];
                        const float rBot = lx > 0.0f ? p.d1[2] : p.d1[3];
                        const float r = std::min(ly > 0.0f ? rBot : rTop, std::min(hw, hh));
                        const float qx = std::abs(lx) - hw + r, qy = std::abs(ly) - hh + r;
                        const float d = std::sqrt(std::pow(std::max(qx, 0.0f), 2.0f)
                                                  + std::pow(std::max(qy, 0.0f), 2.0f))
                                      + std::min(std::max(qx, qy), 0.0f) - r;
                        const float bw = p.d2[0], fwd = std::max(aaGeom, p.d2[1]);
                        const float fa = (1.0f - refSmoothstep(-fwd, fwd, d)) * c0[3];
                        src = { c0[0], c0[1], c0[2] };
                        alpha = fa;
                        if (bw > 0.0f)
                        {
                            const float bd = std::abs(d + bw * 0.5f) - bw * 0.5f;
                            const float ba = (1.0f - refSmoothstep(-aaGeom, aaGeom, bd)) * c1[3];
                            const float oA = ba + fa * (1.0f - ba);
                            if (oA > 1e-5f)
                            {
                                for (size_t k = 0; k < 3; ++k)
                                    src[k] = (c1[k] * ba + c0[k] * fa * (1.0f - ba)) / oA;
                                alpha = oA;
                            }
                        }
                    }
                    else
                    {
                        const float dv = sampleAtlas(lerp(tx, p.d0[0], p.e0[0]), lerp(ty, p.d0[1], p.e0[1]));
                        const float a = refSmoothstep(0.5f - p.d1[0] - p.d1[1], 0.5f - p.d1[0] + p.d1[1], dv);
                        alpha = std::pow(a, p.d1[2]) * c0[3];
                        src = { c0[0], c0[1], c0[2] };
                    }
                    if (alpha <= 0.0005f)
                        continue;
                    Rgb& d = fb[static_cast<size_t>(y) * static_cast<size_t>(W) + static_cast<size_t>(x)];
                    d.r = src[0] * alpha + d.r * (1.0f - alpha);
                    d.g = src[1] * alpha + d.g * (1.0f - alpha);
                    d.b = src[2] * alpha + d.b * (1.0f - alpha);
                }
        }
        std::vector<uint8_t> out;
        out.reserve(fb.size() * 3u);
        for (const Rgb& c : fb)
            for (const float v : { c.r, c.g, c.b })
                out.push_back(static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)));
        return out;
    }

    // ---- a panel for HeadlessHost::writePng -------------------------------------------------------------------------

    class StripPanel final : public funkgui::Panel
    {
    public:
        void attach(funkgui::HostServices&) override {}
        int  width() const override { return 60; }
        int  height() const override { return 30; }
        void tick(float) override {}
        void draw(Canvas& c, const funkgui::Theme& th) override
        {
            const float xs[] = { 4.0f, 20.5f, 37.25f, 56.0f };
            const float top[] = { 6.0f, 12.0f, 9.0f, 20.0f };
            c.areaStrip(xs, 4, top, nullptr, 26.0f, th.accentDim, 1.5f, th.accent);
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
    const juce::ScopedJuceInitialiser_GUI juceInit;      // the atlas bakes through JUCE's font stack
    T::Probe P("fg.softraster.area", "", argc, argv);

    auto& fonts = funkgui::FontService::get();
    const funkgui::FontAtlasSdf& atlas = fonts.atlas();
    if (!P.eq("font.ok", fonts.ok(), 1))
        return P.finish();
    Canvas canvas(atlas);

    // ---- the shader's chain and flag decode, from its text ----------------------------------------------------------
    const std::filesystem::path shaderPath =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "shaders" / "fs_ui.sc";
    const std::string shader = readText(shaderPath);
    if (!P.eq("shader.readable", !shader.empty(), 1))
        std::printf("cannot read %s\n", shaderPath.string().c_str());
    P.eq("shader.kind_area_is_3", std::regex_search(shader, std::regex("#define\\s+KIND_AREA\\s+3\\.0\\b")), 1);
    {
        const std::regex lt05("kind\\s*<\\s*0\\.5"), lt15("kind\\s*<\\s*1\\.5"), lt25("kind\\s*<\\s*2\\.5");
        std::smatch a, b, c;
        const bool found = std::regex_search(shader, a, lt05) && std::regex_search(shader, b, lt15)
                        && std::regex_search(shader, c, lt25);
        P.eq("shader.chain_by_range_in_order",
             found && a.position(0) < b.position(0) && b.position(0) < c.position(0), 1);
        P.eq("shader.no_greater_than_kind_test",
             std::regex_search(shader, std::regex("(kind|v_data2\\.z)\\s*>\\s*[0-9]")), 0);
    }
    double ba = 0, bb = 0, bc = 0, ta = 0, tb = 0, tc = 0;
    const bool haveBottom = decodeConstants(shader, "bottom", ba, bb, bc);
    const bool haveBoth = decodeConstants(shader, "both", ta, tb, tc);
    P.eq("shader.decode_found", haveBottom && haveBoth, 1);

    // ---- flags 0..7: shader decode == mirror decode == what the mirror strokes --------------------------------------
    {
        // One stroke-only column, top edge 10 -> 14, bottom 26 -> 22, stroke 2 px, white on black, dpi 1, ss 1.
        canvas.begin(frame(40, 40, 1.0f));
        canvas.area(10.0f, 30.0f, 10.0f, 14.0f, 26.0f, 22.0f, Col{ 0, 0, 0, 0 }, 2.0f, Col{ 255, 255, 255, 255 });
        const PrimList base = canvas.end();
        std::vector<Image> images;
        for (int f = 0; f < 8; ++f)
        {
            PrimList l = base;
            l.prims[0].d2[3] = static_cast<float>(f);
            images.push_back(funkgui::rasterise(l, atlas, 1));
        }
        for (int f = 0; f < 8; ++f)
        {
            const std::string key = "flags.f" + std::to_string(f);
            const float ff = static_cast<float>(f);
            const bool sBottom = glslMod(std::floor(ff * static_cast<float>(ba) + static_cast<float>(bb)),
                                         static_cast<float>(bc)) > 0.5f;
            const bool sBoth = glslMod(std::floor(ff * static_cast<float>(ta) + static_cast<float>(tb)),
                                       static_cast<float>(tc)) > 0.5f;
            const auto bits = static_cast<uint32_t>(ff + 0.5f);                   // the mirror's int(d2[3] + 0.5)
            P.eq(key + ".decode_bottom", sBottom, (bits & funkgui::pflag::strokeBottom) != 0);
            P.eq(key + ".decode_both", sBoth, (bits & funkgui::pflag::strokeBoth) != 0);

            // Pixel column 20 (centre x 20.5, t 0.525): the top edge is at y 12.1 and the bottom at 23.9, so rows 12
            // and 23 (centres 12.5, 23.5) lie within 0.4 px of them, well inside the 1 px half-width stroke.
            const Image& img = images[static_cast<size_t>(f)];
            const bool topLit = px(img, 20, 12)[0] > 128, bottomLit = px(img, 20, 23)[0] > 128;
            const bool midDark = px(img, 20, 17)[0] < 8;
            P.eq(key + ".strokes_top", topLit, !sBottom || sBoth);
            P.eq(key + ".strokes_bottom", bottomLit, sBottom || sBoth);
            P.eq(key + ".no_fill", midDark, 1);
            P.eq(key + ".live_bit_inert", img.rgba == images[static_cast<size_t>(f ^ 1)].rgba, 1);
        }
    }

    // ---- tiling: a strip with fractional column edges has no seam ---------------------------------------------------
    for (const float dpi : { 1.0f, 2.0f })
        for (const int ss : { 1, 2 })
        {
            const std::string key = "tile.dpi" + std::to_string(static_cast<int>(dpi)) + ".ss" + std::to_string(ss);
            canvas.begin(frame(48, 32, dpi));
            const float xs[] = { 4.0f, 9.5f, 13.25f, 20.0f, 31.75f, 40.0f };
            const float top[] = { 8.0f, 8.0f, 8.0f, 8.0f, 8.0f, 8.0f };
            const Col fill{ 200, 100, 50, 128 };
            canvas.areaStrip(xs, 6, top, nullptr, 24.0f, fill);
            const Image img = funkgui::rasterise(canvas.end(), atlas, ss);
            P.eq(key + ".size", img.w == static_cast<int>(48 * dpi) && img.h == static_cast<int>(32 * dpi), 1);
            // Interior: logical x 4..40, y 10..22 (more than one AA width inside both edges).
            const int x0 = static_cast<int>(4 * dpi), x1 = static_cast<int>(40 * dpi);
            const int y0 = static_cast<int>(10 * dpi), y1 = static_cast<int>(22 * dpi);
            const uint8_t* ref = px(img, x0, y0);
            const float a = 128.0f / 255.0f;
            const auto expect = [a](int c) {
                return static_cast<int>(static_cast<float>(c) / 255.0f * a * 255.0f + 0.5f);
            };
            bool uniform = true;
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x)
                    uniform = uniform && std::equal(ref, ref + 4, px(img, x, y));
            P.eq(key + ".seamless", uniform, 1);
            P.eq(key + ".single_blend", std::abs(ref[0] - expect(200)) <= 1 && std::abs(ref[1] - expect(100)) <= 1
                                            && std::abs(ref[2] - expect(50)) <= 1 && ref[3] == 255, 1);
            const uint8_t* left = px(img, x0 - 1, (y0 + y1) / 2);
            const uint8_t* right = px(img, x1, (y0 + y1) / 2);
            P.eq(key + ".outside_is_ground", left[0] == 0 && left[1] == 0 && left[2] == 0 && right[0] == 0
                                                 && right[1] == 0 && right[2] == 0, 1);
        }

    // ---- AA: a pixel centre on the edge is half covered -------------------------------------------------------------
    {
        canvas.begin(frame(40, 20, 1.0f));
        canvas.area(0.0f, 40.0f, 5.5f, 5.5f, 15.5f, 15.5f, Col{ 255, 255, 255, 255 });
        const Image img = funkgui::rasterise(canvas.end(), atlas, 1);
        P.near("aa.top_edge_half", px(img, 20, 5)[0], 127.5, 0.51);
        P.near("aa.bottom_edge_half", px(img, 20, 15)[0], 127.5, 0.51);
        P.eq("aa.inside_full", px(img, 20, 10)[0], 255);
        P.eq("aa.outside_empty", px(img, 20, 3)[0] + px(img, 20, 17)[0], 0);
    }

    // ---- kinds 0..2: the mirror matches HR's maths (v0.3.0 FrameRender) ---------------------------------------------
    {
        const funkgui::Theme th = funkgui::Theme::graphite();
        FrameInfo info = frame(240, 120, 1.0f, th.ground);
        info.textGamma = th.textGamma;
        canvas.begin(info);
        canvas.rrect(8.0f, 8.0f, 40.0f, 24.0f, 0.0f, th.ink52);
        canvas.rrect(56.25f, 8.5f, 40.0f, 24.0f, 6.0f, th.ink32, 1.5f, th.accent);
        canvas.rrect4(104.1f, 8.2f, 48.3f, 24.4f, 0.0f, 8.0f, 2.0f, 12.0f, th.accentDim, 0.0f, {}, 2.5f);
        canvas.rrect(160.0f, 8.0f, 24.0f, 24.0f, 12.0f, th.ink70.withAlpha(0.0f), 1.5f, th.ink70);
        canvas.hairlineH(8.0f, 40.3f, 180.0f, th.ink16);
        canvas.segment(8.0f, 80.0f, 90.0f, 50.0f, 1.5f, th.ink100);
        canvas.segment(100.0f, 50.0f, 100.0f, 90.0f, 2.0f, th.ink52, 0.5f);
        canvas.text("THRESHOLD \xE2\x88\x92" "12.5 DB", 120.0f, 56.0f, funkgui::type::kLabel, th.ink100);
        canvas.text("4:1", 120.0f, 76.0f, funkgui::type::kValueP, th.ink70);
        const PrimList& l = canvas.end();
        const Image img = funkgui::rasterise(l, atlas, 1);
        const std::vector<uint8_t> ref = hrReference(l, atlas);
        int worst = 0;
        for (int y = 0; y < img.h; ++y)
            for (int x = 0; x < img.w; ++x)
                for (int k = 0; k < 3; ++k)
                    worst = std::max(worst, std::abs(static_cast<int>(px(img, x, y)[k])
                                                     - static_cast<int>(ref[(static_cast<size_t>(y) * 240u
                                                                              + static_cast<size_t>(x)) * 3u
                                                                             + static_cast<size_t>(k)])));
        P.le("hr_kinds.worst_channel_delta", worst, 1);
        P.eq("hr_kinds.no_missing_glyphs", l.missingGlyphs, 0);
    }

    // ---- the recorder's shapes --------------------------------------------------------------------------------------
    {
        canvas.begin(frame(100, 100, 2.0f));
        canvas.setTag(funkgui::tags::slotTrack);
        canvas.area(10.0f, 20.0f, 5.0f, 7.0f, 30.0f, 28.0f, Col{ 1, 2, 3, 4 }, 2.0f, Col{ 5, 6, 7, 8 }, AreaEdge::both);
        canvas.setLive(true);
        canvas.area(20.0f, 30.0f, 5.0f, 5.0f, 9.0f, 9.0f, Col{ 1, 2, 3, 4 }, 0.0f, {}, AreaEdge::bottom);
        canvas.setLive(false);
        canvas.setTag(0);
        const PrimList& l = canvas.end();
        const bool two = l.prims.size() == 2;
        P.eq("record.area.count", static_cast<int64_t>(l.prims.size()), 2);
        if (two)
        {
            const Prim& p = l.prims[0];
            // y extent 5 - 1.5 - 1 .. 30 + 1.5 + 1; centre y 17.5; hw 5, hh 15.
            P.eq("record.area.quad", sameBits(p.x0, 10.0f) && sameBits(p.x1, 20.0f) && sameBits(p.y0, 2.5f)
                                         && sameBits(p.y1, 32.5f), 1);
            P.eq("record.area.local", sameBits(p.d0[0], -5.0f) && sameBits(p.d0[1], -15.0f) && sameBits(p.d0[2], 5.0f)
                                          && sameBits(p.d0[3], 15.0f) && sameBits(p.e0[0], 5.0f)
                                          && sameBits(p.e0[1], 15.0f), 1);
            P.eq("record.area.edges", sameBits(p.d1[0], -12.5f) && sameBits(p.d1[1], -10.5f)
                                          && sameBits(p.d1[2], 12.5f) && sameBits(p.d1[3], 10.5f), 1);
            P.eq("record.area.d2", sameBits(p.d2[0], 1.0f) && sameBits(p.d2[1], 0.0f) && sameBits(p.d2[2], 3.0f)
                                       && sameBits(p.d2[3], 4.0f), 1);
            P.eq("record.area.colours_tag", p.c0 == 0x04030201u && p.c1 == 0x08070605u
                                                && p.tag == funkgui::tags::slotTrack, 1);
            const Prim& q = l.prims[1];
            P.eq("record.area.live_bottom_flags", sameBits(q.d2[3], 3.0f) && sameBits(q.d2[0], 0.0f)
                                                      && sameBits(q.y0, 3.5f) && sameBits(q.y1, 10.5f), 1);
        }

        canvas.begin(frame(100, 100, 2.0f));
        canvas.area(10.0f, 10.0f, 0.0f, 0.0f, 5.0f, 5.0f, Col{});                  // no width
        canvas.area(10.0f, 5.0f, 0.0f, 0.0f, 5.0f, 5.0f, Col{});                   // inverted
        canvas.area(std::nanf(""), 5.0f, 0.0f, 0.0f, 5.0f, 5.0f, Col{});           // NaN end
        P.eq("record.area.degenerate_none", static_cast<int64_t>(canvas.end().prims.size()), 0);

        canvas.begin(frame(100, 100, 2.0f));
        const float xs[] = { 0.0f, 10.0f, 10.0f, 25.0f, 20.0f, 40.0f };             // two non-increasing pairs
        const float top[] = { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f };
        const float bot[] = { 50.0f, 51.0f, 52.0f, 53.0f, 54.0f, 55.0f };
        canvas.areaStrip(xs, 6, top, bot, 0.0f, Col{});
        const PrimList& s = canvas.end();
        P.eq("record.strip.count", static_cast<int64_t>(s.prims.size()), 3);
        P.eq("record.strip.tiles", s.prims.size() == 3 && sameBits(s.prims[0].x1, 10.0f)
                                       && sameBits(s.prims[1].x0, 10.0f) && sameBits(s.prims[1].x1, 25.0f)
                                       && sameBits(s.prims[2].x0, 20.0f), 1);
        canvas.begin(frame(100, 100, 2.0f));
        canvas.areaStrip(xs, 3, top, nullptr, 60.0f, Col{});
        const PrimList& f = canvas.end();
        P.eq("record.strip.flat_base", f.prims.size() == 1 && sameBits(f.prims[0].y1, 61.5f), 1);
        canvas.begin(frame(100, 100, 2.0f));
        canvas.areaStrip(xs, 1, top, nullptr, 60.0f, Col{});
        canvas.areaStrip(nullptr, 6, top, nullptr, 60.0f, Col{});
        canvas.areaStrip(xs, 6, nullptr, nullptr, 60.0f, Col{});
        P.eq("record.strip.degenerate_none", static_cast<int64_t>(canvas.end().prims.size()), 0);

        canvas.begin(frame(100, 100, 2.0f));
        const float ys[] = { 5.0f, 9.0f, 2.0f, 7.0f, 3.0f };
        canvas.polyline(top, ys, 5, 1.5f, Col{ 10, 20, 30, 255 });
        const PrimList& pl = canvas.end();
        bool segs = pl.prims.size() == 4;
        for (const Prim& p : pl.prims)
            segs = segs && sameBits(p.d2[2], 2.0f) && sameBits(p.d2[0], 0.75f);
        P.eq("record.polyline.segments", segs, 1);

        canvas.begin(frame(100, 100, 2.0f));
        canvas.disc(50.0f, 40.0f, 4.0f, Col{ 1, 1, 1, 255 }, 1.0f, Col{ 2, 2, 2, 255 });
        canvas.disc(50.0f, 40.0f, 0.0f, Col{ 1, 1, 1, 255 });
        const PrimList& d = canvas.end();
        P.eq("record.disc.rrect", d.prims.size() == 1 && sameBits(d.prims[0].d2[2], 0.0f)
                                      && sameBits(d.prims[0].x0, 44.5f) && sameBits(d.prims[0].y1, 45.5f)
                                      && sameBits(d.prims[0].d1[0], 4.0f) && sameBits(d.prims[0].d2[0], 1.0f), 1);

        canvas.begin(frame(100, 100, 2.0f));
        canvas.dotted(10.2f, 20.3f, 10.0f, 2.5f, Col{ 3, 3, 3, 255 });            // k * 2.5 < 10: 4 dots
        canvas.dotted(10.0f, 20.0f, 10.0f, 0.0f, Col{});
        canvas.dotted(10.0f, 20.0f, -1.0f, 2.0f, Col{});
        const PrimList& dt = canvas.end();
        bool dots = dt.prims.size() == 4;
        for (size_t k = 0; dots && k < dt.prims.size(); ++k)
        {
            const Prim& p = dt.prims[k];
            const float x = std::floor((10.2f + 2.5f * static_cast<float>(k)) * 2.0f + 0.5f) / 2.0f;
            dots = sameBits(p.x0, x - 1.5f) && sameBits(p.y0, 20.5f - 1.5f) && sameBits(p.d0[2], 0.25f)
                && sameBits(p.d0[3], 0.25f);
        }
        P.eq("record.dotted.snapped_device_px", dots, 1);

        canvas.begin(frame(100, 100, 2.0f));
        const funkgui::AxisMap mx{ 1.0f, 2.0f, 3.0f, 4.0f, false }, my{ 5.0f, 6.0f, 7.0f, 8.0f, true };
        canvas.axis(300, &mx, &my);
        canvas.axis(301, nullptr, &my);
        const PrimList& ax = canvas.end();
        P.eq("record.axis", ax.prims.empty() && ax.axes.size() == 2 && ax.axes[0].tag == 300 && ax.axes[0].hasX
                                && ax.axes[0].hasY && sameBits(ax.axes[0].x.v1, 4.0f) && ax.axes[0].y.log
                                && ax.axes[1].tag == 301 && !ax.axes[1].hasX && ax.axes[1].hasY, 1);
    }

    // ---- rasterise sizes and HeadlessHost::writePng -----------------------------------------------------------------
    {
        canvas.begin(frame(101, 33, 1.5f));
        canvas.rrect(2.0f, 2.0f, 10.0f, 10.0f, 2.0f, Col{ 255, 255, 255, 255 });
        const Image img = funkgui::rasterise(canvas.end(), atlas, 3);
        P.eq("raster.physical_size", img.w == 152 && img.h == 50 && img.rgba.size() == 152u * 50u * 4u, 1);
        canvas.begin(frame(0, 10, 2.0f));
        P.eq("raster.empty_frame", funkgui::rasterise(canvas.end(), atlas, 2).w, 0);
        P.eq("png.empty_image_refused", funkgui::writePng(Image{}, "empty.png"), 0);
        P.eq("png.bad_path_refused", funkgui::writePng(img, "no-such-dir/x.png"), 0);

        StripPanel panel;
        funkgui::HeadlessHost host(panel, 0, 2.0f);
        P.eq("host.png_before_draw", host.writePng("host.png", 2), 0);
        const PrimList& l = host.draw();
        const bool wrote = host.writePng("host.png", 2);
        P.eq("host.png_written", wrote, 1);
        const juce::File file = juce::File::getCurrentWorkingDirectory().getChildFile("host.png");
        const juce::Image back = juce::ImageFileFormat::loadFrom(file);
        const Image want = funkgui::rasterise(l, atlas, 2);
        bool same = back.isValid() && back.getWidth() == 120 && back.getHeight() == 60 && want.w == 120;
        for (int y = 0; same && y < want.h; ++y)
            for (int x = 0; same && x < want.w; ++x)
            {
                const juce::Colour c = back.getPixelAt(x, y);
                const uint8_t* w = px(want, x, y);
                same = c.getRed() == w[0] && c.getGreen() == w[1] && c.getBlue() == w[2] && c.getAlpha() == w[3];
            }
        P.eq("host.png_is_the_frame", same, 1);
        P.eq("host.png_no_partial_left", file.getSiblingFile("host.png.partial").exists(), 0);
    }
    return P.finish();
}

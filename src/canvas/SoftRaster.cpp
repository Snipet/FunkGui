#include <funkgui/canvas/SoftRaster.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

// The CPU rasteriser (02 §4.3, §5.10): shaders/fs_ui.sc evaluated per sample, so a PrimList can be looked at with no
// GPU. The kind chain and every branch mirror the shader line for line, and the two change together (A §2.6 #4); what
// the GPU does around the shader is mirrored too:
//
// - Coverage: a sample belongs to a primitive when it lies in the half-open quad [x0, x1) x [y0, y1) (the rasteriser's
//   top-left rule on an axis-aligned quad), so AREA columns that share an edge tile with no gap and no double cover.
// - Varyings: the local coordinates (d0.xy, text UVs) interpolate linearly from the TL corner's (d0[0], d0[1]) to the
//   BR corner's (e0[0], e0[1]); d0.zw, d1, d2 and the colours are flat (canvas/Expand.h's six vertices).
// - aa = 1 / dpi (u_viewSize.z, one physical pixel in logical px) whatever the supersampling.
// - The atlas is sampled bilinearly with clamped edges and no mips (BgfxContext's R8 texture, texture2DLod level 0).
// - Blending is BGFX_STATE_BLEND_ALPHA on a non-sRGB target: rgb = src.rgb * a + dst.rgb * (1 - a) in the stored
//   (gamma-encoded) values. The image's alpha composites source-over (a + dst.a * (1 - a)), so the frame over its
//   opaque clear colour is opaque. The GPU's 8-bit target rounds after every primitive; here the frame accumulates in
//   float and is rounded once, after the box filter.
// - Classification by range, as the shader: d2[2] < 0.5 rrect, < 1.5 text, < 2.5 segment, else area. The AREA flags
//   are decoded as int(d2[3] + 0.5) (bit 1 strokes the bottom edge, bit 2 both; bit 0, live, changes nothing), where
//   the shader takes mod(floor(f * 0.5 + 0.01), 2) and mod(floor(f * 0.25 + 0.01), 2); fg.softraster.area checks the
//   two decodes agree for flags 0..7 against the shader's own text.
//
// The image is the frame's physical size, round(logical * dpi) per axis (dpi <= 0.05 or not finite draws at 1x, as
// Canvas::begin), sampled at supersample x supersample points per physical pixel (1..4) and box-filtered down.

namespace funkgui
{
    namespace
    {
        struct Rgba
        {
            float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
        };

        Rgba unpack(uint32_t c) noexcept
        {
            return { static_cast<float>(c & 0xFFu) / 255.0f, static_cast<float>((c >> 8) & 0xFFu) / 255.0f,
                     static_cast<float>((c >> 16) & 0xFFu) / 255.0f, static_cast<float>((c >> 24) & 0xFFu) / 255.0f };
        }

        float clamp01(float x) noexcept { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

        // GLSL smoothstep. GLSL leaves e0 >= e1 undefined; like the previous CPU rasteriser (HR FrameRender) the width
        // is kept above 1e-9, so a zero-width text feather is a step instead of a NaN.
        float smoothstep(float e0, float e1, float x) noexcept
        {
            const float t = clamp01((x - e0) / std::max(e1 - e0, 1.0e-9f));
            return t * t * (3.0f - 2.0f * t);
        }

        float mixf(float a, float b, float t) noexcept { return a + (b - a) * t; }

        // texture2DLod(s_texColor, uv, 0).x: bilinear over texel centres, clamped to the edge texels.
        float sampleAtlas(const std::vector<uint8_t>& px, float u, float v) noexcept
        {
            constexpr int W = FontAtlasSdf::kAtlasW, H = FontAtlasSdf::kAtlasH;
            if (px.size() < static_cast<size_t>(W) * static_cast<size_t>(H))
                return 0.0f;
            const float fx = std::clamp(u * static_cast<float>(W) - 0.5f, 0.0f, static_cast<float>(W - 1));
            const float fy = std::clamp(v * static_cast<float>(H) - 0.5f, 0.0f, static_cast<float>(H - 1));
            const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
            const int x1 = std::min(x0 + 1, W - 1), y1 = std::min(y0 + 1, H - 1);
            const float tx = fx - static_cast<float>(x0), ty = fy - static_cast<float>(y0);
            const auto at = [&px](int x, int y) noexcept {
                return static_cast<float>(px[static_cast<size_t>(y) * W + static_cast<size_t>(x)]) / 255.0f;
            };
            return mixf(mixf(at(x0, y0), at(x1, y0), tx), mixf(at(x0, y1), at(x1, y1), tx), ty);
        }

        // The flags as the mirror reads them (02 §4.3): int(d2[3] + 0.5), 0 when that is not a small non-negative
        // integer. fg.softraster.area proves the shader's float decode gives the same bits for flags 0..7.
        uint32_t flagsOf(const Prim& p) noexcept
        {
            const float f = p.d2[3] + 0.5f;
            return f >= 1.0f && f < 2147483520.0f ? static_cast<uint32_t>(f) : 0u;
        }

        // One primitive's fs_ui.sc output at local position (lx, ly), straight alpha.
        Rgba shade(const Prim& p, const Rgba& c0, const Rgba& c1, float lx, float ly, float aa,
                   const std::vector<uint8_t>& atlas) noexcept
        {
            const float kind = p.d2[2];
            if (kind < 0.5f)
            {
                // ---- rrect (fs_ui.sc: per-corner radii, inside border, optional glow)
                const float bx = p.d0[2], by = p.d0[3];
                const float rTop = lx > 0.0f ? p.d1[1] : p.d1[0];
                const float rBot = lx > 0.0f ? p.d1[2] : p.d1[3];
                float r = ly > 0.0f ? rBot : rTop;
                r = std::min(r, std::min(bx, by));
                const float qx = std::abs(lx) - bx + r, qy = std::abs(ly) - by + r;
                const float mx = std::max(qx, 0.0f), my = std::max(qy, 0.0f);
                const float d = std::sqrt(mx * mx + my * my) + std::min(std::max(qx, qy), 0.0f) - r;
                const float bw = p.d2[0], soft = p.d2[1];
                const float fw = std::max(aa, soft);
                const float fa = (1.0f - smoothstep(-fw, fw, d)) * c0.a;
                Rgba col{ c0.r, c0.g, c0.b, fa };
                if (bw > 0.0f)
                {
                    const float bd = std::abs(d + bw * 0.5f) - bw * 0.5f;
                    const float ba = (1.0f - smoothstep(-aa, aa, bd)) * c1.a;
                    const float outA = ba + fa * (1.0f - ba);
                    const float k = 1.0f / std::max(outA, 1e-5f);
                    col = { (c1.r * ba + c0.r * fa * (1.0f - ba)) * k, (c1.g * ba + c0.g * fa * (1.0f - ba)) * k,
                            (c1.b * ba + c0.b * fa * (1.0f - ba)) * k, outA };
                }
                return col;
            }
            if (kind < 1.5f)
            {
                // ---- text (lx, ly are the atlas UVs)
                const float d = sampleAtlas(atlas, lx, ly);
                const float weight = p.d1[0], taa = p.d1[1];
                const float a = smoothstep(0.5f - weight - taa, 0.5f - weight + taa, d);
                return { c0.r, c0.g, c0.b, c0.a * std::pow(a, p.d1[2]) };
            }
            if (kind < 2.5f)
            {
                // ---- segment: a capsule from a to b, half-width d2[0]
                const float ax = p.d1[0], ay = p.d1[1];
                const float bax = p.d1[2] - ax, bay = p.d1[3] - ay;
                const float t = clamp01(((lx - ax) * bax + (ly - ay) * bay) / std::max(bax * bax + bay * bay, 1e-6f));
                const float dx = lx - ax - bax * t, dy = ly - ay - bay * t;
                const float d = std::sqrt(dx * dx + dy * dy) - p.d2[0];
                const float fw = std::max(aa, p.d2[1]);
                return { c0.r, c0.g, c0.b, (1.0f - smoothstep(-fw, fw, d)) * c0.a };
            }

            // ---- area: one column between two straight edges (02 §4.1)
            const float hw = std::max(p.d0[2], 1e-4f);
            const float t = clamp01(lx / (2.0f * hw) + 0.5f);
            const float yT = mixf(p.d1[0], p.d1[1], t);
            const float yB = mixf(p.d1[2], p.d1[3], t);
            const float sT = (p.d1[1] - p.d1[0]) / (2.0f * hw);
            const float sB = (p.d1[3] - p.d1[2]) / (2.0f * hw);
            const float dT = (yT - ly) / std::sqrt(1.0f + sT * sT);          // > 0 above the top edge
            const float dB = (ly - yB) / std::sqrt(1.0f + sB * sB);          // > 0 below the bottom edge
            const float fa = (1.0f - smoothstep(-aa, aa, std::max(dT, dB))) * c0.a;
            const uint32_t fl = flagsOf(p);
            const bool bottom = (fl & pflag::strokeBottom) != 0;
            const bool both = (fl & pflag::strokeBoth) != 0;
            const float dS = both ? std::min(std::abs(dT), std::abs(dB)) : (bottom ? std::abs(dB) : std::abs(dT));
            const float hs = p.d2[0];
            const float sa = hs > 0.0f ? (1.0f - smoothstep(hs - aa, hs + aa, dS)) * c1.a : 0.0f;
            const float oA = sa + fa * (1.0f - sa);
            const float k = 1.0f / std::max(oA, 1e-5f);
            return { (c1.r * sa + c0.r * fa * (1.0f - sa)) * k, (c1.g * sa + c0.g * fa * (1.0f - sa)) * k,
                     (c1.b * sa + c0.b * fa * (1.0f - sa)) * k, oA };
        }

        // The first sample index whose centre (i + 0.5) / scale is >= edge.
        int firstAtOrAfter(float edge, float scale) noexcept
        {
            const float f = std::ceil(edge * scale - 0.5f);
            return f < -1.0e9f ? -1000000000 : (f > 1.0e9f ? 1000000000 : static_cast<int>(f));
        }
    }

    Image rasterise(const PrimList& list, const FontAtlasSdf& atlas, int supersample)
    {
        Image img;
        const float dpi = std::isfinite(list.info.dpi) && list.info.dpi > 0.05f ? list.info.dpi : 1.0f;
        const int ss = std::clamp(supersample, 1, 4);
        const double pw = std::round(static_cast<double>(list.info.logicalW) * dpi);
        const double ph = std::round(static_cast<double>(list.info.logicalH) * dpi);
        constexpr double kMaxSide = 16384.0;                  // a larger frame is not a panel: nothing is drawn
        if (!(pw >= 1.0 && ph >= 1.0 && pw <= kMaxSide && ph <= kMaxSide))
            return img;
        img.w = static_cast<int>(pw);
        img.h = static_cast<int>(ph);

        const int W = img.w * ss, H = img.h * ss;
        const float scale = dpi * static_cast<float>(ss);     // samples per logical px
        const float aa = 1.0f / dpi;
        const Col clear = list.info.clear;
        std::vector<Rgba> fb(static_cast<size_t>(W) * static_cast<size_t>(H),
                             Rgba{ static_cast<float>(clear.r) / 255.0f, static_cast<float>(clear.g) / 255.0f,
                                   static_cast<float>(clear.b) / 255.0f, static_cast<float>(clear.a) / 255.0f });
        const std::vector<uint8_t>& field = atlas.pixels();

        for (const Prim& p : list.prims)
        {
            if (!(p.x1 > p.x0) || !(p.y1 > p.y0))
                continue;                                     // empty or not finite: the GPU draws nothing either
            const int sx0 = std::max(0, firstAtOrAfter(p.x0, scale));
            const int sx1 = std::min(W, firstAtOrAfter(p.x1, scale));
            const int sy0 = std::max(0, firstAtOrAfter(p.y0, scale));
            const int sy1 = std::min(H, firstAtOrAfter(p.y1, scale));
            if (sx0 >= sx1 || sy0 >= sy1)
                continue;

            const Rgba c0 = unpack(p.c0), c1 = unpack(p.c1);
            const float qw = p.x1 - p.x0, qh = p.y1 - p.y0;
            for (int y = sy0; y < sy1; ++y)
            {
                const float fy = (static_cast<float>(y) + 0.5f) / scale;
                if (fy < p.y0 || !(fy < p.y1))
                    continue;                                 // the half-open rule, exactly, at the rounding edge
                const float ly = mixf(p.d0[1], p.e0[1], (fy - p.y0) / qh);
                Rgba* row = &fb[static_cast<size_t>(y) * static_cast<size_t>(W)];
                for (int x = sx0; x < sx1; ++x)
                {
                    const float fx = (static_cast<float>(x) + 0.5f) / scale;
                    if (fx < p.x0 || !(fx < p.x1))
                        continue;
                    const float lx = mixf(p.d0[0], p.e0[0], (fx - p.x0) / qw);
                    const Rgba s = shade(p, c0, c1, lx, ly, aa, field);
                    const float a = clamp01(s.a);
                    if (!(a > 0.0f))
                        continue;
                    Rgba& d = row[x];
                    d.r = s.r * a + d.r * (1.0f - a);
                    d.g = s.g * a + d.g * (1.0f - a);
                    d.b = s.b * a + d.b * (1.0f - a);
                    d.a = a + d.a * (1.0f - a);
                }
            }
        }

        // Box filter to the physical size and round once.
        img.rgba.assign(static_cast<size_t>(img.w) * static_cast<size_t>(img.h) * 4u, 0);
        const float inv = 1.0f / static_cast<float>(ss * ss);
        const auto to8 = [](float v) noexcept { return static_cast<uint8_t>(clamp01(v) * 255.0f + 0.5f); };
        for (int y = 0; y < img.h; ++y)
            for (int x = 0; x < img.w; ++x)
            {
                Rgba acc;
                for (int j = 0; j < ss; ++j)
                {
                    const Rgba* row = &fb[static_cast<size_t>(y * ss + j) * static_cast<size_t>(W)];
                    for (int i = 0; i < ss; ++i)
                    {
                        const Rgba& s = row[x * ss + i];
                        acc.r += s.r;
                        acc.g += s.g;
                        acc.b += s.b;
                        acc.a += s.a;
                    }
                }
                const size_t at = static_cast<size_t>(y) * static_cast<size_t>(img.w) + static_cast<size_t>(x);
                uint8_t* o = &img.rgba[at * 4u];
                o[0] = to8(acc.r * inv);
                o[1] = to8(acc.g * inv);
                o[2] = to8(acc.b * inv);
                o[3] = to8(acc.a * inv);
            }
        return img;
    }

    bool writePng(const Image& img, const char* path)
    {
        if (path == nullptr || *path == 0 || img.w <= 0 || img.h <= 0
            || img.rgba.size() != static_cast<size_t>(img.w) * static_cast<size_t>(img.h) * 4u)
            return false;

        juce::Image out(juce::Image::ARGB, img.w, img.h, false);
        {
            const juce::Image::BitmapData bd(out, juce::Image::BitmapData::writeOnly);
            for (int y = 0; y < img.h; ++y)
                for (int x = 0; x < img.w; ++x)
                {
                    const uint8_t* s = &img.rgba[(static_cast<size_t>(y) * static_cast<size_t>(img.w)
                                                  + static_cast<size_t>(x)) * 4u];
                    bd.setPixelColour(x, y, juce::Colour(s[0], s[1], s[2], s[3]));   // premultiplies
                }
        }

        // Written beside the destination and renamed onto it once complete, so a reader never sees half a file.
        const juce::File dest = juce::File::getCurrentWorkingDirectory().getChildFile(juce::String::fromUTF8(path));
        const juce::File tmp = dest.getSiblingFile(dest.getFileName() + ".partial");
        tmp.deleteFile();
        bool ok = false;
        {
            juce::FileOutputStream os(tmp);
            juce::PNGImageFormat png;
            ok = os.openedOk() && png.writeImageToStream(out, os);
            if (ok)
            {
                os.flush();
                ok = os.getStatus().wasOk();
            }
        }
        if (ok && std::rename(tmp.getFullPathName().toRawUTF8(), dest.getFullPathName().toRawUTF8()) == 0)
            return true;
        tmp.deleteFile();
        return false;
    }
}

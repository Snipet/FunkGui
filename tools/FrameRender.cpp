// funkgui_framerender: rasterises a canvas dump (the snapshot's SdfCanvas::dumpNextFrameTo format, v1) into a PNG,
// evaluating the same signed-distance fields the fragment shader does and sampling the same runtime-baked atlas.
//
// This is how a panel's real geometry gets inspected: run the product once with <ENV_PREFIX>CANVAS_DUMP set, then
// render what it actually drew rather than a reimplementation of it.
//
//   funkgui_framerender <dump> <out.png> [supersample 1-4, default 2]
//
// It is also a layout gate. A picture needs a person to look at it; a fingerprint does not:
//
//   funkgui_framerender fingerprint <probe> <dump> --golden-root <dir> --arch arm64|x86_64 [--bless-to <dir>]
//                       [--results <dir>]
//
// hashes every primitive's geometry and atlas coordinates (positions, SDF extents, corner radii, glyph UVs) and
// deliberately NOT its colours, so the theme and colour-only fades do not move it. Two captures of the same build
// fingerprint identically; a moved label, a changed type size, a new primitive or a re-subset font does not. This is
// HardwareReverb's layout hash, kept exactly (the future `--legacy-hr` hash, 02 §3.9): it skips the "energy caps" of
// HR's Rank display (3 px bars inside y 165-295, its one piece of live geometry). G3/G4 replace the dump format (v2),
// the live-geometry rule (the Prim `live` flag) and the rasteriser (canvas/SoftRaster) and give this tool AREA prims.
// (Seeded from HardwareReverb Tools/FrameRender.cpp; ported to Harness v2.)

#include <funkgui/test/Harness.h>
#include <funkgui/text/BundledFont.h>
#include <funkgui/text/FontAtlasSdf.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace T = funkgui::test;
using funkgui::FontAtlasSdf;

namespace
{
    struct Prim
    {
        float x0, y0, x1, y1;
        uint32_t c0, c1;
        float d0[4], e0[2], d1[4], d2[4];
    };

    struct Rgba { float r, g, b, a; };

    Rgba unpack(uint32_t c)
    {
        return { static_cast<float>( c        & 0xff) / 255.0f,
                 static_cast<float>((c >>  8) & 0xff) / 255.0f,
                 static_cast<float>((c >> 16) & 0xff) / 255.0f,
                 static_cast<float>((c >> 24) & 0xff) / 255.0f };
    }

    float smoothstep(float e0, float e1, float x)
    {
        const float t = juce::jlimit(0.0f, 1.0f, (x - e0) / juce::jmax(e1 - e0, 1.0e-9f));
        return t * t * (3.0f - 2.0f * t);
    }
}

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const std::vector<std::string> args = T::positionals(argc, argv);
    const bool fingerprint = !args.empty() && args[0] == "fingerprint";
    if (fingerprint ? args.size() != 3 : (args.size() < 2 || args.size() > 3))
    {
        std::printf("usage: funkgui_framerender <dump> <out.png> [scale]\n"
                    "       funkgui_framerender fingerprint <probe> <dump> --golden-root <dir> --arch arm64|x86_64 "
                    "[--bless-to <dir>] [--results <dir>]\n");
        return 2;
    }
    const juce::File in = juce::File::getCurrentWorkingDirectory().getChildFile(
        juce::String::fromUTF8(args[fingerprint ? 2 : 0].c_str()));
    if (!fingerprint && !in.existsAsFile())
    {
        std::printf("no dump at %s\n", in.getFullPathName().toRawUTF8());
        return 1;
    }

    float viewW = 880.0f, viewH = 520.0f, dpi = 2.0f;
    unsigned clearRgb = 0x16171A;
    std::vector<Prim> prims;

    for (const auto& lineStr : juce::StringArray::fromLines(in.loadFileAsString()))
    {
        const auto l = lineStr.trim();
        if (l.startsWith("clear"))
        {
            std::sscanf(l.toRawUTF8(), "clear %x", &clearRgb);
            continue;
        }
        if (l.startsWith("view"))
        {
            std::sscanf(l.toRawUTF8(), "view %f %f dpi %f", &viewW, &viewH, &dpi);
            continue;
        }
        if (!l.startsWith("p ")) continue;
        Prim p{};
        const int n = std::sscanf(l.toRawUTF8(),
            "p %f %f %f %f c0 %x c1 %x d0 %f %f %f %f e0 %f %f "
            "d1 %f %f %f %f d2 %f %f %f %f",
            &p.x0, &p.y0, &p.x1, &p.y1, &p.c0, &p.c1,
            &p.d0[0], &p.d0[1], &p.d0[2], &p.d0[3],
            &p.e0[0], &p.e0[1],
            &p.d1[0], &p.d1[1], &p.d1[2], &p.d1[3],
            &p.d2[0], &p.d2[1], &p.d2[2], &p.d2[3]);
        if (n == 20) prims.push_back(p);
    }
    std::printf("view %.0fx%.0f dpi %.1f, %d primitives\n",
                (double) viewW, (double) viewH, (double) dpi, (int) prims.size());

    if (fingerprint)
    {
        T::Probe P(args[1], "", argc, argv);
        P.eq("dump.readable", in.existsAsFile(), 1);
        if (!P.ge("dump.primitives", static_cast<double>(prims.size()), 1))
            return P.finish();
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](float v) { uint32_t b; std::memcpy(&b, &v, 4);
                                  for (int k = 0; k < 4; ++k) { h ^= (b >> (k * 8)) & 0xff; h *= 1099511628211ull; } };
        int statics = 0, text = 0, rank = 0, caps = 0;
        float maxX = 0.0f, maxY = 0.0f;
        int segments = 0;
        for (const auto& p : prims)
        {
            const bool isText = p.d2[2] > 0.5f && p.d2[2] < 1.5f;
            const bool isSeg  = p.d2[2] > 1.5f;
            if (isSeg) ++segments;
            const bool inRank = p.y0 >= 165.0f && p.y1 <= 295.0f && !isText && !isSeg;
            // A cap is a 3 px rectangle riding a Rank stroke: SDF half-height
            // 1.5 where every stroke's is at least 8. Identified by the field,
            // not the quad, which is padded for antialiasing.
            const bool cap = inRank && p.d0[3] <= 2.0f;
            if (cap) { ++caps; continue; }
            if (inRank) ++rank;
            if (isText) ++text;
            ++statics;
            mix(p.x0); mix(p.y0); mix(p.x1); mix(p.y1);
            for (float v : p.d0) mix(v);
            for (float v : p.e0) mix(v);
            for (float v : p.d1) mix(v);
            for (float v : p.d2) mix(v);
            maxX = juce::jmax(maxX, p.x1); maxY = juce::jmax(maxY, p.y1);
        }
        P.hash("layout.geometry", h);
        P.num("layout.static_count", statics,  T::Tol::exact());
        P.num("layout.text_count",   text,     T::Tol::exact());
        P.num("layout.rank_strokes", rank,     T::Tol::exact());
        P.num("layout.segments",     segments, T::Tol::exact());
        P.num("layout.view_w", viewW, T::Tol::exact());
        P.num("layout.view_h", viewH, T::Tol::exact());
        P.num("layout.max_x",  maxX,  T::Tol::abs(0.01));
        P.num("layout.max_y",  maxY,  T::Tol::abs(0.01));
        std::printf("geometry %016llx  static %d (text %d, rank strokes %d)  live caps excluded %d  extent %.1f x %.1f\n",
                    (unsigned long long) h, statics, text, rank, caps, (double) maxX, (double) maxY);
        return P.finish();
    }
    if (prims.empty()) return 1;

    const juce::File out =
        juce::File::getCurrentWorkingDirectory().getChildFile(juce::String::fromUTF8(args[1].c_str()));
    const int ss = args.size() > 2 ? juce::jlimit(1, 4, std::atoi(args[2].c_str())) : 2;

    FontAtlasSdf atlas;
    if (!atlas.bake(funkgui::BundledFont::data(), funkgui::BundledFont::size())) { std::printf("atlas bake failed\n"); return 1; }

    const int W = static_cast<int>(viewW) * ss;
    const int H = static_cast<int>(viewH) * ss;
    std::vector<Rgba> fb(static_cast<size_t>(W) * H, Rgba{ 0, 0, 0, 1 });

    for (auto& px : fb)
        px = { static_cast<float>((clearRgb >> 16) & 0xff) / 255.0f,
               static_cast<float>((clearRgb >>  8) & 0xff) / 255.0f,
               static_cast<float>( clearRgb        & 0xff) / 255.0f, 1.0f };

    const auto& field = atlas.pixels();
    auto sampleAtlas = [&](float u, float v)
    {
        const float fx = juce::jlimit(0.0f, (float) FontAtlasSdf::kAtlasW - 1.0f,
                                      u * FontAtlasSdf::kAtlasW - 0.5f);
        const float fy = juce::jlimit(0.0f, (float) FontAtlasSdf::kAtlasH - 1.0f,
                                      v * FontAtlasSdf::kAtlasH - 0.5f);
        const int x0 = (int) fx, y0 = (int) fy;
        const int x1 = juce::jmin(x0 + 1, FontAtlasSdf::kAtlasW - 1);
        const int y1 = juce::jmin(y0 + 1, FontAtlasSdf::kAtlasH - 1);
        const float tx = fx - (float) x0, ty = fy - (float) y0;
        auto at = [&](int x, int y)
        { return field[(size_t) y * FontAtlasSdf::kAtlasW + (size_t) x] / 255.0f; };
        return juce::jmap(ty, juce::jmap(tx, at(x0, y0), at(x1, y0)),
                              juce::jmap(tx, at(x0, y1), at(x1, y1)));
    };

    // One physical pixel in logical px — the shader's u_viewSize.z. The render
    // is supersampled, so a sample here is 1/ss of a logical pixel.
    const float aaGeom = 1.0f / dpi;

    for (const auto& p : prims)
    {
        const int px0 = juce::jmax(0, (int) std::floor(p.x0 * ss));
        const int py0 = juce::jmax(0, (int) std::floor(p.y0 * ss));
        const int px1 = juce::jmin(W, (int) std::ceil (p.x1 * ss));
        const int py1 = juce::jmin(H, (int) std::ceil (p.y1 * ss));
        const bool isText = p.d2[2] > 0.5f && p.d2[2] < 1.5f;
        const bool isSeg  = p.d2[2] > 1.5f;

        const auto col0 = unpack(p.c0);
        const auto col1 = unpack(p.c1);

        for (int y = py0; y < py1; ++y)
        {
            for (int x = px0; x < px1; ++x)
            {
                // Fractional position within the primitive's quad.
                const float fx = ((float) x + 0.5f) / ss;
                const float fy = ((float) y + 0.5f) / ss;
                const float tx = (fx - p.x0) / juce::jmax(p.x1 - p.x0, 1e-6f);
                const float ty = (fy - p.y0) / juce::jmax(p.y1 - p.y0, 1e-6f);
                if (tx < 0.0f || tx > 1.0f || ty < 0.0f || ty > 1.0f) continue;

                Rgba src; float alpha;

                if (isSeg)
                {
                    // The capsule, exactly as fs_ui.sc evaluates it.
                    const float hw = p.d0[2], hh = p.d0[3];
                    const float padX = (p.x1 - p.x0) * 0.5f - hw;
                    const float padY = (p.y1 - p.y0) * 0.5f - hh;
                    const float lx = juce::jmap(tx, -hw - padX, hw + padX);
                    const float ly = juce::jmap(ty, -hh - padY, hh + padY);
                    const float ax = p.d1[0], ay = p.d1[1];
                    const float bax = p.d1[2] - ax, bay = p.d1[3] - ay;
                    const float den = juce::jmax(bax * bax + bay * bay, 1e-6f);
                    const float t = juce::jlimit(0.0f, 1.0f, ((lx - ax) * bax + (ly - ay) * bay) / den);
                    const float dx = lx - ax - bax * t, dy = ly - ay - bay * t;
                    const float d = std::sqrt(dx * dx + dy * dy) - p.d2[0];
                    const float fw = juce::jmax(aaGeom, p.d2[1]);
                    alpha = (1.0f - smoothstep(-fw, fw, d)) * col0.a;
                    src = { col0.r, col0.g, col0.b, alpha };
                }
                else if (!isText)
                {
                    // Local coords interpolate from -(half+pad) to +(half+pad).
                    const float hw = p.d0[2], hh = p.d0[3];
                    const float padX = (p.x1 - p.x0) * 0.5f - hw;
                    const float padY = (p.y1 - p.y0) * 0.5f - hh;
                    const float lx = juce::jmap(tx, -hw - padX, hw + padX);
                    const float ly = juce::jmap(ty, -hh - padY, hh + padY);

                    const float rTop = (lx > 0.0f) ? p.d1[1] : p.d1[0];
                    const float rBot = (lx > 0.0f) ? p.d1[2] : p.d1[3];
                    float r = (ly > 0.0f) ? rBot : rTop;
                    r = juce::jmin(r, juce::jmin(hw, hh));

                    const float qx = std::abs(lx) - hw + r;
                    const float qy = std::abs(ly) - hh + r;
                    const float d = std::sqrt(std::pow(juce::jmax(qx, 0.0f), 2.0f)
                                            + std::pow(juce::jmax(qy, 0.0f), 2.0f))
                                  + juce::jmin(juce::jmax(qx, qy), 0.0f) - r;

                    const float bw = p.d2[0], soft = p.d2[1];
                    const float fwd = juce::jmax(aaGeom, soft);
                    const float fa = (1.0f - smoothstep(-fwd, fwd, d)) * col0.a;
                    src = { col0.r, col0.g, col0.b, fa };
                    alpha = fa;

                    if (bw > 0.0f)
                    {
                        const float bd = std::abs(d + bw * 0.5f) - bw * 0.5f;
                        const float ba = (1.0f - smoothstep(-aaGeom, aaGeom, bd)) * col1.a;
                        const float oA = ba + fa * (1.0f - ba);
                        if (oA > 1e-5f)
                        {
                            src = { (col1.r * ba + col0.r * fa * (1.0f - ba)) / oA,
                                    (col1.g * ba + col0.g * fa * (1.0f - ba)) / oA,
                                    (col1.b * ba + col0.b * fa * (1.0f - ba)) / oA, oA };
                            alpha = oA;
                        }
                    }
                }
                else
                {
                    // uv interpolates from vertex 0's to vertex 2's.
                    const float uu = juce::jmap(tx, p.d0[0], p.e0[0]);
                    const float vv = juce::jmap(ty, p.d0[1], p.e0[1]);
                    const float dv = sampleAtlas(uu, vv);
                    const float weight = p.d1[0], taa = p.d1[1], gamma = p.d1[2];
                    float a = smoothstep(0.5f - weight - taa, 0.5f - weight + taa, dv);
                    a = std::pow(a, gamma);
                    alpha = a * col0.a;
                    src = { col0.r, col0.g, col0.b, alpha };
                }

                if (alpha <= 0.0005f) continue;
                auto& dst = fb[(size_t) y * W + (size_t) x];
                dst.r = src.r * alpha + dst.r * (1.0f - alpha);
                dst.g = src.g * alpha + dst.g * (1.0f - alpha);
                dst.b = src.b * alpha + dst.b * (1.0f - alpha);
            }
        }
    }

    juce::Image img(juce::Image::RGB, W, H, true);
    {
        juce::Image::BitmapData bd(img, juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
            {
                const auto& c = fb[(size_t) y * W + (size_t) x];
                bd.setPixelColour(x, y, juce::Colour::fromFloatRGBA(
                    juce::jlimit(0.0f, 1.0f, c.r), juce::jlimit(0.0f, 1.0f, c.g),
                    juce::jlimit(0.0f, 1.0f, c.b), 1.0f));
            }
    }
    out.deleteFile();
    juce::FileOutputStream os(out);
    juce::PNGImageFormat png;
    png.writeImageToStream(img, os);
    std::printf("wrote %s (%dx%d)\n", out.getFullPathName().toRawUTF8(), W, H);
    return 0;
}

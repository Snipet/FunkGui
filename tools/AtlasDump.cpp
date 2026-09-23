// Offscreen harness: bakes the SDF font atlas exactly as the editor does,
// times it, and writes two PNGs — the raw distance field, and a rendering of
// a sample string through the same edge function the fragment shader uses.
//
// The editor has no headless mode, so without this there is no way to inspect
// glyph quality or bake cost except by eye on a running plugin.
//
// Extra arguments are typeface names; each is baked and rendered so the same
// strings can be compared at the sizes the panel actually draws them. This is
// how a bundled face gets chosen: the 48 px bake is downsampled to 10-11 px
// for labels, and that downsample is what separates faces that survive from
// faces that go to mush.
//
//   cmake --build build --target HardwareReverbAtlasDump
//   ./build/HardwareReverbAtlasDump /tmp/out "IBM Plex Sans" "Open Sans"

#include "gui/FontAtlasSdf.h"

#include "gui/BundledFont.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using hrvbgui::FontAtlasSdf;

namespace
{
    void writePng(const juce::File& f, const juce::Image& img)
    {
        f.deleteFile();
        juce::FileOutputStream out(f);
        juce::PNGImageFormat png;
        png.writeImageToStream(img, out);
    }

    // Mirrors fs_text.sc: the atlas stores distance with the edge at 0.5, and
    // the edge is resolved with a screen-space-derivative-width smoothstep.
    float coverage(float d, float aa, float weight)
    {
        const float lo = 0.5f - weight - aa;
        const float hi = 0.5f - weight + aa;
        const float t  = juce::jlimit(0.0f, 1.0f, (d - lo) / juce::jmax(hi - lo, 1.0e-6f));
        return t * t * (3.0f - 2.0f * t);
    }
}

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    const juce::File outDir(argc > 1 ? juce::String(argv[1])
                                     : juce::String("/tmp/hrvb-atlas"));
    outDir.createDirectory();

    juce::StringArray faces;
    for (int i = 2; i < argc; ++i) faces.add(juce::String(argv[i]));
    if (faces.isEmpty()) faces.add(juce::String());

    // The strings the panel actually draws, at the sizes it draws them.
    struct Line { const char* text; float px; float weight; };
    static const Line lines[] = {
        { "DECAY  SIZE  DAMPING  MIX  PRE-DELAY  X-OVER", 11.0f, 0.03f },
        { "4.04 S   94 MS   0.30 HZ   35 %   1.00 X", 24.0f, 0.0f },
        { "PER PASS -1.40 DB   HF CORNER 3.3 KHZ", 10.0f, 0.03f },
        { "1.00  11.5  4.04", 44.0f, 0.03f },
    };

    const int W = 1000, rowH = 150;
    const int H = rowH * faces.size() + 8;
    juce::Image img(juce::Image::RGB, W, H, true);
    juce::Image::BitmapData bd(img, juce::Image::BitmapData::writeOnly);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            bd.setPixelColour(x, y, juce::Colour::fromRGB(0x16, 0x17, 0x1a));

    for (int fi = 0; fi < faces.size(); ++fi)
    {
        FontAtlasSdf atlas;
        const auto t0 = std::chrono::steady_clock::now();
        juce::MemoryBlock mb;
        bool fromFile = false;
        if (faces[fi].contains("/"))
        {
            juce::File f{ faces[fi] };
            fromFile = f.existsAsFile() && f.loadFileAsData(mb);
        }
        // An empty name means "what the plugin actually ships", i.e. the
        // bundled face — not the system default.
        const bool ok = fromFile
            ? atlas.bake(mb.getData(), mb.getSize())
            : (faces[fi].isEmpty()
                 ? atlas.bake(hrvbgui::BundledFont::data(), hrvbgui::BundledFont::size())
                 : atlas.bake(nullptr, 0, faces[fi].toRawUTF8()));
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0).count();

        const juce::String label = faces[fi].isEmpty()
            ? juce::String("(bundled: ") + hrvbgui::BundledFont::name() + ")"
            : (faces[fi].contains("/") ? juce::File{ faces[fi] }.getFileNameWithoutExtension()
                                       : faces[fi]);
        std::printf("%-24s %s  %5.1f ms  cap %.1f  x %.1f  x/cap %.2f\n",
                    label.toRawUTF8(), ok ? "ok " : "BAD", ms,
                    atlas.capHeight(), atlas.xHeight(),
                    atlas.capHeight() > 0 ? atlas.xHeight() / atlas.capHeight() : 0.0f);
        if (!ok) continue;

        const auto& field = atlas.pixels();
        auto sample = [&](float u, float v)
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

        auto draw = [&](const char* text, float px, float weight,
                        float x0, float yTop, juce::Colour col)
        {
            const float scale = px / FontAtlasSdf::kBasePx;
            const float aa = 1.0f / (2.0f * FontAtlasSdf::kSpread * scale);
            const float base = yTop + atlas.ascent() * scale;
            float pen = x0;
            for (const char* p = text; *p != 0; )
            {
                uint32_t cp = (unsigned char) *p++;
                if (cp >= 0x80)
                {
                    int extra = (cp & 0xE0) == 0xC0 ? 1 : (cp & 0xF0) == 0xE0 ? 2 : 3;
                    cp &= (extra == 1 ? 0x1Fu : extra == 2 ? 0x0Fu : 0x07u);
                    for (int i = 0; i < extra && *p; ++i)
                        cp = (cp << 6) | ((unsigned char) *p++ & 0x3Fu);
                }
                if (cp == ' ') { pen += atlas.spaceAdvance() * scale; continue; }
                const auto* g = atlas.glyph(cp);
                if (g == nullptr) continue;
                const float gx = pen + g->bx * scale, gy = base + g->by * scale;
                const float gw = g->w * scale,        gh = g->h * scale;
                for (int y = (int) std::floor(gy); y < (int) std::ceil(gy + gh); ++y)
                {
                    if (y < 0 || y >= H) continue;
                    for (int x = (int) std::floor(gx); x < (int) std::ceil(gx + gw); ++x)
                    {
                        if (x < 0 || x >= W) continue;
                        const float u = g->u0 + (g->u1 - g->u0) * (((float) x + 0.5f - gx) / gw);
                        const float v = g->v0 + (g->v1 - g->v0) * (((float) y + 0.5f - gy) / gh);
                        const float a = coverage(sample(u, v), aa, weight);
                        if (a <= 0.001f) continue;
                        bd.setPixelColour(x, y, bd.getPixelColour(x, y)
                                                  .overlaidWith(col.withAlpha(a)));
                    }
                }
                pen += g->advance * scale;
            }
        };

        const float top = (float) (fi * rowH) + 10.0f;
        draw(label.toRawUTF8(), 10.0f, 0.03f, 14.0f, top,
             juce::Colour::fromRGB(0xFF, 0x5A, 0x1F));
        draw(lines[0].text, lines[0].px, lines[0].weight, 14.0f, top + 20.0f,
             juce::Colour::fromRGB(0x8A, 0x8E, 0x95));
        draw(lines[1].text, lines[1].px, lines[1].weight, 14.0f, top + 40.0f,
             juce::Colour::fromRGB(0xEC, 0xEA, 0xE4));
        draw(lines[2].text, lines[2].px, lines[2].weight, 14.0f, top + 76.0f,
             juce::Colour::fromRGB(0x57, 0x5C, 0x63));
        draw(lines[3].text, lines[3].px, lines[3].weight, 470.0f, top + 44.0f,
             juce::Colour::fromRGB(0xEC, 0xEA, 0xE4));

        for (int x = 8; x < W - 8; ++x)
            bd.setPixelColour(x, fi * rowH + rowH - 2,
                              juce::Colour::fromRGB(0x33, 0x37, 0x3C));
    }

    writePng(outDir.getChildFile("faces.png"), img);
    std::printf("wrote %s/faces.png\n", outDir.getFullPathName().toRawUTF8());
    return 0;
}

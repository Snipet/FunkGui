#include <funkgui/canvas/Expand.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

// The CPU half of BgfxSink (02 §3.2, SPRINTS §7 D9): a Prim back into HR's six non-indexed vertices a, b, c, a, c, d
// (the snapshot's SdfCanvas::quad, gpu/SdfCanvas.cpp:85-89). No bgfx here, so fg.canvas.expansion runs headless;
// BgfxSink::submit (G7) copies exactly this stream into the transient buffer, whose layout is BgfxContext's (A §2.2).

namespace funkgui
{
    static_assert(offsetof(Vtx, x) == 0 && offsetof(Vtx, c0) == 8 && offsetof(Vtx, c1) == 12
                      && offsetof(Vtx, d0) == 16 && offsetof(Vtx, d1) == 32 && offsetof(Vtx, d2) == 48,
                  "Vtx offsets are BgfxContext's vertex layout (A §2.2)");

    void expand(const Prim& p, Vtx* out) noexcept
    {
        // Every vertex carries the Prim's colours, SDF extents and per-kind data; only the position and the local
        // coordinates (d0.xy) differ per corner. Written field by field so no padding or tag byte reaches the stream.
        Vtx v;
        v.x = 0.0f;
        v.y = 0.0f;
        v.c0 = p.c0;
        v.c1 = p.c1;
        v.d0[0] = 0.0f;
        v.d0[1] = 0.0f;
        v.d0[2] = p.d0[2];
        v.d0[3] = p.d0[3];
        for (int i = 0; i < 4; ++i)
        {
            v.d1[i] = p.d1[i];
            v.d2[i] = p.d2[i];
        }

        const auto at = [&v](float px, float py, float lx, float ly) noexcept
        {
            Vtx r = v;
            r.x = px;
            r.y = py;
            r.d0[0] = lx;
            r.d0[1] = ly;
            return r;
        };
        const Vtx a = at(p.x0, p.y0, p.d0[0], p.d0[1]);
        const Vtx b = at(p.x1, p.y0, p.e0[0], p.d0[1]);
        const Vtx c = at(p.x1, p.y1, p.e0[0], p.e0[1]);
        const Vtx d = at(p.x0, p.y1, p.d0[0], p.e0[1]);
        out[0] = a;
        out[1] = b;
        out[2] = c;
        out[3] = a;
        out[4] = c;
        out[5] = d;
    }

    void expand(const PrimList& list, std::vector<Vtx>& out)
    {
        out.resize(list.prims.size() * static_cast<size_t>(kVerticesPerPrim));   // shrinking keeps the capacity
        Vtx* dst = out.data();
        for (const Prim& p : list.prims)
        {
            expand(p, dst);
            dst += kVerticesPerPrim;
        }
    }
}

// ---- v0.19.0: shader draws (canvas/Expand.h) ------------------------------------------------------------------------

namespace funkgui
{
    void expandShader(const ShaderDraw& s, Vtx* out) noexcept
    {
        const float x0 = s.rect.x, y0 = s.rect.y, x1 = s.rect.x + s.rect.w, y1 = s.rect.y + s.rect.h;
        const auto at = [&s](float px, float py, float u, float v) noexcept
        {
            Vtx r{};
            r.x = px;
            r.y = py;
            r.d0[0] = u;
            r.d0[1] = v;
            r.d0[2] = u * s.rect.w;                  // exactly 0 or w (and h): u and v are 0 or 1
            r.d0[3] = v * s.rect.h;
            return r;
        };
        const Vtx a = at(x0, y0, 0.0f, 0.0f);
        const Vtx b = at(x1, y0, 1.0f, 0.0f);
        const Vtx c = at(x1, y1, 1.0f, 1.0f);
        const Vtx d = at(x0, y1, 0.0f, 1.0f);
        out[0] = a;
        out[1] = b;
        out[2] = c;
        out[3] = a;
        out[4] = c;
        out[5] = d;
    }

    void drawSteps(const PrimList& list, std::vector<DrawStep>& out)
    {
        out.clear();
        const auto n = static_cast<uint32_t>(list.prims.size());
        uint32_t cursor = 0;                         // the first primitive not yet in a step
        for (std::size_t i = 0; i < list.shaders.size(); ++i)
        {
            const uint32_t at = std::min(list.shaders[i].at, n);
            if (at > cursor)
            {
                out.push_back({ DrawStep::Kind::prims, cursor, at - cursor });
                cursor = at;
            }
            out.push_back({ DrawStep::Kind::shader, static_cast<uint32_t>(i), 1u });
        }
        if (cursor < n)
            out.push_back({ DrawStep::Kind::prims, cursor, n - cursor });
    }

    PixelRect shaderPixels(const ShaderDraw& s, const FrameInfo& info, int physW, int physH) noexcept
    {
        PixelRect px;
        if (info.logicalW <= 0 || info.logicalH <= 0 || physW <= 0 || physH <= 0)
            return px;
        float x0 = s.rect.x, y0 = s.rect.y, x1 = s.rect.x + s.rect.w, y1 = s.rect.y + s.rect.h;
        if (s.clipped)
        {
            x0 = std::max(x0, s.clip.x);
            y0 = std::max(y0, s.clip.y);
            x1 = std::min(x1, s.clip.x + s.clip.w);
            y1 = std::min(y1, s.clip.y + s.clip.h);
        }
        const float sx = static_cast<float>(physW) / static_cast<float>(info.logicalW);
        const float sy = static_cast<float>(physH) / static_cast<float>(info.logicalH);
        // The first pixel whose centre (i + 0.5) / scale is at or after the edge, clamped to 0..limit (NaN -> 0).
        const auto first = [](float edge, float scale, int limit) noexcept
        {
            const float f = std::ceil(edge * scale - 0.5f);
            return f > 0.0f ? (f < static_cast<float>(limit) ? static_cast<int>(f) : limit) : 0;
        };
        const int px0 = first(x0, sx, physW), px1 = first(x1, sx, physW);
        const int py0 = first(y0, sy, physH), py1 = first(y1, sy, physH);
        if (px1 <= px0 || py1 <= py0)
            return px;
        px = { px0, py0, px1 - px0, py1 - py0 };
        return px;
    }
}

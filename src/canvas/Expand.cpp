#include <funkgui/canvas/Expand.h>

#include <funkgui/canvas/Images.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

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
        std::size_t s = 0, m = 0;                    // the next shader draw, the next image draw
        while (s < list.shaders.size() || m < list.images.size())
        {
            // v0.20.0: recording order between the two kinds (an image draw follows shaders[0 .. shaderAt)).
            const bool image = m < list.images.size()
                               && (s >= list.shaders.size() || list.images[m].shaderAt <= s);
            const uint32_t at = std::min(image ? list.images[m].at : list.shaders[s].at, n);
            if (at > cursor)
            {
                out.push_back({ DrawStep::Kind::prims, cursor, at - cursor });
                cursor = at;
            }
            if (image)
                out.push_back({ DrawStep::Kind::image, static_cast<uint32_t>(m++), 1u });
            else
                out.push_back({ DrawStep::Kind::shader, static_cast<uint32_t>(s++), 1u });
        }
        if (cursor < n)
            out.push_back({ DrawStep::Kind::prims, cursor, n - cursor });
    }

    namespace
    {
        // The pixels whose centres lie in the half-open [x0, x1) x [y0, y1) (logical px) of a physW x physH frame.
        PixelRect pixelsOf(float x0, float y0, float x1, float y1, const FrameInfo& info, int physW, int physH) noexcept
        {
            PixelRect px;
            if (info.logicalW <= 0 || info.logicalH <= 0 || physW <= 0 || physH <= 0)
                return px;
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

    PixelRect shaderPixels(const ShaderDraw& s, const FrameInfo& info, int physW, int physH) noexcept
    {
        float x0 = s.rect.x, y0 = s.rect.y, x1 = s.rect.x + s.rect.w, y1 = s.rect.y + s.rect.h;
        if (s.clipped)
        {
            x0 = std::max(x0, s.clip.x);
            y0 = std::max(y0, s.clip.y);
            x1 = std::min(x1, s.clip.x + s.clip.w);
            y1 = std::min(y1, s.clip.y + s.clip.h);
        }
        return pixelsOf(x0, y0, x1, y1, info, physW, physH);
    }
}

// ---- v0.20.0: image draws (canvas/Expand.h) -------------------------------------------------------------------------

namespace funkgui
{
    void expandImage(const ImageDraw& m, Vtx* out) noexcept
    {
        constexpr float e = kImageApron;
        const float w = m.dst.w, h = m.dst.h;
        const auto at = [&m, w, h](float px, float py) noexcept
        {
            Vtx r{};
            r.x = m.dst.x + px;
            r.y = m.dst.y + py;
            r.d0[0] = px / w;
            r.d0[1] = py / h;
            r.d0[2] = px;
            r.d0[3] = py;
            return r;
        };
        const Vtx a = at(-e, -e);
        const Vtx b = at(w + e, -e);
        const Vtx c = at(w + e, h + e);
        const Vtx d = at(-e, h + e);
        out[0] = a;
        out[1] = b;
        out[2] = c;
        out[3] = a;
        out[4] = c;
        out[5] = d;
    }

    PixelRect imagePixels(const ImageDraw& m, const FrameInfo& info, int physW, int physH) noexcept
    {
        float x0 = m.dst.x - kImageApron, y0 = m.dst.y - kImageApron;
        float x1 = m.dst.x + m.dst.w + kImageApron, y1 = m.dst.y + m.dst.h + kImageApron;
        if (m.clipped)
        {
            x0 = std::max(x0, m.clip.x);
            y0 = std::max(y0, m.clip.y);
            x1 = std::min(x1, m.clip.x + m.clip.w);
            y1 = std::min(y1, m.clip.y + m.clip.h);
        }
        return pixelsOf(x0, y0, x1, y1, info, physW, physH);
    }

    int imageLevel(const ImageDraw& m, float dpi, int picW, int picH) noexcept
    {
        const int levels = images::levelCount(picW, picH);
        if (levels <= 1)
            return 0;
        // Texels of the picture per device px along each axis; the larger one decides.
        const float dw = m.dst.w * dpi, dh = m.dst.h * dpi;
        const float rx = m.uv.w * static_cast<float>(picW) / dw;
        const float ry = m.uv.h * static_cast<float>(picH) / dh;
        const float ratio = std::max(rx, ry);
        if (!std::isfinite(ratio) || !(ratio >= 2.0f))
            return 0;
        int level = 0;
        float step = 2.0f;                           // 2^(level + 1)
        while (level + 1 < levels && ratio >= step)
        {
            ++level;
            step *= 2.0f;
        }
        return level;
    }

    ImageSampling imageSampling(const ImageDraw& m, int level, int picW, int picH) noexcept
    {
        ImageSampling s;
        s.u0 = m.uv.x;
        s.v0 = m.uv.y;
        s.du = m.uv.w;
        s.dv = m.uv.h;
        const int lw = std::max(1, picW >> std::clamp(level, 0, 30));
        const int lh = std::max(1, picH >> std::clamp(level, 0, 30));
        // Half a texel of the level inside each edge of uv; the middle when uv is narrower than one texel.
        const auto range = [](float start, float size, int texels, float& lo, float& hi) noexcept
        {
            const float half = 0.5f / static_cast<float>(texels);
            lo = start + half;
            hi = start + size - half;
            if (!(hi >= lo))
                lo = hi = start + size * 0.5f;
        };
        range(m.uv.x, m.uv.w, lw, s.loU, s.hiU);
        range(m.uv.y, m.uv.h, lh, s.loV, s.hiV);
        return s;
    }
}

// ---- v0.26.0: primitives as instances (canvas/Expand.h) --------------------------------------------------------------
// The record BgfxSink uploads per primitive and, in instanceCorner(), shaders/vs_ui_inst.sc's own steps on the CPU. The
// three packed fields are integers below 2^24, so every float below is exact: a product by a power of two, a floor, and
// a difference of two such integers. The shader takes the same steps, so whatever a compiler reorders (fast math
// included) the bytes it unpacks are the ones packed here.

namespace funkgui
{
    static_assert(offsetof(Inst, rect) == 0 && offsetof(Inst, d0) == 16 && offsetof(Inst, e0d2) == 32
                      && offsetof(Inst, d1) == 48 && offsetof(Inst, packed) == 64,
                  "Inst is i_data0..i_data4 in order (shaders/varying_inst.def.sc)");

    namespace
    {
        // c / 255 for every byte, rounded to the nearest float (a constant expression rounds as the run time does).
        constexpr std::array<float, 256> makeUnorm8() noexcept
        {
            std::array<float, 256> t{};
            for (int c = 0; c < 256; ++c)
                t[static_cast<std::size_t>(c)] = static_cast<float>(c) / 255.0f;
            return t;
        }
        constexpr std::array<float, 256> kUnorm8 = makeUnorm8();

        // The kind as an integer 0..255 (0 for anything else: the record of a primitive that is not instanceable).
        uint32_t kindOf(const Prim& p) noexcept
        {
            const float k = p.d2[2];
            return k >= 0.0f && k <= 255.0f ? static_cast<uint32_t>(k) : 0u;
        }

        // vs_ui_inst.sc's bytes3(): the three bytes of a packed field, low first.
        struct Bytes3
        {
            float lo, mid, hi;
        };
        Bytes3 bytes3(float p) noexcept
        {
            const float hi = std::floor(p * (1.0f / 65536.0f));
            const float lo = p - hi * 65536.0f;
            const float mid = std::floor(lo * (1.0f / 256.0f));
            return { lo - mid * 256.0f, mid, hi };
        }

        uint32_t byteOf(float b) noexcept { return static_cast<uint32_t>(b) & 0xffu; }

        // The corners of expand()'s order a, b, c, a, c, d on the unit quad (BgfxSink's quad vertex buffer).
        constexpr float kCornerX[kVerticesPerPrim] = { 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 0.0f };
        constexpr float kCornerY[kVerticesPerPrim] = { 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f };
    }

    bool instanceable(const Prim& p) noexcept
    {
        const float k = p.d2[2];
        if (!(k >= 0.0f && k <= 255.0f))
            return false;
        return std::bit_cast<uint32_t>(static_cast<float>(static_cast<uint32_t>(k))) == std::bit_cast<uint32_t>(k);
    }

    Inst instance(const Prim& p) noexcept
    {
        Inst r;
        r.rect[0] = p.x0;
        r.rect[1] = p.y0;
        r.rect[2] = p.x1;
        r.rect[3] = p.y1;
        for (int i = 0; i < 4; ++i)
        {
            r.d0[i] = p.d0[i];
            r.d1[i] = p.d1[i];
        }
        r.e0d2[0] = p.e0[0];
        r.e0d2[1] = p.e0[1];
        r.e0d2[2] = p.d2[0];
        r.e0d2[3] = p.d2[1];
        r.packed[0] = static_cast<float>(p.c0 & 0xffffffu);
        r.packed[1] = static_cast<float>((p.c0 >> 24) | ((p.c1 & 0xffffu) << 8));
        r.packed[2] = static_cast<float>(((p.c1 >> 16) & 0xffffu) | (kindOf(p) << 16));
        r.packed[3] = p.d2[3];
        return r;
    }

    bool instances(const PrimList& list, std::vector<Inst>& out)
    {
        out.resize(list.prims.size());                   // shrinking keeps the capacity
        Inst* dst = out.data();
        for (const Prim& p : list.prims)
        {
            if (!instanceable(p))
                return false;
            *dst++ = instance(p);
        }
        return true;
    }

    Vtx instanceCorner(const Inst& in, int k) noexcept
    {
        const int at = std::clamp(k, 0, kVerticesPerPrim - 1);
        const bool right = kCornerX[at] > 0.5f;
        const bool bottom = kCornerY[at] > 0.5f;
        const Bytes3 a = bytes3(in.packed[0]);       // c0's r, g, b
        const Bytes3 b = bytes3(in.packed[1]);       // c0's a, c1's r, g
        const Bytes3 c = bytes3(in.packed[2]);       // c1's b, a, the kind
        Vtx v;
        v.x = right ? in.rect[2] : in.rect[0];
        v.y = bottom ? in.rect[3] : in.rect[1];
        v.c0 = byteOf(a.lo) | (byteOf(a.mid) << 8) | (byteOf(a.hi) << 16) | (byteOf(b.lo) << 24);
        v.c1 = byteOf(b.mid) | (byteOf(b.hi) << 8) | (byteOf(c.lo) << 16) | (byteOf(c.mid) << 24);
        v.d0[0] = right ? in.e0d2[0] : in.d0[0];
        v.d0[1] = bottom ? in.e0d2[1] : in.d0[1];
        v.d0[2] = in.d0[2];
        v.d0[3] = in.d0[3];
        for (int i = 0; i < 4; ++i)
            v.d1[i] = in.d1[i];
        v.d2[0] = in.e0d2[2];
        v.d2[1] = in.e0d2[3];
        v.d2[2] = c.hi;
        v.d2[3] = in.packed[3];
        return v;
    }

    const float* unorm8Values() noexcept { return kUnorm8.data(); }
}

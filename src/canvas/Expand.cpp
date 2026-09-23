#include <funkgui/canvas/Expand.h>

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

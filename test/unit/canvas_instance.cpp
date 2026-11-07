// FUNKGUI_TEST name=fg.canvas.instance timeout=600 gpu=0
//
// fg.canvas.instance (v0.26.0; canvas/Expand.h "primitives as instances"): the 80-byte record BgfxSink uploads per
// primitive gives back expand()'s vertices bit for bit. For every primitive of the frames in instance_scenes.h (every
// kind and variant, every colour byte in every channel of both colours, 5,000 seeded primitives, a frame split by image
// draws) at dpi 1, 1.5 and 2, instanceCorner(instance(p), k), the instanced vertex shader's own steps on the CPU, is
// the k-th Vtx of expand(p) for k = 0..5, byte for byte. Also: the record's layout (five vec4, i_data0..i_data4, 80
// bytes against 384), unorm8Values() (c / 255 rounded to the nearest float, rising, 0 and 1 at the ends), which kinds
// are instanceable (an integer 0..255 held exactly; not -0, 0.5, 256, -1, NaN or infinity), that instances() refuses a
// list holding one that is not (BgfxSink then draws it as vertices) and keeps its capacity, that the tag never reaches
// the record and the live flag changes only the flags. Spec rows only.

#include "instance_scenes.h"

#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/Expand.h>
#include <funkgui/canvas/Prim.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Col.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/test/Harness.h>
#include <funkgui/text/FontService.h>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace T = funkgui::test;
namespace S = instance_scenes;
using funkgui::Canvas;
using funkgui::Col;
using funkgui::Inst;
using funkgui::Prim;
using funkgui::PrimList;
using funkgui::Vtx;

namespace
{
    // Every primitive of the list: its six corners from the record against expand()'s, byte for byte. Logs the first
    // difference.
    bool corners(const PrimList& l, const std::string& key, int& prims)
    {
        prims = static_cast<int>(l.prims.size());
        for (std::size_t i = 0; i < l.prims.size(); ++i)
        {
            const Prim& p = l.prims[i];
            if (!funkgui::instanceable(p))
            {
                std::printf("INFO     %s: primitive %zu is not instanceable (kind %g)\n", key.c_str(), i,
                            static_cast<double>(p.d2[2]));
                return false;
            }
            Vtx want[funkgui::kVerticesPerPrim];
            funkgui::expand(p, want);
            const Inst rec = funkgui::instance(p);
            for (int k = 0; k < funkgui::kVerticesPerPrim; ++k)
            {
                const Vtx got = funkgui::instanceCorner(rec, k);
                if (std::memcmp(&got, &want[k], sizeof(Vtx)) != 0)
                {
                    uint32_t a[16], b[16];
                    std::memcpy(a, &got, sizeof a);
                    std::memcpy(b, &want[k], sizeof b);
                    for (int w = 0; w < 16; ++w)
                        if (a[w] != b[w])
                        {
                            std::printf("INFO     %s: primitive %zu corner %d word %d: %08x, expand() %08x\n",
                                        key.c_str(), i, k, w, a[w], b[w]);
                            break;
                        }
                    return false;
                }
            }
        }
        return true;
    }

    Prim plain()
    {
        Prim p{};
        p.x0 = 1.0f;
        p.y0 = 2.0f;
        p.x1 = 30.0f;
        p.y1 = 40.0f;
        p.c0 = 0x80402010u;
        p.c1 = 0xff00ff00u;
        p.d2[2] = 0.0f;
        return p;
    }
}

int main(int argc, char** argv)
{
    const funkgui::HeadlessGuiScope gui;                 // JUCE's GUI side, when there is JUCE: the atlas bakes there
    T::Probe P("fg.canvas.instance", "", argc, argv);

    // ---- layout -----------------------------------------------------------------------------------------------------
    P.eq("layout.inst_size", static_cast<int64_t>(sizeof(Inst)), 80);
    P.eq("layout.instance_bytes", funkgui::kInstanceBytes, 80);
    P.eq("layout.offsets", offsetof(Inst, rect) == 0 && offsetof(Inst, d0) == 16 && offsetof(Inst, e0d2) == 32
                               && offsetof(Inst, d1) == 48 && offsetof(Inst, packed) == 64, 1);
    P.eq("layout.vertex_bytes_per_prim", static_cast<int64_t>(sizeof(Vtx)) * funkgui::kVerticesPerPrim, 384);

    // ---- the colour table -------------------------------------------------------------------------------------------
    {
        const float* t = funkgui::unorm8Values();
        bool nearest = true, rising = true;
        for (int c = 0; c < 256; ++c)
        {
            volatile float num = static_cast<float>(c);  // computed at run time, as the CPU divides
            const float want = num / 255.0f;
            nearest = nearest && std::bit_cast<uint32_t>(t[c]) == std::bit_cast<uint32_t>(want);
            // And the nearest float to c / 255 in double: the division above rounded once.
            const double exact = static_cast<double>(c) / 255.0;
            const float lo = std::nextafter(t[c], 0.0f), hi = std::nextafter(t[c], 2.0f);
            nearest = nearest && std::fabs(static_cast<double>(t[c]) - exact) <= std::fabs(static_cast<double>(lo) - exact)
                      && std::fabs(static_cast<double>(t[c]) - exact) <= std::fabs(static_cast<double>(hi) - exact);
            rising = rising && (c == 0 || t[c] > t[c - 1]);
        }
        P.eq("unorm8.nearest", nearest, 1);
        P.eq("unorm8.rising", rising, 1);
        P.eq("unorm8.ends", std::bit_cast<uint32_t>(t[0]) == 0u && t[255] == 1.0f, 1);
    }

    // ---- which kinds are instanceable -------------------------------------------------------------------------------
    {
        const auto with = [](float kind) { Prim p = plain(); p.d2[2] = kind; return funkgui::instanceable(p); };
        P.eq("instanceable.kinds", with(0.0f) && with(1.0f) && with(2.0f) && with(3.0f) && with(255.0f), 1);
        P.eq("instanceable.not_negative_zero", with(-0.0f), 0);
        P.eq("instanceable.not_fraction", with(0.5f), 0);
        P.eq("instanceable.not_256", with(256.0f), 0);
        P.eq("instanceable.not_negative", with(-1.0f), 0);
        P.eq("instanceable.not_nan", with(std::numeric_limits<float>::quiet_NaN()), 0);
        P.eq("instanceable.not_infinite", with(std::numeric_limits<float>::infinity()), 0);
        // Every other field may hold anything: it travels as it is.
        Prim odd = plain();
        odd.x0 = -0.0f;
        odd.d0[1] = std::numeric_limits<float>::quiet_NaN();
        odd.d1[3] = std::numeric_limits<float>::denorm_min();
        odd.d2[3] = 0.375f;
        odd.e0[0] = -std::numeric_limits<float>::infinity();
        odd.c0 = 0xffffffffu;
        odd.c1 = 0u;
        PrimList l;
        l.prims.push_back(odd);
        int n = 0;
        P.eq("record.any_other_value", corners(l, "record.any_other_value", n), 1);
    }

    // ---- instances(): refusal, size, order, capacity ----------------------------------------------------------------
    {
        PrimList l;
        for (int i = 0; i < 5; ++i)
        {
            Prim p = plain();
            p.x0 = static_cast<float>(i);
            p.d2[2] = static_cast<float>(i % 4);
            l.prims.push_back(p);
        }
        std::vector<Inst> out;
        bool order = funkgui::instances(l, out) && out.size() == 5;
        for (std::size_t i = 0; order && i < l.prims.size(); ++i)
        {
            const Inst one = funkgui::instance(l.prims[i]);
            order = std::memcmp(&one, &out[i], sizeof one) == 0;
        }
        P.eq("list.size_and_order", order, 1);
        const std::size_t cap = out.capacity();
        PrimList two;
        two.prims.assign(l.prims.begin(), l.prims.begin() + 2);
        P.eq("list.shrinks_keeping_capacity", funkgui::instances(two, out) && out.size() == 2 && out.capacity() == cap,
             1);
        PrimList none;
        P.eq("list.empty", funkgui::instances(none, out) && out.empty(), 1);
        l.prims[3].d2[2] = 1.5f;
        P.eq("list.refuses_a_kind_that_is_not_instanceable", funkgui::instances(l, out), 0);
    }

    auto& fonts = funkgui::FontService::get();
    const funkgui::FontAtlasSdf& atlas = fonts.atlas();  // baked on first use
    if (!P.eq("font.ok", fonts.ok(), 1))
        return P.finish();
    Canvas canvas(atlas);

    // ---- tag and live -----------------------------------------------------------------------------------------------
    {
        canvas.begin(S::frame(2.0f));
        const Col ink{ 0xEC, 0xEA, 0xE4, 0xFF };
        canvas.segment(5.0f, 6.0f, 70.0f, 8.0f, 1.5f, ink);
        {
            const Canvas::Scope tagged(canvas, funkgui::tags::slotCaret, false);
            canvas.segment(5.0f, 6.0f, 70.0f, 8.0f, 1.5f, ink);
        }
        {
            const Canvas::Scope live(canvas, funkgui::tags::none, true);
            canvas.segment(5.0f, 6.0f, 70.0f, 8.0f, 1.5f, ink);
        }
        const PrimList& l = canvas.end();
        const Inst a = funkgui::instance(l.prims[0]), b = funkgui::instance(l.prims[1]);
        Inst c = funkgui::instance(l.prims[2]);
        P.eq("tag.not_in_record", std::memcmp(&a, &b, sizeof a) == 0, 1);
        const bool liveFlag = c.packed[3] == 1.0f && a.packed[3] == 0.0f;
        c.packed[3] = a.packed[3];
        P.eq("live.only_the_flags", liveFlag && std::memcmp(&a, &c, sizeof a) == 0, 1);
    }

    // ---- every primitive of the scenes, at three scales -------------------------------------------------------------
    struct Scale { float dpi; const char* name; };
    struct Scene { const char* name; void (*record)(Canvas&); };
    const Scene scenes[] = { { "kinds", S::kinds }, { "colours", S::colours }, { "stress", S::stress },
                             { "split", S::split } };
    for (const Scale sc : { Scale{ 1.0f, "dpi1" }, Scale{ 1.5f, "dpi1_5" }, Scale{ 2.0f, "dpi2" } })
        for (const Scene& scene : scenes)
        {
            canvas.begin(S::frame(sc.dpi));
            scene.record(canvas);
            const PrimList& l = canvas.end();
            const std::string key = std::string("scene.") + scene.name + "." + sc.name;
            int prims = 0;
            P.eq(key + ".corners", corners(l, key, prims), 1);
            std::vector<Inst> recs;
            P.eq(key + ".instanceable", funkgui::instances(l, recs) && recs.size() == l.prims.size(), 1);
            std::printf("INFO     %s: %d primitives, %zu bytes as instances, %zu as vertices\n", key.c_str(), prims,
                        static_cast<std::size_t>(prims) * sizeof(Inst),
                        static_cast<std::size_t>(prims) * sizeof(Vtx) * funkgui::kVerticesPerPrim);
            if (std::string(scene.name) == "stress")
                P.eq(key + ".prims_at_least", prims >= S::kStressPrims, 1);
        }

    return P.finish();
}

// FUNKGUI_TEST name=fg.canvas.expansion timeout=600 gpu=0
//
// fg.canvas.expansion, the recorder-parity spike (02 §3.2, §3.11; SPRINTS.md S2.3): a call recorded by funkgui::Canvas
// and expanded by funkgui::expand() (canvas/Expand.h) gives the vertex stream HR's SdfCanvas pushed for the same call,
// bit for bit, for one primitive of each HR kind and their variants. Two references:
// - literal vertices worked out by hand for an rrect, a hairline and a segment (values exact in binary floating point);
// - HrReference below: the snapshot's gpu/SdfCanvas.cpp (HR Source/gui/SdfCanvas.cpp) primitive code re-typed
//   expression for expression, pushing HR's 64-byte vertices a, b, c, a, c, d, for every kind including text (over the
//   bundled atlas, with alignment, tracking, tabular digits, extras and an unknown codepoint, at dpi 1, 1.5 and 2).
// Also: the vertex layout offsets, expand(list, out)'s size, order and kept capacity, that the tag never reaches the
// stream and that the live flag changes nothing but d2[3]. Spec rows only.

#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/Expand.h>
#include <funkgui/canvas/Prim.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Col.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/test/Harness.h>
#include <funkgui/text/FontService.h>
#include <funkgui/text/TextStyle.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace T = funkgui::test;
using funkgui::Canvas;
using funkgui::Col;
using funkgui::FontAtlasSdf;
using funkgui::TextStyle;
using funkgui::Vtx;

namespace
{
    // ---- HR's SdfCanvas, primitive code only (gpu/SdfCanvas.cpp:12-260), pushing into a vector ----------------------
    class HrReference
    {
    public:
        explicit HrReference(const FontAtlasSdf& f) : font_(f) {}

        void begin(int logicalH, int physH, float textGamma)
        {
            dpiScale_ = logicalH > 0 ? static_cast<float>(physH) / static_cast<float>(logicalH) : 1.0f;
            if (!(dpiScale_ > 0.05f)) dpiScale_ = 1.0f;
            textGamma_ = textGamma;
            verts.clear();
        }

        void rrect(float x, float y, float w, float h, float radius, Col fill, float borderW = 0.0f, Col border = {},
                   float softness = 0.0f)
        {
            rrect4(x, y, w, h, radius, radius, radius, radius, fill, borderW, border, softness);
        }

        void rrect4(float x, float y, float w, float h, float rTL, float rTR, float rBR, float rBL, Col fill,
                    float borderW = 0.0f, Col border = {}, float softness = 0.0f)
        {
            if (!(w > 0.0f) || !(h > 0.0f)) return;

            const float hw = w * 0.5f, hh = h * 0.5f;
            const float pad = 1.5f + softness;   // AA / glow apron

            Vtx v{};
            v.c0 = pack(fill);
            v.c1 = pack(border);
            v.d0[2] = hw;   v.d0[3] = hh;
            v.d1[0] = rTL;  v.d1[1] = rTR;  v.d1[2] = rBR;  v.d1[3] = rBL;
            v.d2[0] = borderW; v.d2[1] = softness; v.d2[2] = kKindRRect; v.d2[3] = 0.0f;

            auto at = [&](float px, float py, float lx, float ly)
            {
                Vtx r = v; r.x = px; r.y = py; r.d0[0] = lx; r.d0[1] = ly;
                return r;
            };
            quad(at(x - pad,     y - pad,     -hw - pad, -hh - pad),
                 at(x + w + pad, y - pad,      hw + pad, -hh - pad),
                 at(x + w + pad, y + h + pad,  hw + pad,  hh + pad),
                 at(x - pad,     y + h + pad, -hw - pad,  hh + pad));
        }

        void segment(float x0, float y0, float x1, float y1, float width, Col c, float softness = 0.0f)
        {
            if (!(width > 0.0f)) return;
            const float r   = width * 0.5f;
            const float pad = 1.5f + softness;
            const float bx0 = std::min(x0, x1) - r, bx1 = std::max(x0, x1) + r;
            const float by0 = std::min(y0, y1) - r, by1 = std::max(y0, y1) + r;
            const float cx = (bx0 + bx1) * 0.5f, cy = (by0 + by1) * 0.5f;
            const float hw = (bx1 - bx0) * 0.5f, hh = (by1 - by0) * 0.5f;

            Vtx v{};
            v.c0 = pack(c);
            v.c1 = v.c0;
            v.d0[2] = hw; v.d0[3] = hh;
            v.d1[0] = x0 - cx; v.d1[1] = y0 - cy; v.d1[2] = x1 - cx; v.d1[3] = y1 - cy;
            v.d2[0] = r; v.d2[1] = softness; v.d2[2] = kKindSegment; v.d2[3] = 0.0f;
            auto at = [&](float px, float py, float lx, float ly)
            {
                Vtx q = v; q.x = px; q.y = py; q.d0[0] = lx; q.d0[1] = ly;
                return q;
            };
            quad(at(bx0 - pad, by0 - pad, -hw - pad, -hh - pad),
                 at(bx1 + pad, by0 - pad,  hw + pad, -hh - pad),
                 at(bx1 + pad, by1 + pad,  hw + pad,  hh + pad),
                 at(bx0 - pad, by1 + pad, -hw - pad,  hh + pad));
        }

        void hairlineH(float x, float y, float w, Col c)
        {
            const float t = 1.0f / dpiScale_;                       // one device px
            rrect(x, std::floor(y * dpiScale_) / dpiScale_, w, t, 0.0f, c);
        }

        void hairlineV(float x, float y, float h, Col c)
        {
            const float t = 1.0f / dpiScale_;
            rrect(std::floor(x * dpiScale_) / dpiScale_, y, t, h, 0.0f, c);
        }

        float textWidth(const char* s, const TextStyle& st) const
        {
            const auto& f = font_;
            const float scale = st.px / FontAtlasSdf::kBasePx;
            const float digit = f.maxDigitAdvance() * scale;
            float w = 0.0f;
            for (const char* p = s; *p != 0; )
            {
                const uint32_t cp = decodeUtf8(p);
                if (cp == ' ') { w += f.spaceAdvance() * scale + st.tracking; continue; }
                if (st.tabular && isDigit(cp)) { w += digit + st.tracking; continue; }
                if (const auto* g = f.glyph(cp)) w += g->advance * scale + st.tracking;
            }
            return w > 0.0f ? w - st.tracking : 0.0f;
        }

        enum class Align { left, centre, right };

        void text(const char* s, float x, float y, const TextStyle& st, Col c, Align align = Align::left)
        {
            const auto& f = font_;
            const float scale = st.px / FontAtlasSdf::kBasePx;

            if (align != Align::left)
            {
                const float w = textWidth(s, st);
                x -= (align == Align::centre) ? w * 0.5f : w;
            }

            const float aa = 1.0f
                / (dpiScale_ * 2.0f * static_cast<float>(FontAtlasSdf::kSpread) * scale);

            const float digit = f.maxDigitAdvance() * scale;
            const uint32_t col = pack(c);
            float pen  = x;
            const float base = y + f.ascent() * scale;

            for (const char* p = s; *p != 0; )
            {
                const uint32_t cp = decodeUtf8(p);
                if (cp == ' ') { pen += f.spaceAdvance() * scale + st.tracking; continue; }

                const auto* g = f.glyph(cp);
                if (g == nullptr) continue;

                const bool tab = st.tabular && isDigit(cp);
                const float adv = tab ? digit : g->advance * scale;
                const float slot = tab ? (digit - g->advance * scale) * 0.5f : 0.0f;

                const float gx = pen + slot + g->bx * scale;
                const float gy = base + g->by * scale;
                const float gw = g->w * scale;
                const float gh = g->h * scale;

                Vtx v{};
                v.c0 = col; v.c1 = col;
                v.d1[0] = st.weight; v.d1[1] = aa; v.d1[2] = textGamma_; v.d1[3] = 0.0f;
                v.d2[0] = 0.0f; v.d2[1] = 0.0f; v.d2[2] = kKindText; v.d2[3] = 0.0f;

                auto at = [&](float vx, float vy, float u, float vv)
                {
                    Vtx r = v; r.x = vx; r.y = vy;
                    r.d0[0] = u; r.d0[1] = vv; r.d0[2] = 0.0f; r.d0[3] = 0.0f;
                    return r;
                };
                quad(at(gx,      gy,      g->u0, g->v0),
                     at(gx + gw, gy,      g->u1, g->v0),
                     at(gx + gw, gy + gh, g->u1, g->v1),
                     at(gx,      gy + gh, g->u0, g->v1));

                pen += adv + st.tracking;
            }
        }

        std::vector<Vtx> verts;

    private:
        static constexpr float kKindRRect   = 0.0f;
        static constexpr float kKindText    = 1.0f;
        static constexpr float kKindSegment = 2.0f;

        static uint32_t pack(Col c)
        {
            return  static_cast<uint32_t>(c.r)
                 | (static_cast<uint32_t>(c.g) << 8)
                 | (static_cast<uint32_t>(c.b) << 16)
                 | (static_cast<uint32_t>(c.a) << 24);
        }

        static uint32_t decodeUtf8(const char*& p)
        {
            const auto b0 = static_cast<unsigned char>(*p++);
            if (b0 < 0x80) return b0;

            int extra;
            uint32_t cp;
            if      ((b0 & 0xE0) == 0xC0) { extra = 1; cp = b0 & 0x1Fu; }
            else if ((b0 & 0xF0) == 0xE0) { extra = 2; cp = b0 & 0x0Fu; }
            else if ((b0 & 0xF8) == 0xF0) { extra = 3; cp = b0 & 0x07u; }
            else return 0;

            for (int i = 0; i < extra; ++i)
            {
                const auto b = static_cast<unsigned char>(*p);
                if ((b & 0xC0) != 0x80) return 0;
                cp = (cp << 6) | (b & 0x3Fu);
                ++p;
            }
            return cp;
        }

        static bool isDigit(uint32_t cp) { return cp >= '0' && cp <= '9'; }

        void quad(const Vtx& a, const Vtx& b, const Vtx& c, const Vtx& d)
        {
            verts.push_back(a); verts.push_back(b); verts.push_back(c);
            verts.push_back(a); verts.push_back(c); verts.push_back(d);
        }

        const FontAtlasSdf& font_;
        float dpiScale_ = 1.0f;
        float textGamma_ = 1.0f / 1.4f;
    };

    // ---- helpers ----------------------------------------------------------------------------------------------------

    bool sameBytes(const std::vector<Vtx>& a, const std::vector<Vtx>& b)
    {
        return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(Vtx)) == 0);
    }

    // The first differing float/word of two streams, for the log when a row fails.
    void describeDiff(const char* what, const std::vector<Vtx>& got, const std::vector<Vtx>& want)
    {
        if (got.size() != want.size())
        {
            std::printf("  %s: %zu vertices, HR %zu\n", what, got.size(), want.size());
            return;
        }
        for (size_t i = 0; i < got.size(); ++i)
        {
            uint32_t a[16], b[16];
            std::memcpy(a, &got[i], sizeof a);
            std::memcpy(b, &want[i], sizeof b);
            for (int k = 0; k < 16; ++k)
                if (a[k] != b[k])
                {
                    std::printf("  %s: vertex %zu word %d: %08x, HR %08x\n", what, i, k, a[k], b[k]);
                    return;
                }
        }
    }

    funkgui::FrameInfo frame(float dpi, float gamma)
    {
        funkgui::FrameInfo f;
        f.logicalW = 400;
        f.logicalH = 200;
        f.dpi = dpi;
        f.textGamma = gamma;
        return f;
    }

    Vtx vtx(float x, float y, uint32_t c0, uint32_t c1, float lx, float ly, float hw, float hh, const float (&d1)[4],
            const float (&d2)[4])
    {
        Vtx v{};
        v.x = x;
        v.y = y;
        v.c0 = c0;
        v.c1 = c1;
        v.d0[0] = lx;
        v.d0[1] = ly;
        v.d0[2] = hw;
        v.d0[3] = hh;
        for (int i = 0; i < 4; ++i)
        {
            v.d1[i] = d1[i];
            v.d2[i] = d2[i];
        }
        return v;
    }

    // a, b, c, a, c, d from the four corners of HR's quad().
    std::vector<Vtx> quad(const Vtx& a, const Vtx& b, const Vtx& c, const Vtx& d) { return { a, b, c, a, c, d }; }

    std::vector<Vtx> expanded(const funkgui::PrimList& l)
    {
        std::vector<Vtx> out;
        funkgui::expand(l, out);
        return out;
    }
}

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;      // the atlas bakes through JUCE's font stack
    T::Probe P("fg.canvas.expansion", "", argc, argv);

    // ---- layout -----------------------------------------------------------------------------------------------------
    P.eq("layout.vtx_size", static_cast<int64_t>(sizeof(Vtx)), 64);
    P.eq("layout.offsets", offsetof(Vtx, x) == 0 && offsetof(Vtx, y) == 4 && offsetof(Vtx, c0) == 8
                               && offsetof(Vtx, c1) == 12 && offsetof(Vtx, d0) == 16 && offsetof(Vtx, d1) == 32
                               && offsetof(Vtx, d2) == 48, 1);
    P.eq("layout.vertices_per_prim", funkgui::kVerticesPerPrim, 6);

    auto& fonts = funkgui::FontService::get();
    const FontAtlasSdf& atlas = fonts.atlas();
    if (!P.eq("font.ok", fonts.ok(), 1))
        return P.finish();

    Canvas canvas(atlas);
    HrReference hr(atlas);
    const Col fill{ 1, 2, 3, 4 }, border{ 5, 6, 7, 8 }, ink{ 0xEC, 0xEA, 0xE4, 0xFF };

    // ---- hand-computed vertices -------------------------------------------------------------------------------------
    {
        // rrect(10, 20, 30, 40, r 5, fill, border 2, border): hw 15, hh 20, pad 1.5.
        canvas.begin(frame(1.0f, 1.0f));
        canvas.rrect(10.0f, 20.0f, 30.0f, 40.0f, 5.0f, fill, 2.0f, border);
        const float d1[4] = { 5, 5, 5, 5 }, d2[4] = { 2, 0, 0, 0 };
        const uint32_t c0 = 0x04030201u, c1 = 0x08070605u;
        const auto want = quad(vtx(8.5f, 18.5f, c0, c1, -16.5f, -21.5f, 15, 20, d1, d2),
                               vtx(41.5f, 18.5f, c0, c1, 16.5f, -21.5f, 15, 20, d1, d2),
                               vtx(41.5f, 61.5f, c0, c1, 16.5f, 21.5f, 15, 20, d1, d2),
                               vtx(8.5f, 61.5f, c0, c1, -16.5f, 21.5f, 15, 20, d1, d2));
        const auto got = expanded(canvas.end());
        if (!P.eq("hand.rrect", sameBytes(got, want), 1))
            describeDiff("hand.rrect", got, want);
    }
    {
        // hairlineH(10, 20.3, 50) at dpi 2: y floors to 20, t = 0.5 -> rrect(10, 20, 50, 0.5, 0).
        canvas.begin(frame(2.0f, 1.0f));
        canvas.hairlineH(10.0f, 20.3f, 50.0f, fill);
        const float d1[4] = { 0, 0, 0, 0 }, d2[4] = { 0, 0, 0, 0 };
        const uint32_t c0 = 0x04030201u, c1 = 0xFF000000u;          // border defaults to Col{} = opaque black
        const auto want = quad(vtx(8.5f, 18.5f, c0, c1, -26.5f, -1.75f, 25, 0.25f, d1, d2),
                               vtx(61.5f, 18.5f, c0, c1, 26.5f, -1.75f, 25, 0.25f, d1, d2),
                               vtx(61.5f, 22.0f, c0, c1, 26.5f, 1.75f, 25, 0.25f, d1, d2),
                               vtx(8.5f, 22.0f, c0, c1, -26.5f, 1.75f, 25, 0.25f, d1, d2));
        const auto got = expanded(canvas.end());
        if (!P.eq("hand.hairline", sameBytes(got, want), 1))
            describeDiff("hand.hairline", got, want);
    }
    {
        // segment(10, 10, 20, 30, width 4): r 2; bbox 8..22 x 8..32, centre (15, 20), hw 7, hh 12; ends (-5, -10),
        // (5, 10); d2 (2, 0, kind 2, 0).
        canvas.begin(frame(1.0f, 1.0f));
        canvas.segment(10.0f, 10.0f, 20.0f, 30.0f, 4.0f, fill);
        const float d1[4] = { -5, -10, 5, 10 }, d2[4] = { 2, 0, 2, 0 };
        const uint32_t c = 0x04030201u;
        const auto want = quad(vtx(6.5f, 6.5f, c, c, -8.5f, -13.5f, 7, 12, d1, d2),
                               vtx(23.5f, 6.5f, c, c, 8.5f, -13.5f, 7, 12, d1, d2),
                               vtx(23.5f, 33.5f, c, c, 8.5f, 13.5f, 7, 12, d1, d2),
                               vtx(6.5f, 33.5f, c, c, -8.5f, 13.5f, 7, 12, d1, d2));
        const auto got = expanded(canvas.end());
        if (!P.eq("hand.segment", sameBytes(got, want), 1))
            describeDiff("hand.segment", got, want);
    }

    // ---- against HR's own arithmetic, every kind and variant, at three scales ---------------------------------------
    struct Scale { float dpi; const char* name; };
    for (const Scale sc : { Scale{ 1.0f, "dpi1" }, Scale{ 1.5f, "dpi1_5" }, Scale{ 2.0f, "dpi2" } })
    {
        const float dpi = sc.dpi;
        const std::string at = sc.name;
        const float gamma = 1.0f / 1.4f;
        const auto check = [&](const std::string& name, auto&& record, int prims)
        {
            canvas.begin(frame(dpi, gamma));
            hr.begin(200, static_cast<int>(200.0f * dpi), gamma);
            record();
            const funkgui::PrimList& l = canvas.end();
            const auto got = expanded(l);
            const std::string key = "hr." + name + "." + at;
            if (!P.eq(key + ".vertices", sameBytes(got, hr.verts), 1))
                describeDiff(key.c_str(), got, hr.verts);
            P.eq(key + ".prims", static_cast<int64_t>(l.prims.size()), prims);
        };

        check("rrect", [&] {
            canvas.rrect(12.25f, 7.5f, 33.0f, 18.75f, 4.0f, ink, 1.5f, fill);
            hr.rrect(12.25f, 7.5f, 33.0f, 18.75f, 4.0f, ink, 1.5f, fill);
        }, 1);
        check("rrect_soft", [&] {
            canvas.rrect(3.3f, 4.4f, 5.5f, 6.6f, 9999.0f, ink, 0.0f, {}, 2.75f);
            hr.rrect(3.3f, 4.4f, 5.5f, 6.6f, 9999.0f, ink, 0.0f, {}, 2.75f);
        }, 1);
        check("rrect_rounding", [&] {                   // inputs where (x + w) + pad != x + (w + pad) in float
            canvas.rrect(101.7f, 12.3f, 17.9f, 0.3f, 1.0f, ink, 0.0f, {}, 0.3f);
            hr.rrect(101.7f, 12.3f, 17.9f, 0.3f, 1.0f, ink, 0.0f, {}, 0.3f);
        }, 1);
        check("rrect4", [&] {
            canvas.rrect4(100.1f, 50.2f, 60.3f, 20.4f, 0.0f, 3.0f, 7.5f, 1.25f, fill, 0.5f, ink, 0.3f);
            hr.rrect4(100.1f, 50.2f, 60.3f, 20.4f, 0.0f, 3.0f, 7.5f, 1.25f, fill, 0.5f, ink, 0.3f);
        }, 1);
        check("rrect_empty", [&] {
            canvas.rrect(1.0f, 1.0f, 0.0f, 5.0f, 1.0f, ink);
            canvas.rrect(1.0f, 1.0f, 5.0f, -1.0f, 1.0f, ink);
            canvas.rrect(1.0f, 1.0f, std::nanf(""), 5.0f, 1.0f, ink);
            hr.rrect(1.0f, 1.0f, 0.0f, 5.0f, 1.0f, ink);
            hr.rrect(1.0f, 1.0f, 5.0f, -1.0f, 1.0f, ink);
            hr.rrect(1.0f, 1.0f, std::nanf(""), 5.0f, 1.0f, ink);
        }, 0);
        check("hairlines", [&] {
            canvas.hairlineH(16.0f, 60.3f, 200.0f, ink);
            canvas.hairlineV(230.7f, 56.1f, 40.0f, ink);
            hr.hairlineH(16.0f, 60.3f, 200.0f, ink);
            hr.hairlineV(230.7f, 56.1f, 40.0f, ink);
        }, 2);
        check("segment", [&] {
            canvas.segment(200.0f, 88.3f, 120.7f, 61.1f, 1.25f, ink, 0.5f);
            canvas.segment(10.0f, 10.0f, 10.0f, 90.0f, 2.0f, fill);
            hr.segment(200.0f, 88.3f, 120.7f, 61.1f, 1.25f, ink, 0.5f);
            hr.segment(10.0f, 10.0f, 10.0f, 90.0f, 2.0f, fill);
        }, 2);
        check("segment_empty", [&] {
            canvas.segment(0.0f, 0.0f, 10.0f, 10.0f, 0.0f, ink);
            hr.segment(0.0f, 0.0f, 10.0f, 10.0f, 0.0f, ink);
        }, 0);

        // Text: the recorder's text() against HR's, with an unknown codepoint (U+2212, not in the atlas until G4) that
        // both skip without an advance: the label run is 11 glyphs, 2 spaces and the skipped U+2212.
        const char* label = "Ab 1\xC2\xB0x \xE2\x88\x92q\xE2\x86\x92|%()";
        check("text_left", [&] {
            canvas.text(label, 20.3f, 40.7f, funkgui::type::kLabel, ink);
            hr.text(label, 20.3f, 40.7f, funkgui::type::kLabel, ink);
        }, 11);
        check("text_centre_tabular", [&] {
            canvas.text("-0123.45", 200.0f, 10.0f, funkgui::type::kValueP, ink, funkgui::Align::centre);
            hr.text("-0123.45", 200.0f, 10.0f, funkgui::type::kValueP, ink, HrReference::Align::centre);
        }, 8);
        check("text_right_tracked", [&] {
            canvas.text("THRESHOLD 1:4", 390.5f, 150.25f, funkgui::type::kCaption, fill, funkgui::Align::right);
            hr.text("THRESHOLD 1:4", 390.5f, 150.25f, funkgui::type::kCaption, fill, HrReference::Align::right);
        }, 12);
        check("text_display", [&] {
            canvas.text("88.8", 5.0f, 5.0f, funkgui::type::kDisplay, ink);
            hr.text("88.8", 5.0f, 5.0f, funkgui::type::kDisplay, ink);
        }, 4);
        P.eq("hr.text_width." + at, std::bit_cast<uint32_t>(canvas.textWidth(label, funkgui::type::kLabel))
                                        == std::bit_cast<uint32_t>(hr.textWidth(label, funkgui::type::kLabel)), 1);
    }

    // ---- expand(list, out): size, order, capacity; tag and live -----------------------------------------------------
    {
        canvas.begin(frame(2.0f, 1.0f));
        canvas.rrect(1.0f, 2.0f, 3.0f, 4.0f, 0.5f, ink);
        canvas.segment(5.0f, 6.0f, 7.0f, 8.0f, 1.0f, ink);
        canvas.text("Q", 9.0f, 10.0f, funkgui::type::kLabel, ink);
        const funkgui::PrimList& l = canvas.end();
        std::vector<Vtx> out;
        funkgui::expand(l, out);
        bool order = out.size() == 18;
        for (size_t i = 0; order && i < l.prims.size(); ++i)
        {
            Vtx one[6];
            funkgui::expand(l.prims[i], one);
            order = std::memcmp(one, &out[i * 6], sizeof one) == 0;
        }
        P.eq("list.size_and_order", order, 1);

        const size_t cap = out.capacity();
        canvas.begin(frame(2.0f, 1.0f));
        canvas.rrect(1.0f, 2.0f, 3.0f, 4.0f, 0.5f, ink);
        funkgui::expand(canvas.end(), out);
        P.eq("list.shrinks_keeping_capacity", out.size() == 6 && out.capacity() == cap, 1);
        canvas.begin(frame(2.0f, 1.0f));
        funkgui::expand(canvas.end(), out);
        P.eq("list.empty", out.empty(), 1);
    }
    {
        canvas.begin(frame(2.0f, 1.0f));
        canvas.segment(5.0f, 6.0f, 70.0f, 8.0f, 1.5f, ink);
        {
            const Canvas::Scope tagged(canvas, funkgui::tags::slotCaret, false);
            canvas.segment(5.0f, 6.0f, 70.0f, 8.0f, 1.5f, ink);
        }
        {
            const Canvas::Scope live(canvas, funkgui::tags::none, true);
            canvas.segment(5.0f, 6.0f, 70.0f, 8.0f, 1.5f, ink);
        }
        const auto v = expanded(canvas.end());
        P.eq("tag.not_in_stream", std::memcmp(&v[0], &v[6], 6 * sizeof(Vtx)) == 0, 1);
        bool onlyFlags = true;
        for (int i = 0; i < 6; ++i)
        {
            Vtx a = v[static_cast<size_t>(i)], b = v[static_cast<size_t>(12 + i)];
            onlyFlags = onlyFlags && b.d2[3] == 1.0f && a.d2[3] == 0.0f;
            a.d2[3] = b.d2[3];
            onlyFlags = onlyFlags && std::memcmp(&a, &b, sizeof a) == 0;
        }
        P.eq("live.only_d2w", onlyFlags, 1);
    }
    return P.finish();
}

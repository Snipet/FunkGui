#include <funkgui/canvas/Canvas.h>

#include <funkgui/text/TextFit.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

// The CPU recorder's HR half (02 §3.3): begin/end, the tag and live state, and HR's primitives with SdfCanvas.cpp's
// arithmetic kept expression for expression (the snapshot's gpu/SdfCanvas.cpp:95-260), so the six vertices
// canvas/Expand.h rebuilds from each Prim are bit-identical to the ones HR pushed for the same call
// (fg.canvas.expansion proves it per kind). HR's local-coordinate corners are (d0.x, d0.y), (e0.x, d0.y),
// (e0.x, e0.y), (d0.x, e0.y) for every kind, so a Prim stores the TL and BR corners and nothing is lost. G4's shapes
// (area, areaStrip, polyline, disc, dotted, axis) live in CanvasShapes.cpp and append through emit() as well.

namespace funkgui
{
    namespace
    {
        // HR SdfCanvas.cpp's pack(): RGBA8 as r | g<<8 | b<<16 | a<<24.
        uint32_t pack(Col c) noexcept
        {
            return static_cast<uint32_t>(c.r)
                 | (static_cast<uint32_t>(c.g) << 8)
                 | (static_cast<uint32_t>(c.b) << 16)
                 | (static_cast<uint32_t>(c.a) << 24);
        }

        bool isDigit(uint32_t cp) noexcept { return cp >= '0' && cp <= '9'; }

        // What an undecodable byte sequence is recorded as among the missing codepoints (0 marks an empty slot).
        constexpr uint32_t kReplacement = 0xFFFDu;

        void countMissing(PrimList& l, uint32_t cp) noexcept
        {
            ++l.missingGlyphs;
            const uint32_t v = cp == 0 ? kReplacement : cp;
            for (uint32_t& slot : l.missingFirst)
            {
                if (slot == v)
                    return;
                if (slot == 0)
                {
                    slot = v;
                    return;
                }
            }
        }
    }

    Canvas::Canvas(const FontAtlasSdf& atlas) : atlas_(atlas) {}

    void Canvas::begin(const FrameInfo& info)
    {
        list_.clear();
        list_.info = info;
        // HR begin(): a scale that is not a usable positive number draws at 1x instead of dividing by ~0.
        if (!(list_.info.dpi > 0.05f))
            list_.info.dpi = 1.0f;
    }

    const PrimList& Canvas::end()
    {
        return list_;
    }

    Prim& Canvas::emit(PrimKind kind)
    {
        Prim& p = list_.prims.emplace_back();            // value-initialised: every field 0
        p.d2[2] = static_cast<float>(static_cast<uint8_t>(kind));
        p.d2[3] = live_ ? static_cast<float>(pflag::live) : 0.0f;
        p.tag = tag_;
        return p;
    }

    // ---- HR primitives (SdfCanvas.cpp:95-260) -----------------------------------------------------------------------

    void Canvas::rrect(float x, float y, float w, float h, float radius, Col fill, float borderW, Col border,
                       float softness)
    {
        rrect4(x, y, w, h, radius, radius, radius, radius, fill, borderW, border, softness);
    }

    void Canvas::rrect4(float x, float y, float w, float h, float rTL, float rTR, float rBR, float rBL, Col fill,
                        float borderW, Col border, float softness)
    {
        if (!(w > 0.0f) || !(h > 0.0f))
            return;

        const float hw = w * 0.5f, hh = h * 0.5f;
        const float pad = 1.5f + softness;               // AA / glow apron

        Prim& p = emit(PrimKind::rrect);
        p.c0 = pack(fill);
        p.c1 = pack(border);
        p.d0[2] = hw;
        p.d0[3] = hh;
        p.d1[0] = rTL;
        p.d1[1] = rTR;
        p.d1[2] = rBR;
        p.d1[3] = rBL;
        p.d2[0] = borderW;
        p.d2[1] = softness;
        // HR's corners: a = (x - pad, y - pad) local (-hw - pad, -hh - pad), c = (x + w + pad, y + h + pad) local
        // (hw + pad, hh + pad); b and d mix them.
        p.x0 = x - pad;
        p.y0 = y - pad;
        p.x1 = x + w + pad;
        p.y1 = y + h + pad;
        p.d0[0] = -hw - pad;
        p.d0[1] = -hh - pad;
        p.e0[0] = hw + pad;
        p.e0[1] = hh + pad;
    }

    void Canvas::hairlineH(float x, float y, float w, Col c)
    {
        const float dpi = list_.info.dpi;
        const float t = 1.0f / dpi;                      // one device px
        rrect(x, std::floor(y * dpi) / dpi, w, t, 0.0f, c);
    }

    void Canvas::hairlineV(float x, float y, float h, Col c)
    {
        const float dpi = list_.info.dpi;
        const float t = 1.0f / dpi;
        rrect(std::floor(x * dpi) / dpi, y, t, h, 0.0f, c);
    }

    void Canvas::segment(float x0, float y0, float x1, float y1, float width, Col c, float softness)
    {
        if (!(width > 0.0f))
            return;
        const float r = width * 0.5f;
        const float pad = 1.5f + softness;
        // The quad is the segment's bounding box grown by the radius and the AA apron; local coordinates are relative
        // to its centre, and the endpoints travel in d1 in that same frame (HR).
        const float bx0 = std::min(x0, x1) - r, bx1 = std::max(x0, x1) + r;
        const float by0 = std::min(y0, y1) - r, by1 = std::max(y0, y1) + r;
        const float cx = (bx0 + bx1) * 0.5f, cy = (by0 + by1) * 0.5f;
        const float hw = (bx1 - bx0) * 0.5f, hh = (by1 - by0) * 0.5f;

        Prim& p = emit(PrimKind::segment);
        p.c0 = pack(c);
        p.c1 = p.c0;
        p.d0[2] = hw;
        p.d0[3] = hh;
        p.d1[0] = x0 - cx;
        p.d1[1] = y0 - cy;
        p.d1[2] = x1 - cx;
        p.d1[3] = y1 - cy;
        p.d2[0] = r;
        p.d2[1] = softness;
        p.x0 = bx0 - pad;
        p.y0 = by0 - pad;
        p.x1 = bx1 + pad;
        p.y1 = by1 + pad;
        p.d0[0] = -hw - pad;
        p.d0[1] = -hh - pad;
        p.e0[0] = hw + pad;
        p.e0[1] = hh + pad;
    }

    float Canvas::snapY(float y) const
    {
        const float dpi = list_.info.dpi;
        return std::floor(y * dpi + 0.5f) / dpi;
    }

    float Canvas::snapX(float x) const
    {
        const float dpi = list_.info.dpi;
        return std::floor(x * dpi + 0.5f) / dpi;
    }

    float Canvas::textWidth(const char* utf8, const TextStyle& st) const
    {
        return text::width(atlas_, utf8, st);          // the one definition (text/TextFit.h), so the two never drift
    }

    float Canvas::capCentreTop(float centreY, const TextStyle& st) const
    {
        const float scale = st.px / FontAtlasSdf::kBasePx;
        // Top of the em box such that the cap band straddles centreY.
        return centreY - (atlas_.ascent() - atlas_.capHeight() * 0.5f) * scale;
    }

    float Canvas::sharedBaselineTop(float otherTop, const TextStyle& other, const TextStyle& mine) const
    {
        return otherTop + (other.px - mine.px) * (atlas_.ascent() / FontAtlasSdf::kBasePx);
    }

    void Canvas::text(const char* s, float x, float y, const TextStyle& st, Col c, Align align)
    {
        if (s == nullptr || !atlas_.baked())
            return;                                      // HR: no font, no text (and nothing counted)

        const auto& f = atlas_;
        const float scale = st.px / FontAtlasSdf::kBasePx;

        if (align != Align::left)
        {
            const float w = textWidth(s, st);
            x -= (align == Align::centre) ? w * 0.5f : w;
        }

        // One physical pixel expressed in field units, given how far the distance range is being scaled at this size
        // (HR: computed here rather than from fwidth() in the shader).
        const float aa = 1.0f / (list_.info.dpi * 2.0f * static_cast<float>(FontAtlasSdf::kSpread) * scale);

        const float digit = f.maxDigitAdvance() * scale;
        const uint32_t col = pack(c);
        const float gamma = list_.info.textGamma;
        float pen = x;
        const float base = y + f.ascent() * scale;

        for (const char* p = s; *p != 0;)
        {
            const uint32_t cp = text::decodeUtf8(p);
            if (cp == ' ')
            {
                pen += f.spaceAdvance() * scale + st.tracking;
                continue;
            }

            const auto* g = f.glyph(cp);
            if (g == nullptr)
            {
                countMissing(list_, cp);                 // HR dropped it silently (A §2.5); the dump reports it
                continue;
            }

            const bool tab = st.tabular && isDigit(cp);
            const float adv = tab ? digit : g->advance * scale;
            // Tabular figures share one advance so columns of numbers do not shimmer; each glyph is centred in it.
            const float slot = tab ? (digit - g->advance * scale) * 0.5f : 0.0f;

            const float gx = pen + slot + g->bx * scale;
            const float gy = base + g->by * scale;
            const float gw = g->w * scale;
            const float gh = g->h * scale;

            Prim& q = emit(PrimKind::text);
            q.c0 = col;
            q.c1 = col;
            q.d1[0] = st.weight;
            q.d1[1] = aa;
            q.d1[2] = gamma;
            q.x0 = gx;
            q.y0 = gy;
            q.x1 = gx + gw;
            q.y1 = gy + gh;
            q.d0[0] = g->u0;
            q.d0[1] = g->v0;
            q.e0[0] = g->u1;
            q.e0[1] = g->v1;

            pen += adv + st.tracking;
        }
    }

    // ---- tag and live state -----------------------------------------------------------------------------------------

    void Canvas::setTag(Tag t) { tag_ = t; }
    Tag  Canvas::tag() const { return tag_; }
    void Canvas::setLive(bool on) { live_ = on; }
    bool Canvas::live() const { return live_; }

    Canvas::Scope::Scope(Canvas& canvas, Tag tag, bool live) : c(canvas), t(canvas.tag()), l(canvas.live())
    {
        c.setTag(tag);
        c.setLive(live);
    }

    Canvas::Scope::~Scope()
    {
        c.setTag(t);
        c.setLive(l);
    }

    float Canvas::dpi() const { return list_.info.dpi; }

    int Canvas::missingGlyphs() const { return static_cast<int>(list_.missingGlyphs); }
}

#include <funkgui/gpu/SdfCanvas.h>
#include <string>
#include <funkgui/gpu/BgfxContext.h>
#include <funkgui/gpu/FramePump.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace funkgui
{
    namespace
    {
        constexpr float kKindRRect   = 0.0f;
        constexpr float kKindText    = 1.0f;
        constexpr float kKindSegment = 2.0f;

        inline uint32_t pack(Col c)
        {
            return  static_cast<uint32_t>(c.r)
                 | (static_cast<uint32_t>(c.g) << 8)
                 | (static_cast<uint32_t>(c.b) << 16)
                 | (static_cast<uint32_t>(c.a) << 24);
        }

        // Minimal UTF-8 decode. char is signed on arm64, so passing *p straight
        // to glyph(char) turned every byte of a multi-byte sequence negative
        // and dropped it, silently losing characters from the middle of any
        // string containing a degree or multiplication sign. Returns 0 and
        // consumes one byte on malformed input so a bad string cannot spin the
        // caller's loop.
        uint32_t decodeUtf8(const char*& p)
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

        inline bool isDigit(uint32_t cp) { return cp >= '0' && cp <= '9'; }
    }

    void SdfCanvas::begin(bgfx::ViewId view, bgfx::FrameBufferHandle fb,
                          int logicalW, int logicalH, int physW, int physH,
                          Col clear, float seconds)
    {
        clear_ = clear;
        view_  = view;
        viewW_ = static_cast<float>(logicalW);
        viewH_ = static_cast<float>(logicalH);
        seconds_ = seconds;
        dpiScale_ = logicalH > 0 ? static_cast<float>(physH) / static_cast<float>(logicalH)
                                 : 1.0f;
        if (!(dpiScale_ > 0.05f)) dpiScale_ = 1.0f;
        verts_.clear();

        bgfx::setViewFrameBuffer(view_, fb);
        bgfx::setViewRect(view_, 0, 0,
                          static_cast<uint16_t>(physW),
                          static_cast<uint16_t>(physH));
        // The clear colour is derived from the palette rather than duplicated
        // as a literal, so theming cannot desync the two.
        bgfx::setViewClear(view_, BGFX_CLEAR_COLOR,
                           (static_cast<uint32_t>(clear.r) << 24)
                         | (static_cast<uint32_t>(clear.g) << 16)
                         | (static_cast<uint32_t>(clear.b) << 8)
                         | 0xffu);
        bgfx::setViewMode(view_, bgfx::ViewMode::Sequential);
        bgfx::touch(view_);
    }

    void SdfCanvas::quad(const Vtx& a, const Vtx& b, const Vtx& c, const Vtx& d)
    {
        verts_.push_back(a); verts_.push_back(b); verts_.push_back(c);
        verts_.push_back(a); verts_.push_back(c); verts_.push_back(d);
    }

    void SdfCanvas::rrect(float x, float y, float w, float h, float radius,
                          Col fill, float borderW, Col border, float softness)
    {
        rrect4(x, y, w, h, radius, radius, radius, radius,
               fill, borderW, border, softness);
    }

    void SdfCanvas::rrect4(float x, float y, float w, float h,
                           float rTL, float rTR, float rBR, float rBL,
                           Col fill, float borderW, Col border, float softness)
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

    void SdfCanvas::segment(float x0, float y0, float x1, float y1, float width, Col c,
                            float softness)
    {
        if (!(width > 0.0f)) return;
        const float r   = width * 0.5f;
        const float pad = 1.5f + softness;
        // The quad is the segment's bounding box grown by the radius and the
        // AA apron; local coordinates are relative to its centre, and the
        // endpoints travel in d1 in that same frame.
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

    float SdfCanvas::snapY(float y) const
    {
        return std::floor(y * dpiScale_ + 0.5f) / dpiScale_;
    }

    void SdfCanvas::hairlineH(float x, float y, float w, Col c)
    {
        const float t = 1.0f / dpiScale_;                       // one device px
        rrect(x, std::floor(y * dpiScale_) / dpiScale_, w, t, 0.0f, c);
    }

    void SdfCanvas::hairlineV(float x, float y, float h, Col c)
    {
        const float t = 1.0f / dpiScale_;
        rrect(std::floor(x * dpiScale_) / dpiScale_, y, t, h, 0.0f, c);
    }

    float SdfCanvas::textWidth(const char* s, const TextStyle& st) const
    {
        const auto& f = ctx_.font();
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
        // Tracking is trailing space after the last glyph; excluding it keeps
        // centred and right-aligned runs optically correct.
        return w > 0.0f ? w - st.tracking : 0.0f;
    }

    float SdfCanvas::capCentreTop(float centreY, const TextStyle& st) const
    {
        const auto& f = ctx_.font();
        const float scale = st.px / FontAtlasSdf::kBasePx;
        // Top of the em box such that the cap band straddles centreY.
        return centreY - (f.ascent() - f.capHeight() * 0.5f) * scale;
    }

    float SdfCanvas::sharedBaselineTop(float otherTop, const TextStyle& other,
                                       const TextStyle& mine) const
    {
        const auto& f = ctx_.font();
        return otherTop + (other.px - mine.px) * (f.ascent() / FontAtlasSdf::kBasePx);
    }

    void SdfCanvas::text(const char* s, float x, float y, const TextStyle& st,
                         Col c, Align align)
    {
        if (!ctx_.hasFont()) return;

        const auto& f = ctx_.font();
        const float scale = st.px / FontAtlasSdf::kBasePx;

        if (align != Align::left)
        {
            const float w = textWidth(s, st);
            x -= (align == Align::centre) ? w * 0.5f : w;
        }

        // One physical pixel expressed in field units, given how far the
        // distance range is being scaled at this size. Computing it here rather
        // than from fwidth() in the shader keeps the fragment path free of
        // derivatives.
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
            // Tabular figures share one advance so columns of numbers do not
            // shimmer as digits change; each glyph is centred in that slot.
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

    namespace
    {
        // Set once from the environment, consumed after g_dumpSkip frames.
        const char* g_dumpPath = nullptr;
        bool g_dumpDone = false;
        int  g_dumpSkip = 0;
    }

    void SdfCanvas::dumpNextFrameTo(const char* path, int skipFrames)
    {
        g_dumpPath = path;
        g_dumpSkip = skipFrames;
        g_dumpDone = false;
    }

    void SdfCanvas::end()
    {
        if (verts_.empty()) return;

        if (g_dumpPath != nullptr && !g_dumpDone && g_dumpSkip-- <= 0)
        {
            g_dumpDone = true;
            // Written beside the destination and renamed onto it once closed:
            // Scripts/capture-frame.sh takes the file's existence to mean the
            // frame is complete and stops the app at once, and a poll landing
            // inside a thousand buffered writes read a truncated frame.
            // rename() within one directory is atomic.
            const std::string tmpPath = std::string(g_dumpPath) + ".partial";
            if (FILE* f = std::fopen(tmpPath.c_str(), "w"))
            {
                std::fprintf(f, "clear %02x%02x%02x\n", clear_.r, clear_.g, clear_.b);
                std::fprintf(f, "view %g %g dpi %g clock %s fps %.1f rate %s\n",
             viewW_, viewH_, dpiScale_,
             FramePump::get().isDisplayLinked() ? "displaylink" : "timer",
             FramePump::get().measuredFps(),
             FramePump::get().wantedFullRate() ? "full" : "idle");
                // Two triangles per primitive: vertex 0 is the top-left corner
                // and vertex 2 the bottom-right, which is enough to reconstruct
                // the quad and its per-primitive parameters.
                for (size_t i = 0; i + 5 < verts_.size(); i += 6)
                {
                    const Vtx& a = verts_[i];
                    const Vtx& c = verts_[i + 2];
                    std::fprintf(f,
                        "p %g %g %g %g  c0 %08x c1 %08x  d0 %g %g %g %g  "
                        "e0 %g %g  d1 %g %g %g %g  d2 %g %g %g %g\n",
                        a.x, a.y, c.x, c.y, a.c0, a.c1,
                        a.d0[0], a.d0[1], a.d0[2], a.d0[3],
                        c.d0[0], c.d0[1],
                        a.d1[0], a.d1[1], a.d1[2], a.d1[3],
                        a.d2[0], a.d2[1], a.d2[2], a.d2[3]);
                }
                const bool written = std::fclose(f) == 0;
                if (written) std::rename(tmpPath.c_str(), g_dumpPath);
                else         std::remove(tmpPath.c_str());
            }
        }

        const uint32_t n = static_cast<uint32_t>(verts_.size());
        if (bgfx::getAvailTransientVertexBuffer(n, ctx_.layout()) < n)
        {
            // The whole frame is one batch, so exhausting the transient buffer
            // drops everything rather than one class of widget. At this
            // primitive count it should be unreachable.
            verts_.clear();
            return;
        }

        bgfx::TransientVertexBuffer tvb;
        bgfx::allocTransientVertexBuffer(&tvb, n, ctx_.layout());
        std::memcpy(tvb.data, verts_.data(), n * sizeof(Vtx));

        const float vs[4] = { viewW_, viewH_, 1.0f / dpiScale_, seconds_ };
        bgfx::setUniform(ctx_.uViewSize(), vs);
        bgfx::setTexture(0, ctx_.sTexColor(), ctx_.fontTex());
        bgfx::setVertexBuffer(0, &tvb, 0, n);
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                     | BGFX_STATE_BLEND_ALPHA);
        bgfx::submit(view_, ctx_.progUi());
        verts_.clear();
    }
}

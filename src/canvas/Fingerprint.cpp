#include <funkgui/canvas/Fingerprint.h>

// addMetrics writes rows on a Harness v2 Probe, so this one library file needs the harness's definition. The harness
// compares floats exactly on purpose (03 §2.6); JUCE's recommended warnings, which a consumer's target applies to
// FunkGui's sources, include -Wfloat-equal.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfloat-equal"
#include <funkgui/test/Harness.h>
#pragma clang diagnostic pop

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <funkgui/core/CLocale.h>                        // snprintfC, strtofC: the C locale on macOS and Linux

// Frame fingerprints (02 §3.9). Choices where 02 §3.9 is silent (G3):
// - A primitive's kind is classified by range, as the shader and SoftRaster do (d2[2] < 0.5 rrect, < 1.5 text, < 2.5
//   segment, else area); its flags are int(d2[3] + 0.5).
// - Colours and tags are not hashed (a theme is a pure token swap; tags are counted in tagCounts). The flags in d2[3]
//   are hashed with the rest of d2.
// - excludeLive leaves live primitives out of both hashes, the kind counts, the extents and tagCounts; `live` counts
//   them either way. `statics` counts the primitives that were hashed.
// - tagCounts lists the hashed primitives' tags, untagged (0) excluded.
// - A quantised value outside int32 saturates; NaN hashes as INT32_MIN.
// - legacyHr is HR's FrameRender.cpp:108-150 exactly: every float first taken through "%g" and back (HR hashed what
//   its "%g" dump held), raw float bits hashed, no quantum, no gamma rule, the Rank band's energy caps (non-text,
//   non-segment prims inside y 165-295 with an SDF half-height <= 2) skipped and counted as `live`, text hashed into
//   `geometry` with everything else; `text` stays 0, and neither areas nor tags are counted (HR had neither).

namespace funkgui
{
    namespace
    {
        constexpr uint64_t kFnvOffset = 1469598103934665603ull;
        constexpr uint64_t kFnvPrime  = 1099511628211ull;

        void mix32(uint64_t& h, uint32_t bits) noexcept
        {
            for (int k = 0; k < 4; ++k)                  // little-endian bytes, whatever the host order (HR)
            {
                h ^= (bits >> (k * 8)) & 0xFFu;
                h *= kFnvPrime;
            }
        }

        int32_t quantise(float v, float quantum) noexcept
        {
            if (std::isnan(v))
                return INT32_MIN;
            const float q = v / quantum;
            if (!(q < 2147483520.0f))                    // the largest float below 2^31
                return INT32_MAX;
            if (!(q > -2147483648.0f))
                return INT32_MIN;
            return static_cast<int32_t>(std::lrint(q));
        }

        enum class Class : uint8_t { rrect, text, segment, area };

        Class classify(const Prim& p) noexcept
        {
            const float k = p.d2[2];
            return k < 0.5f ? Class::rrect : (k < 1.5f ? Class::text : (k < 2.5f ? Class::segment : Class::area));
        }

        bool isLive(const Prim& p) noexcept
        {
            const float f = p.d2[3] + 0.5f;
            return f >= 1.0f && f < 2147483520.0f && (static_cast<uint32_t>(f) & pflag::live) != 0;
        }

        // The 18 hashed fields, in 02 §3.9's order.
        std::array<float, 18> fields(const Prim& p) noexcept
        {
            return { p.x0, p.y0, p.x1, p.y1, p.d0[0], p.d0[1], p.d0[2], p.d0[3], p.e0[0], p.e0[1],
                     p.d1[0], p.d1[1], p.d1[2], p.d1[3], p.d2[0], p.d2[1], p.d2[2], p.d2[3] };
        }

        void countTag(std::vector<std::pair<Tag, int>>& counts, Tag t)
        {
            if (t == tags::none)
                return;
            const auto it = std::lower_bound(counts.begin(), counts.end(), t,
                                             [](const std::pair<Tag, int>& e, Tag v) { return e.first < v; });
            if (it != counts.end() && it->first == t)
                ++it->second;
            else
                counts.insert(it, { t, 1 });
        }

        // A float as HR's dump held it: printed with "%g", read back with sscanf("%f"), both in the C locale.
        float throughG(float v)
        {
            char buf[64];
            snprintfC(buf, sizeof buf, "%g", static_cast<double>(v));
            return strtofC(buf, nullptr);
        }

        Fingerprint legacy(const PrimList& list)
        {
            Fingerprint fp;
            uint64_t h = kFnvOffset;
            for (const Prim& p : list.prims)
            {
                std::array<float, 18> v = fields(p);
                for (float& x : v)
                    x = throughG(x);
                const float y0 = v[1], x1 = v[2], y1 = v[3], hh = v[7], kind = v[16];
                const bool isText = kind > 0.5f && kind < 1.5f;
                const bool isSeg = kind > 1.5f;
                const bool inRank = y0 >= 165.0f && y1 <= 295.0f && !isText && !isSeg;
                if (inRank && hh <= 2.0f)
                {
                    ++fp.live;                           // HR's "energy caps": its one piece of live geometry
                    continue;
                }
                ++fp.statics;
                if (isText)
                    ++fp.texts;
                else if (isSeg)
                    ++fp.segments;
                else
                    ++fp.rrects;
                for (const float x : v)
                {
                    uint32_t bits = 0;
                    std::memcpy(&bits, &x, sizeof bits);
                    mix32(h, bits);
                }
                fp.maxX = std::max(fp.maxX, x1);
                fp.maxY = std::max(fp.maxY, y1);
            }
            fp.geometry = h;
            return fp;
        }
    }

    Fingerprint fingerprint(const PrimList& list, const FingerprintOptions& opt)
    {
        if (opt.legacyHr)
            return legacy(list);

        const float quantum = opt.quantum > 0.0f && std::isfinite(opt.quantum) ? opt.quantum : 1.0f / 1024.0f;
        Fingerprint fp;
        uint64_t geometry = kFnvOffset, text = kFnvOffset;
        for (const Prim& p : list.prims)
        {
            const bool live = isLive(p);
            if (live)
                ++fp.live;
            if (live && opt.excludeLive)
                continue;

            const Class k = classify(p);
            std::array<float, 18> v = fields(p);
            if (k == Class::text && opt.themeInvariant)
                v[12] = 0.0f;                            // d1[2], the text gamma: the one per-theme geometry field
            uint64_t& h = k == Class::text ? text : geometry;
            for (const float x : v)
                mix32(h, static_cast<uint32_t>(quantise(x, quantum)));

            ++fp.statics;
            switch (k)
            {
                case Class::rrect:   ++fp.rrects;   break;
                case Class::text:    ++fp.texts;    break;
                case Class::segment: ++fp.segments; break;
                case Class::area:    ++fp.areas;    break;
            }
            fp.maxX = std::max(fp.maxX, p.x1);
            fp.maxY = std::max(fp.maxY, p.y1);
            countTag(fp.tagCounts, p.tag);
        }

        // v0.19.0: the shader draws, after every primitive (Fingerprint.h says what is hashed). None: nothing mixed.
        for (const ShaderDraw& s : list.shaders)
        {
            if (s.live)
                ++fp.live;
            if (s.live && opt.excludeLive)
                continue;
            mix32(geometry, s.id);
            mix32(geometry, s.at);
            const std::array<float, 8> v = { s.rect.x, s.rect.y, s.rect.w, s.rect.h,
                                             s.clip.x, s.clip.y, s.clip.w, s.clip.h };
            mix32(geometry, s.clipped ? 1u : 0u);
            for (const float x : v)
                mix32(geometry, static_cast<uint32_t>(quantise(x, quantum)));
            ++fp.shaders;
            fp.maxX = std::max(fp.maxX, s.rect.right());
            fp.maxY = std::max(fp.maxY, s.rect.bottom());
            countTag(fp.tagCounts, s.tag);
        }

        // v0.20.0: the image draws, after the shader draws (Fingerprint.h says what is hashed). None: nothing mixed.
        for (const ImageDraw& m : list.images)
        {
            if (m.live)
                ++fp.live;
            if (m.live && opt.excludeLive)
                continue;
            mix32(geometry, m.id);
            mix32(geometry, m.at);
            mix32(geometry, m.shaderAt);
            mix32(geometry, m.clipped ? 1u : 0u);
            const std::array<float, 13> v = { m.dst.x,  m.dst.y,  m.dst.w,  m.dst.h,  m.uv.x,   m.uv.y,  m.uv.w,
                                              m.uv.h,   m.radius, m.clip.x, m.clip.y, m.clip.w, m.clip.h };
            for (const float x : v)
                mix32(geometry, static_cast<uint32_t>(quantise(x, quantum)));
            ++fp.images;
            fp.maxX = std::max(fp.maxX, m.dst.right());
            fp.maxY = std::max(fp.maxY, m.dst.bottom());
            countTag(fp.tagCounts, m.tag);
        }
        fp.geometry = geometry;
        fp.text = text;
        return fp;
    }

    void addMetrics(test::Probe& P, std::string_view prefix, const Fingerprint& fp)
    {
        namespace T = funkgui::test;
        const std::string pre(prefix);
        P.hash(pre + ".geometry", fp.geometry);
        P.hash(pre + ".text", fp.text);
        P.num(pre + ".statics", fp.statics, T::Tol::exact());
        P.num(pre + ".live", fp.live, T::Tol::exact());
        P.num(pre + ".texts", fp.texts, T::Tol::exact());
        P.num(pre + ".rrects", fp.rrects, T::Tol::exact());
        P.num(pre + ".segments", fp.segments, T::Tol::exact());
        P.num(pre + ".areas", fp.areas, T::Tol::exact());
        P.num(pre + ".max_x", static_cast<double>(fp.maxX), T::Tol::abs(0.01));
        P.num(pre + ".max_y", static_cast<double>(fp.maxY), T::Tol::abs(0.01));
        if (fp.shaders != 0)                             // v0.19.0: only a frame with shader draws has the row
            P.num(pre + ".shaders", fp.shaders, T::Tol::exact());
        if (fp.images != 0)                              // v0.20.0: only a frame with image draws has the row
            P.num(pre + ".images", fp.images, T::Tol::exact());
        for (const auto& [tag, count] : fp.tagCounts)
        {
            // Golden keys are lower case: SLOT_LABEL -> <prefix>.tag.slot_label; an unnamed tag by its number.
            std::string name;
            if (const char* n = tagName(tag))
                for (const char* c = n; *c != 0; ++c)
                    name += (*c >= 'A' && *c <= 'Z') ? static_cast<char>(*c - 'A' + 'a') : *c;
            else
                name = std::to_string(static_cast<unsigned>(tag));
            P.num(pre + ".tag." + name, count, T::Tol::exact());
        }
    }
}

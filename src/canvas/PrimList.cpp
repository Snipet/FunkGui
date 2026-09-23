#include <funkgui/canvas/PrimList.h>

#include <funkgui/canvas/Tags.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <xlocale.h>                                     // after <cstdio>/<cstdlib>: fprintf_l, strtof_l (macOS)

// Dump v2 (02 §3.8), a superset of the snapshot's SdfCanvas::end() dump (v1, gpu/SdfCanvas.cpp:290-319), and the tag
// name registry (canvas/Tags.h) it spells tags with.
//
//   funkgui-dump 2
//   clear 16171a
//   view 960 640 dpi 2 theme 0 frame 61 dt 0.0166666675 clock fixed fps 0.0 rate full seconds 1.0166 gamma 0.714285731
//   glyphs missing 0
//   overflow 3                                            (only when FrameInfo::overflows > 0)
//   axis LEVEL x 556 748 -42 6 lin y 332 140 -42 6 lin    (one per AxisRec; "-" for an absent axis)
//   p 38.5 38.5 57.5 49.5  c0 ffe4eaec c1 ffe4eaec  d0 -9.5 -5.5 8 4  e0 9.5 5.5  d1 0 0 0 0  d2 0 0 0 0  t SLOT_LABEL
//
// Choices where 02 §3.8 is silent (G3):
// - Floats are "%.9g" everywhere (an exact float round trip) except fps, which keeps HR's "%.1f" (a measurement).
// - The view line also carries "seconds S gamma G" (FrameInfo::seconds, ::textGamma), so a v2 dump holds the whole
//   FrameInfo; HR's sscanf("view %f %f dpi %f") reads the unchanged prefix. The clock is fixed, displaylink or timer.
// - A tag is written by name, or as its decimal number when it has none (an unregistered product tag). A parsed name
//   that matches no FunkGui or registered name becomes tag 0: names are metadata, and a FunkGui tool must still read a
//   product's dump. A malformed name ([A-Z0-9_]+ is the grammar) is a parse error.
// - An undecodable UTF-8 sequence is listed among the missing codepoints as U+FFFD (Canvas).
// - Numbers are written and read in the C locale whatever the process locale is.
// - parseText is strict: a v2 dump must start with "funkgui-dump 2"; without that line it is read as HR's v1 (clear,
//   view and p lines only). Unknown keywords, fields or view keys, a p line with other than 20 fields (+ "t NAME"), a
//   repeated header line, or a missing clear or view line make it return false (with `out` cleared).

namespace funkgui
{
    // ---- tag names --------------------------------------------------------------------------------------------------

    namespace
    {
        struct TagRegistry
        {
            std::vector<TagName> names;                  // product tags (>= tags::firstProduct), in registration order
        };

        TagRegistry& registry()
        {
            // Never destroyed (HR idiom): a plug-in binary is unloaded while statics are torn down in an order nothing
            // here controls.
            static TagRegistry* const r = new TagRegistry();
            return *r;
        }

        bool validTagName(std::string_view s) noexcept
        {
            if (s.empty())
                return false;
            for (const char c : s)
                if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
                    return false;
            return true;
        }
    }

    // Ignored (so that every dump name reads back as the tag it was written for): a tag in FunkGui's range, a null or
    // unspellable name, and a name FunkGui or another product tag already uses.
    void registerTagNames(std::span<const TagName> names)
    {
        auto& reg = registry().names;
        for (const TagName& n : names)
        {
            if (n.tag < tags::firstProduct || n.name == nullptr || !validTagName(n.name))
                continue;
            const Tag owner = tagFromName(n.name);
            if (owner != tags::none && owner != n.tag)
                continue;
            bool replaced = false;
            for (TagName& have : reg)
                if (have.tag == n.tag)
                {
                    have.name = n.name;
                    replaced = true;
                }
            if (!replaced)
                reg.push_back(n);
        }
    }

    const char* tagName(Tag t)
    {
        if (t == tags::none)
            return nullptr;
        if (t <= tags::lastFunkGui)
        {
            for (const TagName& n : kFunkGuiTagNames)
                if (n.tag == t)
                    return n.name;
            return nullptr;
        }
        for (const TagName& n : registry().names)
            if (n.tag == t)
                return n.name;
        return nullptr;
    }

    Tag tagFromName(const char* name)
    {
        if (name == nullptr || *name == 0)
            return tags::none;
        for (const TagName& n : kFunkGuiTagNames)
            if (std::strcmp(n.name, name) == 0)
                return n.tag;
        for (const TagName& n : registry().names)
            if (std::strcmp(n.name, name) == 0)
                return n.tag;
        return tags::none;
    }

    // ---- PrimList ---------------------------------------------------------------------------------------------------

    void PrimList::clear()
    {
        info = FrameInfo{};
        prims.clear();
        axes.clear();
        missingGlyphs = 0;
        std::fill(std::begin(missingFirst), std::end(missingFirst), 0u);
    }

    namespace
    {
        // The C locale, for number formatting and parsing that no setlocale() in the host can change: the *_l
        // functions take a null locale_t as the C locale (xlocale(3)).
        constexpr locale_t kCLocale = nullptr;

        const char* clockName(const FrameInfo& i) noexcept
        {
            return i.fixedClock ? "fixed" : (i.displayLinked ? "displaylink" : "timer");
        }

        double dbl(float v) noexcept { return static_cast<double>(v); }

        // A tag as the dump spells it: its name, or its decimal number.
        std::string tagText(Tag t)
        {
            if (const char* n = tagName(t))
                return n;
            return std::to_string(static_cast<unsigned>(t));
        }

        bool writeAxis(std::FILE* f, locale_t loc, const char* axisName, bool has, const AxisMap& m)
        {
            if (!has)
                return fprintf_l(f, loc, " %s -", axisName) >= 0;
            return fprintf_l(f, loc, " %s %.9g %.9g %.9g %.9g %s", axisName, dbl(m.px0), dbl(m.px1), dbl(m.v0),
                             dbl(m.v1), m.log ? "log" : "lin") >= 0;
        }
    }

    bool PrimList::writeText(std::FILE* f) const
    {
        if (f == nullptr)
            return false;
        const locale_t loc = kCLocale;
        bool ok = true;
        const auto put = [&ok](int rc) { ok = ok && rc >= 0; };

        put(fprintf_l(f, loc, "funkgui-dump 2\n"));
        put(fprintf_l(f, loc, "clear %02x%02x%02x\n", info.clear.r, info.clear.g, info.clear.b));
        put(fprintf_l(f, loc,
                      "view %d %d dpi %.9g theme %d frame %u dt %.9g clock %s fps %.1f rate %s seconds %.9g "
                      "gamma %.9g\n",
                      info.logicalW, info.logicalH, dbl(info.dpi), info.theme, static_cast<unsigned>(info.frame),
                      dbl(info.dt), clockName(info), dbl(info.fps), info.fullRate ? "full" : "idle", dbl(info.seconds),
                      dbl(info.textGamma)));

        put(fprintf_l(f, loc, "glyphs missing %u", static_cast<unsigned>(missingGlyphs)));
        for (const uint32_t cp : missingFirst)
            if (cp != 0)
                put(fprintf_l(f, loc, " U+%04X", static_cast<unsigned>(cp)));
        put(fprintf_l(f, loc, "\n"));

        if (info.overflows > 0)
            put(fprintf_l(f, loc, "overflow %u\n", static_cast<unsigned>(info.overflows)));

        for (const AxisRec& a : axes)
        {
            put(fprintf_l(f, loc, "axis %s", tagText(a.tag).c_str()));
            ok = ok && writeAxis(f, loc, "x", a.hasX, a.x) && writeAxis(f, loc, "y", a.hasY, a.y);
            put(fprintf_l(f, loc, "\n"));
        }

        for (const Prim& p : prims)
        {
            put(fprintf_l(f, loc,
                          "p %.9g %.9g %.9g %.9g  c0 %08x c1 %08x  d0 %.9g %.9g %.9g %.9g  e0 %.9g %.9g  "
                          "d1 %.9g %.9g %.9g %.9g  d2 %.9g %.9g %.9g %.9g",
                          dbl(p.x0), dbl(p.y0), dbl(p.x1), dbl(p.y1), static_cast<unsigned>(p.c0),
                          static_cast<unsigned>(p.c1), dbl(p.d0[0]), dbl(p.d0[1]), dbl(p.d0[2]), dbl(p.d0[3]),
                          dbl(p.e0[0]), dbl(p.e0[1]), dbl(p.d1[0]), dbl(p.d1[1]), dbl(p.d1[2]), dbl(p.d1[3]),
                          dbl(p.d2[0]), dbl(p.d2[1]), dbl(p.d2[2]), dbl(p.d2[3])));
            if (p.tag != tags::none)
                put(fprintf_l(f, loc, "  t %s", tagText(p.tag).c_str()));
            put(fprintf_l(f, loc, "\n"));
            if (!ok)
                return false;
        }
        return ok && std::fflush(f) == 0 && std::ferror(f) == 0;
    }

    // ---- parser -----------------------------------------------------------------------------------------------------

    namespace
    {
        bool isSpace(char c) noexcept { return c == ' ' || c == '\t'; }

        // The whitespace-separated tokens of one line.
        void tokenize(std::string_view line, std::vector<std::string_view>& out)
        {
            out.clear();
            size_t i = 0;
            while (i < line.size())
            {
                while (i < line.size() && isSpace(line[i]))
                    ++i;
                const size_t b = i;
                while (i < line.size() && !isSpace(line[i]))
                    ++i;
                if (i > b)
                    out.push_back(line.substr(b, i - b));
            }
        }

        // A token as a float, the whole token, in the C locale (strtof's grammar: "nan", "inf" and hex floats too).
        bool toFloat(std::string_view t, float& v)
        {
            char buf[64];
            if (t.empty() || t.size() >= sizeof buf || isSpace(t.front()))
                return false;
            std::memcpy(buf, t.data(), t.size());
            buf[t.size()] = 0;
            char* end = nullptr;
            v = strtof_l(buf, &end, kCLocale);
            return end == buf + t.size();
        }

        // An unsigned decimal of at most `maxDigits` digits (no sign, no spaces).
        bool toUnsigned(std::string_view t, uint64_t& v, size_t maxDigits = 10)
        {
            if (t.empty() || t.size() > maxDigits)
                return false;
            v = 0;
            for (const char c : t)
            {
                if (c < '0' || c > '9')
                    return false;
                v = v * 10u + static_cast<uint64_t>(c - '0');
            }
            return true;
        }

        bool toInt(std::string_view t, int& v)
        {
            bool neg = false;
            if (!t.empty() && t.front() == '-')
            {
                neg = true;
                t.remove_prefix(1);
            }
            uint64_t u = 0;
            if (!toUnsigned(t, u, 10) || u > 2147483647u)
                return false;
            v = neg ? -static_cast<int>(u) : static_cast<int>(u);
            return true;
        }

        bool toU32(std::string_view t, uint32_t& v)
        {
            uint64_t u = 0;
            if (!toUnsigned(t, u, 10) || u > 0xFFFFFFFFu)
                return false;
            v = static_cast<uint32_t>(u);
            return true;
        }

        // 1..maxDigits hex digits, nothing else.
        bool toHex(std::string_view t, uint32_t& v, size_t maxDigits)
        {
            if (t.empty() || t.size() > maxDigits)
                return false;
            v = 0;
            for (const char c : t)
            {
                uint32_t n = 0;
                if (c >= '0' && c <= '9')      n = static_cast<uint32_t>(c - '0');
                else if (c >= 'a' && c <= 'f') n = static_cast<uint32_t>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') n = static_cast<uint32_t>(c - 'A' + 10);
                else return false;
                v = (v << 4) | n;
            }
            return true;
        }

        // A logical size: v2 writes "%d", HR's v1 "%g" of a float holding an integer.
        bool toSize(std::string_view t, int& v)
        {
            float f = 0.0f;
            if (!toFloat(t, f) || !(f >= 0.0f) || !(f <= 1.0e6f) || std::floor(f) < f)
                return false;
            v = static_cast<int>(f);
            return true;
        }

        // "NAME" or a decimal number. An unknown well-formed name is tag 0 (see the file comment).
        bool toTag(std::string_view t, Tag& tag)
        {
            if (!t.empty() && t.front() >= '0' && t.front() <= '9')
            {
                uint64_t u = 0;
                if (!toUnsigned(t, u, 5) || u > 0xFFFFu)
                    return false;
                tag = static_cast<Tag>(u);
                return true;
            }
            if (!validTagName(t))
                return false;
            const std::string name(t);
            tag = tagFromName(name.c_str());
            return true;
        }

        bool parseView(const std::vector<std::string_view>& tk, FrameInfo& info)
        {
            if (tk.size() < 3 || !toSize(tk[1], info.logicalW) || !toSize(tk[2], info.logicalH))
                return false;
            bool haveDpi = false;
            uint32_t seen = 0;                           // one bit per key: each at most once
            for (size_t i = 3; i < tk.size(); i += 2)
            {
                if (i + 1 >= tk.size())
                    return false;
                const std::string_view k = tk[i], v = tk[i + 1];
                const auto once = [&seen](uint32_t bit)
                {
                    const bool firstTime = (seen & bit) == 0;
                    seen |= bit;
                    return firstTime;
                };
                if (k == "dpi")
                {
                    if (!once(1u << 0) || !toFloat(v, info.dpi))
                        return false;
                    haveDpi = true;
                }
                else if (k == "theme")   { if (!once(1u << 1) || !toInt(v, info.theme)) return false; }
                else if (k == "frame")   { if (!once(1u << 2) || !toU32(v, info.frame)) return false; }
                else if (k == "dt")      { if (!once(1u << 3) || !toFloat(v, info.dt)) return false; }
                else if (k == "fps")     { if (!once(1u << 4) || !toFloat(v, info.fps)) return false; }
                else if (k == "seconds") { if (!once(1u << 5) || !toFloat(v, info.seconds)) return false; }
                else if (k == "gamma")   { if (!once(1u << 6) || !toFloat(v, info.textGamma)) return false; }
                else if (k == "clock")
                {
                    if (!once(1u << 7))
                        return false;
                    if (v == "fixed")            { info.fixedClock = true;  info.displayLinked = false; }
                    else if (v == "displaylink") { info.fixedClock = false; info.displayLinked = true; }
                    else if (v == "timer")       { info.fixedClock = false; info.displayLinked = false; }
                    else return false;
                }
                else if (k == "rate")
                {
                    if (!once(1u << 8))
                        return false;
                    if (v == "full")      info.fullRate = true;
                    else if (v == "idle") info.fullRate = false;
                    else return false;
                }
                else
                    return false;
            }
            return haveDpi;
        }

        bool parseAxisPart(const std::vector<std::string_view>& tk, size_t& i, std::string_view name, bool& has,
                           AxisMap& m)
        {
            if (i >= tk.size() || tk[i] != name)
                return false;
            ++i;
            if (i < tk.size() && tk[i] == "-")
            {
                has = false;
                m = AxisMap{};
                ++i;
                return true;
            }
            if (i + 5 > tk.size() || !toFloat(tk[i], m.px0) || !toFloat(tk[i + 1], m.px1) || !toFloat(tk[i + 2], m.v0)
                || !toFloat(tk[i + 3], m.v1))
                return false;
            if (tk[i + 4] == "log")      m.log = true;
            else if (tk[i + 4] == "lin") m.log = false;
            else return false;
            has = true;
            i += 5;
            return true;
        }

        bool parseAxis(const std::vector<std::string_view>& tk, AxisRec& a)
        {
            if (tk.size() < 2 || !toTag(tk[1], a.tag))
                return false;
            size_t i = 2;
            return parseAxisPart(tk, i, "x", a.hasX, a.x) && parseAxisPart(tk, i, "y", a.hasY, a.y) && i == tk.size();
        }

        bool parsePrim(const std::vector<std::string_view>& tk, Prim& p)
        {
            // p x0 y0 x1 y1 c0 H c1 H d0 a b c d e0 a b d1 a b c d d2 a b c d [t NAME]: 27 or 29 tokens.
            if (tk.size() != 27 && tk.size() != 29)
                return false;
            p = Prim{};
            const auto floats = [&tk](size_t at, float* v, int n)
            {
                for (int k = 0; k < n; ++k)
                    if (!toFloat(tk[at + static_cast<size_t>(k)], v[k]))
                        return false;
                return true;
            };
            float xy[4] = {};
            if (!floats(1, xy, 4) || tk[5] != "c0" || !toHex(tk[6], p.c0, 8) || tk[7] != "c1" || !toHex(tk[8], p.c1, 8)
                || tk[9] != "d0" || !floats(10, p.d0, 4) || tk[14] != "e0" || !floats(15, p.e0, 2) || tk[17] != "d1"
                || !floats(18, p.d1, 4) || tk[22] != "d2" || !floats(23, p.d2, 4))
                return false;
            p.x0 = xy[0];
            p.y0 = xy[1];
            p.x1 = xy[2];
            p.y1 = xy[3];
            if (tk.size() == 29 && (tk[27] != "t" || !toTag(tk[28], p.tag)))
                return false;
            return true;
        }

        bool parseGlyphs(const std::vector<std::string_view>& tk, PrimList& out)
        {
            if (tk.size() < 3 || tk.size() > 3 + std::size(out.missingFirst) || tk[1] != "missing"
                || !toU32(tk[2], out.missingGlyphs))
                return false;
            for (size_t i = 3; i < tk.size(); ++i)
            {
                const std::string_view u = tk[i];
                uint32_t cp = 0;
                if (u.size() < 6 || u.substr(0, 2) != "U+" || !toHex(u.substr(2), cp, 6) || cp == 0 || cp > 0x10FFFFu)
                    return false;
                out.missingFirst[i - 3] = cp;
            }
            return true;
        }

        bool parseLines(std::string_view text, PrimList& out)
        {
            enum : uint32_t { kClear = 1u << 0, kView = 1u << 1, kGlyphs = 1u << 2, kOverflow = 1u << 3 };
            uint32_t seen = 0;
            bool v2 = false, first = true;
            std::vector<std::string_view> tk;
            tk.reserve(32);

            size_t pos = 0;
            while (pos <= text.size())
            {
                size_t nl = text.find('\n', pos);
                if (nl == std::string_view::npos)
                    nl = text.size();
                std::string_view line = text.substr(pos, nl - pos);
                pos = nl + 1;
                if (!line.empty() && line.back() == '\r')
                    line.remove_suffix(1);
                tokenize(line, tk);
                if (tk.empty())
                    continue;

                const std::string_view k = tk[0];
                if (first)
                {
                    first = false;
                    if (k == "funkgui-dump")
                    {
                        if (tk.size() != 2 || tk[1] != "2")
                            return false;                // another version
                        v2 = true;
                        continue;
                    }
                }
                if (k == "p")
                {
                    Prim p;
                    if (!parsePrim(tk, p))
                        return false;
                    out.prims.push_back(p);
                }
                else if (k == "clear")
                {
                    uint32_t rgb = 0;
                    if ((seen & kClear) != 0 || tk.size() != 2 || tk[1].size() != 6 || !toHex(tk[1], rgb, 6))
                        return false;
                    seen |= kClear;
                    out.info.clear = Col{ static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>((rgb >> 8) & 0xFFu),
                                          static_cast<uint8_t>(rgb & 0xFFu), 255 };
                }
                else if (k == "view")
                {
                    if ((seen & kView) != 0 || !parseView(tk, out.info))
                        return false;
                    seen |= kView;
                }
                else if (v2 && k == "glyphs")
                {
                    if ((seen & kGlyphs) != 0 || !parseGlyphs(tk, out))
                        return false;
                    seen |= kGlyphs;
                }
                else if (v2 && k == "overflow")
                {
                    if ((seen & kOverflow) != 0 || tk.size() != 2 || !toU32(tk[1], out.info.overflows))
                        return false;
                    seen |= kOverflow;
                }
                else if (v2 && k == "axis")
                {
                    AxisRec a;
                    if (!parseAxis(tk, a))
                        return false;
                    out.axes.push_back(a);
                }
                else
                    return false;
            }
            return (seen & kClear) != 0 && (seen & kView) != 0;
        }
    }

    bool PrimList::parseText(std::string_view text, PrimList& out)
    {
        out.clear();
        if (parseLines(text, out))
            return true;
        out.clear();
        return false;
    }
}

// funkgui_framerender: a frame dump (PrimList::writeText, dump v2, or HR's v1) as a PNG, or as its fingerprint
// (FCompressor docs/design/02-funkgui-and-ui.md §4.4; 03 §3.6). A thin CLI over PrimList::parseText, canvas/SoftRaster
// (the CPU mirror of shaders/fs_ui.sc, AREA included) and canvas/Fingerprint: this tool holds no rasteriser and no
// hash of its own, so what it shows and hashes is what the library's probes show and hash.
//
//   funkgui_framerender <dump> <out.png> [ss]
//       Renders the frame at its physical size (the dump's view size * dpi), ss x ss samples per pixel (1..4,
//       default 2), over the bundled face's atlas. How agents look at a frame (03 §4.7 item 5).
//
//   funkgui_framerender --fingerprint <dump> [--legacy-hr]
//       Prints the fingerprint, one "<key> <value>" line each: geometry, text (hashes), statics, live, texts, rrects,
//       segments, areas, max_x, max_y, tag.<name>, view_w, view_h, glyphs_missing (FingerprintOptions defaults: live
//       primitives, colours and text gamma left out). Live parity (03 §3.6) compares its geometry line with the
//       headless probe's hash of the same state. --legacy-hr prints HR's layout.* metrics instead (below).
//
//   funkgui_framerender --fingerprint <dump> [--legacy-hr] --check <probe> --golden-root <dir> --arch arm64|x86_64
//                       [--bless-to <dir>] [--results <dir>] [--only <glob>] [--verbose]
//       The same numbers as golden rows of the Harness v2 probe <probe> (global scope; there is no --bless, 03 §3.2):
//       frame.* (Fingerprint.h addMetrics, plus frame.view_w/h), or with --legacy-hr HR's nine rows under HR's own
//       keys (layout.geometry, static_count, text_count, rank_strokes, segments, view_w, view_h, max_x, max_y), so
//       HR's framerender goldens carry over to its migration gate (SPRINTS §5). Spec rows: the dump reads and parses,
//       it holds at least one primitive, and (v2 only) no glyph is missing.
//
// --legacy-hr is HR's FrameRender.cpp:108-150 exactly (FingerprintOptions::legacyHr; 02 §3.9): every float taken
// through "%g", raw bits hashed, the Rank display's energy caps (non-text, non-segment primitives inside y 165..295
// with an SDF half-height <= 2) skipped, and rank_strokes counting the other primitives in that band. HR's hard-coded
// band exists only here; the v2 fingerprint leaves out what the live flag marks (02 §4.4).
//
// Exit codes: 0 done; 1 the dump cannot be read or parsed, is empty, the atlas does not bake or the PNG cannot be
// written; 2 usage. With --check the probe's own codes (03 §3.2.4: 0 pass, 1 spec_fail, 2 golden_drift,
// 3 golden_missing, 4 harness_error), and a usage error is 4.

#include <funkgui/canvas/Fingerprint.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/canvas/SoftRaster.h>
#include <funkgui/canvas/Tags.h>
#include <funkgui/test/Harness.h>
#include <funkgui/text/FontService.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <xlocale.h>                                     // after <cstdio>/<cstdlib>: snprintf_l, strtof_l (macOS)

namespace T = funkgui::test;

namespace
{
    constexpr const char* kUsage =
        "usage: funkgui_framerender <dump> <out.png> [ss 1-4, default 2]\n"
        "       funkgui_framerender --fingerprint <dump> [--legacy-hr]\n"
        "       funkgui_framerender --fingerprint <dump> [--legacy-hr] --check <probe> --golden-root <dir> "
        "--arch arm64|x86_64 [--bless-to <dir>] [--results <dir>] [--only <glob>] [--verbose]\n";

    // The whole file, or false when it cannot be read.
    bool readFile(const std::string& path, std::string& out)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f)
            return false;
        std::ostringstream ss;
        ss << f.rdbuf();
        out = ss.str();
        return !f.bad();
    }

    // A float as HR's v1 dump held it: printed with "%g" and read back (Fingerprint.cpp's legacy rule, C locale).
    float throughG(float v)
    {
        constexpr locale_t kCLocale = nullptr;
        char buf[64];
        snprintf_l(buf, sizeof buf, kCLocale, "%g", static_cast<double>(v));
        return strtof_l(buf, nullptr, kCLocale);
    }

    // HR's rank_strokes (FrameRender.cpp:118-128): non-text, non-segment primitives inside the Rank band y 165..295
    // that are not energy caps (SDF half-height <= 2). The one HR metric Fingerprint does not carry.
    int legacyRankStrokes(const funkgui::PrimList& l)
    {
        int rank = 0;
        for (const funkgui::Prim& p : l.prims)
        {
            const float y0 = throughG(p.y0), y1 = throughG(p.y1), hh = throughG(p.d0[3]), kind = throughG(p.d2[2]);
            const bool isText = kind > 0.5f && kind < 1.5f;
            const bool isSeg = kind > 1.5f;
            const bool inRank = y0 >= 165.0f && y1 <= 295.0f && !isText && !isSeg;
            if (inRank && !(hh <= 2.0f))
                ++rank;
        }
        return rank;
    }

    // Golden-key spelling of a tag: SLOT_LABEL -> slot_label; an unnamed tag by its number (as addMetrics).
    std::string tagKey(funkgui::Tag t)
    {
        std::string name;
        if (const char* n = funkgui::tagName(t))
            for (const char* c = n; *c != 0; ++c)
                name += (*c >= 'A' && *c <= 'Z') ? static_cast<char>(*c - 'A' + 'a') : *c;
        else
            name = std::to_string(static_cast<unsigned>(t));
        return name;
    }

    void printFingerprint(const funkgui::PrimList& l, const funkgui::Fingerprint& fp)
    {
        std::printf("geometry %016llx\n", static_cast<unsigned long long>(fp.geometry));
        std::printf("text %016llx\n", static_cast<unsigned long long>(fp.text));
        std::printf("statics %d\nlive %d\ntexts %d\nrrects %d\nsegments %d\nareas %d\n", fp.statics, fp.live,
                    fp.texts, fp.rrects, fp.segments, fp.areas);
        std::printf("max_x %.9g\nmax_y %.9g\n", static_cast<double>(fp.maxX), static_cast<double>(fp.maxY));
        for (const auto& [tag, count] : fp.tagCounts)
            std::printf("tag.%s %d\n", tagKey(tag).c_str(), count);
        std::printf("view_w %d\nview_h %d\nglyphs_missing %u\n", l.info.logicalW, l.info.logicalH, l.missingGlyphs);
    }

    void printLegacy(const funkgui::PrimList& l, const funkgui::Fingerprint& fp, int rank)
    {
        std::printf("layout.geometry %016llx\n", static_cast<unsigned long long>(fp.geometry));
        std::printf("layout.static_count %d\nlayout.text_count %d\nlayout.rank_strokes %d\nlayout.segments %d\n",
                    fp.statics, fp.texts, rank, fp.segments);
        std::printf("layout.view_w %d\nlayout.view_h %d\n", l.info.logicalW, l.info.logicalH);
        std::printf("layout.max_x %.9g\nlayout.max_y %.9g\n", static_cast<double>(fp.maxX),
                    static_cast<double>(fp.maxY));
        std::printf("live_caps_excluded %d\n", fp.live);
    }

    // --fingerprint [--check]: the dump's fingerprint, printed or as a probe's rows.
    int fingerprintMode(const std::string& dumpPath, bool legacy, const std::string& probe, int argc, char** argv)
    {
        std::string text;
        const bool readable = readFile(dumpPath, text);
        funkgui::PrimList l;
        const bool parsed = readable && funkgui::PrimList::parseText(text, l);
        if (!parsed)
            std::fprintf(stderr, "funkgui_framerender: %s %s\n", readable ? "cannot parse" : "cannot read",
                         dumpPath.c_str());

        funkgui::FingerprintOptions opt;
        opt.legacyHr = legacy;
        const funkgui::Fingerprint fp = funkgui::fingerprint(l, opt);
        const int rank = legacy ? legacyRankStrokes(l) : 0;
        if (parsed)
            std::printf("view %d x %d dpi %.9g, %zu primitives, %zu axes, %u missing glyphs\n", l.info.logicalW,
                        l.info.logicalH, static_cast<double>(l.info.dpi), l.prims.size(), l.axes.size(),
                        l.missingGlyphs);

        if (probe.empty())
        {
            if (!parsed || l.prims.empty())
                return 1;
            if (legacy)
                printLegacy(l, fp, rank);
            else
                printFingerprint(l, fp);
            return 0;
        }

        T::Probe P(probe, "", argc, argv);
        P.eq("dump.readable", readable, 1);
        P.eq("dump.parsed", parsed, 1);
        if (!P.ge("dump.primitives", static_cast<double>(l.prims.size()), 1))
            return P.finish();
        if (legacy)
        {
            P.hash("layout.geometry", fp.geometry);
            P.num("layout.static_count", fp.statics, T::Tol::abs(0));
            P.num("layout.text_count", fp.texts, T::Tol::abs(0));
            P.num("layout.rank_strokes", rank, T::Tol::abs(0));
            P.num("layout.segments", fp.segments, T::Tol::abs(0));
            P.num("layout.view_w", l.info.logicalW, T::Tol::abs(0));
            P.num("layout.view_h", l.info.logicalH, T::Tol::abs(0));
            P.num("layout.max_x", static_cast<double>(fp.maxX), T::Tol::abs(0.01));
            P.num("layout.max_y", static_cast<double>(fp.maxY), T::Tol::abs(0.01));
            std::printf("geometry %016llx  static %d (text %d, rank strokes %d)  live caps excluded %d  "
                        "extent %.1f x %.1f\n", static_cast<unsigned long long>(fp.geometry), fp.statics, fp.texts,
                        rank, fp.live, static_cast<double>(fp.maxX), static_cast<double>(fp.maxY));
        }
        else
        {
            P.eq("frame.glyphs_missing", l.missingGlyphs, 0);
            funkgui::addMetrics(P, "frame", fp);
            P.num("frame.view_w", l.info.logicalW, T::Tol::exact());
            P.num("frame.view_h", l.info.logicalH, T::Tol::exact());
            std::printf("geometry %016llx  text %016llx  statics %d (text %d, areas %d)  live %d  extent %.2f x %.2f\n",
                        static_cast<unsigned long long>(fp.geometry), static_cast<unsigned long long>(fp.text),
                        fp.statics, fp.texts, fp.areas, fp.live, static_cast<double>(fp.maxX),
                        static_cast<double>(fp.maxY));
        }
        return P.finish();
    }

    // <dump> <out.png> [ss]: the frame through SoftRaster.
    int renderMode(const std::string& dumpPath, const std::string& pngPath, int ss)
    {
        std::string text;
        if (!readFile(dumpPath, text))
        {
            std::fprintf(stderr, "funkgui_framerender: cannot read %s\n", dumpPath.c_str());
            return 1;
        }
        funkgui::PrimList l;
        if (!funkgui::PrimList::parseText(text, l))
        {
            std::fprintf(stderr, "funkgui_framerender: cannot parse %s (not a dump v1/v2)\n", dumpPath.c_str());
            return 1;
        }
        std::printf("view %d x %d dpi %.9g, %zu primitives, %u missing glyphs\n", l.info.logicalW, l.info.logicalH,
                    static_cast<double>(l.info.dpi), l.prims.size(), l.missingGlyphs);
        if (l.prims.empty())
        {
            std::fprintf(stderr, "funkgui_framerender: %s holds no primitive\n", dumpPath.c_str());
            return 1;
        }

        auto& fonts = funkgui::FontService::get();
        const funkgui::FontAtlasSdf& atlas = fonts.atlas();
        if (!fonts.ok())
        {
            std::fprintf(stderr, "funkgui_framerender: the bundled face did not bake\n");
            return 1;
        }
        const funkgui::Image img = funkgui::rasterise(l, atlas, ss);
        if (img.w <= 0 || img.h <= 0)
        {
            std::fprintf(stderr, "funkgui_framerender: the frame has no drawable size\n");
            return 1;
        }
        if (!funkgui::writePng(img, pngPath.c_str()))
        {
            std::fprintf(stderr, "funkgui_framerender: cannot write %s\n", pngPath.c_str());
            return 1;
        }
        std::printf("wrote %s (%d x %d, ss %d)\n", pngPath.c_str(), img.w, img.h, ss);
        return 0;
    }

    bool parseSupersample(std::string_view s, int& out)
    {
        if (s.size() != 1 || s[0] < '1' || s[0] > '4')
            return false;
        out = s[0] - '0';
        return true;
    }
}

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;      // the atlas bakes through JUCE's font stack

    // The tool's own flags come out first; with --check, what is left goes to the Probe (whose flags they are).
    std::string dump, probe;
    bool fingerprint = false, legacy = false, check = false, usageError = false;
    std::vector<char*> rest;
    rest.push_back(argc > 0 ? argv[0] : nullptr);
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view a = argv[i] != nullptr ? argv[i] : "";
        if (a == "--fingerprint" || a == "--check")
        {
            if (i + 1 >= argc || argv[i + 1] == nullptr || argv[i + 1][0] == '\0'
                || std::string_view(argv[i + 1]).starts_with("--"))
            {
                std::fprintf(stderr, "funkgui_framerender: %s needs a value\n", argv[i]);
                usageError = true;
                continue;
            }
            (a == "--fingerprint" ? dump : probe) = argv[++i];
            (a == "--fingerprint" ? fingerprint : check) = true;
            continue;
        }
        if (a == "--legacy-hr")
        {
            legacy = true;
            continue;
        }
        rest.push_back(argv[i]);
    }

    if (check)
    {
        const int n = static_cast<int>(rest.size());
        rest.push_back(nullptr);
        if (usageError || !fingerprint || !T::positionals(n, rest.data()).empty())
        {
            std::fprintf(stderr, "%s", kUsage);
            return 4;
        }
        return fingerprintMode(dump, legacy, probe, n, rest.data());
    }

    // Without --check every remaining argument is positional.
    std::vector<std::string> positional;
    for (size_t i = 1; i < rest.size(); ++i)
    {
        if (std::string_view(rest[i]).starts_with("--"))
        {
            std::fprintf(stderr, "funkgui_framerender: unknown option %s\n", rest[i]);
            usageError = true;
        }
        positional.emplace_back(rest[i]);
    }
    if (usageError)
    {
        std::fprintf(stderr, "%s", kUsage);
        return 2;
    }
    if (fingerprint)
    {
        if (!positional.empty())
        {
            std::fprintf(stderr, "%s", kUsage);
            return 2;
        }
        return fingerprintMode(dump, legacy, "", 0, nullptr);
    }
    int ss = 2;
    if (legacy || positional.size() < 2 || positional.size() > 3
        || (positional.size() == 3 && !parseSupersample(positional[2], ss)))
    {
        std::fprintf(stderr, "%s", kUsage);
        return 2;
    }
    return renderMode(positional[0], positional[1], ss);
}

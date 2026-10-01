// GalleryProbe: one widget-gallery section (test/gallery/GalleryPanel.h) run headless, as the test fg.gallery.<section>
// (FCompressor docs/design/02-funkgui-and-ui.md §3.11; SPRINTS.md §7 D10). Every test/gallery/*.cpp is compiled into
// this tool, and each section file registers its own test, so a widget card adds fg.gallery.<section> without touching
// another section's goldens.
//
//   FunkGuiGalleryProbe <probe> [--section <name>] [--dump-dir <dir>] --golden-root <dir> --arch arm64|x86_64|wasm32
//                       [--bless-to <dir>] [--results <dir>] [--only <glob>] [--verbose]
//   FunkGuiGalleryProbe --list
//
// The section is --section's, or the last part of a probe named fg.gallery.<section>. --dump-dir writes every state's
// settled frame as dump v2, <dir>/<section>.<state>.dpi<d>.dump (theme 0), for `funkgui_framerender <dump> <png>`.
// Both flags are the tool's own and are taken out of argv before the harness sees it.
//
// Per state (a fresh GalleryPanel and HeadlessHost each time; settled before and after the state's script, at most
// 600 frames of 1/60 s, else a harness error), at dpi 1 and 2, theme 0 and 1:
//   golden rows   <state>.dpi<d>.*        the theme-0 fingerprint (Fingerprint.h addMetrics)
//                 a11y.<state>            the visible a11y items as a11yDumpLine lines
//   spec rows     <state>.dpi<d>.glyphs_missing = 0, .dump_roundtrip (write -> parse -> bit-equal primitives, frame
//                 info and axes, equal fingerprint), .theme_invariant (theme 1's fingerprint equals theme 0's), and
//                 <state>.a11y_invariant (the same lines at every dpi and theme)
// plus, once: font.ok (the atlas baked from the bundled face) and gallery.names (legal, unique section and state
// names).

#include "../test/gallery/GalleryPanel.h"

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Fingerprint.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/test/Harness.h>
#include <funkgui/text/FontService.h>

#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace T = funkgui::test;
namespace G = funkgui::gallery;

namespace
{
    constexpr int   kMaxSettle = 600;
    constexpr float kDt = 1.0f / 60.0f;

    bool sameBits(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }

    bool sameInfo(const funkgui::FrameInfo& a, const funkgui::FrameInfo& b)
    {
        return a.logicalW == b.logicalW && a.logicalH == b.logicalH && sameBits(a.dpi, b.dpi)
            && a.clear.r == b.clear.r && a.clear.g == b.clear.g && a.clear.b == b.clear.b && a.clear.a == b.clear.a
            && sameBits(a.textGamma, b.textGamma) && a.theme == b.theme && sameBits(a.seconds, b.seconds)
            && a.frame == b.frame && sameBits(a.dt, b.dt) && a.fixedClock == b.fixedClock
            && a.displayLinked == b.displayLinked && sameBits(a.fps, b.fps) && a.fullRate == b.fullRate
            && a.overflows == b.overflows;
    }

    bool sameAxes(const std::vector<funkgui::AxisRec>& a, const std::vector<funkgui::AxisRec>& b)
    {
        if (a.size() != b.size())
            return false;
        const auto sameMap = [](const funkgui::AxisMap& x, const funkgui::AxisMap& y) {
            return sameBits(x.px0, y.px0) && sameBits(x.px1, y.px1) && sameBits(x.v0, y.v0) && sameBits(x.v1, y.v1)
                && x.log == y.log;
        };
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].tag != b[i].tag || a[i].hasX != b[i].hasX || a[i].hasY != b[i].hasY || !sameMap(a[i].x, b[i].x)
                || !sameMap(a[i].y, b[i].y))
                return false;
        return true;
    }

    bool sameFingerprint(const funkgui::Fingerprint& a, const funkgui::Fingerprint& b)
    {
        return a.geometry == b.geometry && a.text == b.text && a.statics == b.statics && a.live == b.live
            && a.texts == b.texts && a.rrects == b.rrects && a.segments == b.segments && a.areas == b.areas
            && sameBits(a.maxX, b.maxX) && sameBits(a.maxY, b.maxY) && a.tagCounts == b.tagCounts;
    }

    // The frame through dump v2 in memory and back: the same primitives bit for bit, the same frame info, axes and
    // missing glyphs, and the same fingerprint.
    bool roundTrips(const funkgui::PrimList& l)
    {
        char* buf = nullptr;
        size_t len = 0;
        std::FILE* f = open_memstream(&buf, &len);
        if (f == nullptr)
            return false;
        const bool written = l.writeText(f);
        const bool closed = std::fclose(f) == 0;
        funkgui::PrimList back;
        const bool parsed = written && closed && funkgui::PrimList::parseText(std::string_view(buf, len), back);
        std::free(buf);
        if (!parsed || back.prims.size() != l.prims.size() || !sameInfo(back.info, l.info)
            || !sameAxes(back.axes, l.axes) || back.missingGlyphs != l.missingGlyphs
            || std::memcmp(back.missingFirst, l.missingFirst, sizeof l.missingFirst) != 0)
            return false;
        for (size_t i = 0; i < l.prims.size(); ++i)
            if (std::memcmp(&back.prims[i], &l.prims[i], sizeof(funkgui::Prim)) != 0)
                return false;
        return sameFingerprint(funkgui::fingerprint(back), funkgui::fingerprint(l));
    }

    std::vector<std::string> a11yLines(const funkgui::HeadlessHost& host)
    {
        std::vector<std::string> lines;
        for (const funkgui::A11yItem& item : host.accessibility())
            if (item.visible)
                lines.push_back(funkgui::a11yDumpLine(item));
        return lines;
    }

    bool writeDumpFile(const std::string& dir, const std::string& name, const funkgui::HeadlessHost& host)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path d = fs::absolute(fs::path(dir), ec);      // relative to the working directory
        if (ec)
            return false;
        fs::create_directories(d, ec);
        return !ec && fs::is_directory(d, ec) && host.writeDump((d / name).string().c_str());
    }

    // One state at one dpi and theme: a fresh section, settled, scripted, settled, drawn.
    struct Run
    {
        bool settled = false;
        int  frames = 0;
        funkgui::Fingerprint fp;
        bool roundTrip = false;
        uint32_t missing = 0;
        std::vector<std::string> a11y;
    };

    Run runState(const G::SectionInfo& s, const G::State& st, float dpi, int theme, const std::string& dumpDir)
    {
        Run r;
        G::GalleryPanel panel(s);
        funkgui::HeadlessHost host(panel, theme, dpi);
        const int before = host.settle(kMaxSettle, kDt);
        if (st.script)
            st.script(host, panel.section());
        const int after = host.settle(kMaxSettle, kDt);
        r.settled = before <= kMaxSettle && after <= kMaxSettle;
        r.frames = before + after;
        const funkgui::PrimList& l = host.draw();
        r.fp = funkgui::fingerprint(l);
        r.roundTrip = roundTrips(l);
        r.missing = l.missingGlyphs;
        r.a11y = a11yLines(host);
        if (!dumpDir.empty() && theme == 0)
        {
            const std::string name = s.name + "." + st.name + ".dpi" + std::to_string(static_cast<int>(dpi)) + ".dump";
            if (!writeDumpFile(dumpDir, name, host))
                std::printf("could not write %s/%s\n", dumpDir.c_str(), name.c_str());
        }
        return r;
    }
}

int main(int argc, char** argv)
{
    const funkgui::HeadlessGuiScope gui;                 // JUCE's GUI side, when there is JUCE: the atlas bakes there

    // The tool's own flags, taken out before the harness parses argv.
    std::string section, dumpDir;
    bool list = false;
    std::vector<char*> args;
    args.push_back(argc > 0 ? argv[0] : nullptr);
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view a = argv[i] != nullptr ? argv[i] : "";
        if (a == "--section" || a == "--dump-dir")
        {
            if (i + 1 >= argc || argv[i + 1] == nullptr || argv[i + 1][0] == '\0' || argv[i + 1][0] == '-')
            {
                std::printf("HARNESS ERROR  %s needs a value\n", argv[i]);
                return 4;
            }
            (a == "--section" ? section : dumpDir) = argv[++i];
            continue;
        }
        if (a == "--list")
        {
            list = true;
            continue;
        }
        args.push_back(argv[i]);
    }
    if (list)
    {
        for (const G::SectionInfo& s : G::sections())
        {
            std::printf("%s:", s.name.c_str());
            for (const G::State& st : s.states)
                std::printf(" %s", st.name.c_str());
            std::printf("\n");
        }
        return 0;
    }
    const int n = static_cast<int>(args.size());
    args.push_back(nullptr);
    const std::vector<std::string> pos = T::positionals(n, args.data());
    if (pos.empty())
    {
        std::fprintf(stderr, "usage: %s <probe> [--section <name>] [--dump-dir <dir>] --golden-root <dir> "
                             "--arch arm64|x86_64|wasm32 [--bless-to <dir>] [--results <dir>]\n       %s --list\n",
                     argc > 0 ? argv[0] : "FunkGuiGalleryProbe", argc > 0 ? argv[0] : "FunkGuiGalleryProbe");
        return 4;
    }
    T::Probe P(pos[0], "", n, args.data());

    constexpr std::string_view kPrefix = "fg.gallery.";
    if (section.empty() && pos[0].starts_with(kPrefix))
        section = pos[0].substr(kPrefix.size());

    // Names become test names and golden keys; a duplicate section would make two tests share goldens.
    bool namesOk = true;
    std::set<std::string> seen;
    for (const G::SectionInfo& s : G::sections())
    {
        std::set<std::string> states;
        namesOk = namesOk && G::validName(s.name) && seen.insert(s.name).second && !s.states.empty()
               && static_cast<bool>(s.make);
        for (const G::State& st : s.states)
            namesOk = namesOk && G::validName(st.name) && states.insert(st.name).second;
    }
    P.eq("gallery.names", namesOk, 1);

    const G::SectionInfo* info = G::findSection(section);
    if (info == nullptr)
    {
        std::string known;
        for (const G::SectionInfo& s : G::sections())
            known += " " + s.name;
        P.harnessError("no gallery section '" + section + "' (registered:" + known + ")");
        return P.finish();
    }

    auto& fonts = funkgui::FontService::get();
    fonts.atlas();
    if (!P.eq("font.ok", fonts.ok(), 1))
        return P.finish();

    for (const G::State& st : info->states)
    {
        std::vector<std::string> a11yRef;
        bool a11yFirst = true, a11ySame = true;
        for (const int dpi : { 1, 2 })
        {
            const std::string key = st.name + ".dpi" + std::to_string(dpi);
            Run runs[2];
            for (const int theme : { 0, 1 })
            {
                Run& r = runs[theme];
                r = runState(*info, st, static_cast<float>(dpi), theme, dumpDir);
                if (!r.settled)
                    P.harnessError("section " + info->name + ", state " + st.name + ", dpi " + std::to_string(dpi)
                                   + ", theme " + std::to_string(theme) + ": not settled within "
                                   + std::to_string(kMaxSettle) + " frames");
                if (a11yFirst)
                {
                    a11yRef = r.a11y;
                    a11yFirst = false;
                }
                a11ySame = a11ySame && r.a11y == a11yRef;
            }
            const Run& r0 = runs[0];
            std::printf("%-24s settle %3d  geometry %016llx  text %016llx  statics %d (text %d)  live %d  "
                        "extent %.2f x %.2f\n",
                        key.c_str(), r0.frames, static_cast<unsigned long long>(r0.fp.geometry),
                        static_cast<unsigned long long>(r0.fp.text), r0.fp.statics, r0.fp.texts, r0.fp.live,
                        static_cast<double>(r0.fp.maxX), static_cast<double>(r0.fp.maxY));
            funkgui::addMetrics(P, key, r0.fp);
            P.eq(key + ".glyphs_missing", r0.missing, 0);
            P.eq(key + ".dump_roundtrip", r0.roundTrip && runs[1].roundTrip, 1);
            P.eq(key + ".theme_invariant", sameFingerprint(r0.fp, runs[1].fp), 1);
        }
        P.eq(st.name + ".a11y_invariant", a11ySame, 1);
        P.lines("a11y." + st.name, a11yRef);
    }
    return P.finish();
}

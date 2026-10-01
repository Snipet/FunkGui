// FunkGuiGalleryLive: the live/headless parity test fg.gallery.live (G7; FCompressor docs/design/02-funkgui-and-ui.md
// §3.9 "Live/headless parity", §5.1; C G1; SPRINTS.md S8.3; K3 #19). Registered by tools/GalleryApp/CMakeLists.txt,
// labels fg;gpu;live: it opens real windows, so it needs a window server and Metal and is never in `verify`.
//
//   FunkGuiGalleryLive fg.gallery.live --app <FunkGuiGalleryApp binary> --capture <tools/capture-frame.sh>
//                      [--out-dir <dir>] --golden-root <dir> --arch arm64|x86_64 [--bless-to <dir>] [--results <dir>]
//
// Per case, in order (a fresh app process each):
//   headless  GalleryPanel(section) in a HeadlessHost at dpi 2, theme 0: settle, the case's keys, one tick (the
//             visible a11y items are kept as lines here: EditorHost's A11Y_DUMP follows its first frame), settle, draw.
//             The frame is written to <out-dir>/<case>.headless.dump.
//   live      tools/capture-frame.sh <app> <out-dir>/<case>.live.dump FUNKGUI_ with FUNKGUI_GALLERY_SECTION, the same
//             keys as FUNKGUI_UI_KEYS, FUNKGUI_UI_FIXED_DT = 1/60 s, FUNKGUI_UI_SCALE = 2, FUNKGUI_UI_THEME = 0,
//             FUNKGUI_CANVAS_DUMP_AFTER = max(8, frames headless needed to settle + 2) and FUNKGUI_A11Y_DUMP: the app's
//             EditorHost draws the section on the GPU, and its recorded frame is dumped once the Panel has been
//             ticked at least as long as the headless one settled.
// Spec rows per case (no goldens: the reference is the headless frame of the same build):
//   <case>.captured          the script exited 0 and the dump parsed
//   <case>.clock_fixed       the dump's clock is "fixed" (UI_FIXED_DT reached the EditorHost)
//   <case>.dpi               the dump's dpi is exactly 2 (UI_SCALE reached the drawable)
//   <case>.glyphs_missing    0
//   <case>.geometry_equal    fingerprint geometry hash, live == headless
//   <case>.text_equal        fingerprint text hash, live == headless
//   <case>.fingerprint_equal every fingerprint field (counts, extents, tag counts)
//   <case>.static_bit_equal  the non-live primitives, in order, bit for bit (02 §3.9: "bit-equal PrimLists")
//   <case>.a11y_equal        the A11Y_DUMP lines == HeadlessHost::accessibility() lines after the same first tick
//   <case>.overflows         0; for the overflow case (an 8 KiB transient buffer) >= 1: BgfxSink's drop is counted and
//                            reaches the dump ("overflow N", 02 §4.5) while the recorded frame is still equal
// The spike fallback of the card (geometry-only parity, text excluded) is not used: text is compared.

#include "../../test/gallery/GalleryPanel.h"

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Fingerprint.h>
#include <funkgui/canvas/Prim.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/test/Harness.h>
#include <funkgui/text/FontService.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <spawn.h>
#include <sys/wait.h>

extern char** environ;

namespace T = funkgui::test;
namespace G = funkgui::gallery;

namespace
{
    constexpr float kDt = 1.0f / 60.0f;
    constexpr int   kMaxSettle = 600;
    constexpr int   kMinDumpAfter = 8;               // HR's capture frame; also lets the fallback clock settle

    struct Case
    {
        const char* key;                             // row prefix and file stem
        const char* section;
        const char* keys;                            // UI_KEYS / HeadlessHost::keys; "" = the section at rest
        uint32_t    transientVbBytes;                // 0 = the default budget
    };

    // Three sections (HR's primitive kinds with text and a live element; KIND_AREA strips; the RuleSlider widget
    // grid), one keyboard-driven state through the UI_KEYS replay, and the overflow path. Web Sprint B (v0.12.0): the
    // services section at rest, whose HOST line draws HostServices::services() and commandKeyIsMeta(), so EditorHost
    // must answer both as HeadlessHost does (every service; the platform's command key).
    constexpr Case kCases[] = {
        { "primitives", "primitives", "", 0 },
        { "area", "area", "", 0 },
        { "ruleslider", "ruleslider", "", 0 },
        { "ruleslider_keys", "ruleslider", "tab,tab,right", 0 },
        { "overflow", "primitives", "", 8192 },
        { "services", "services", "", 0 },
    };

    bool sameBits(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }

    bool sameFingerprint(const funkgui::Fingerprint& a, const funkgui::Fingerprint& b)
    {
        return a.geometry == b.geometry && a.text == b.text && a.statics == b.statics && a.live == b.live
            && a.texts == b.texts && a.rrects == b.rrects && a.segments == b.segments && a.areas == b.areas
            && sameBits(a.maxX, b.maxX) && sameBits(a.maxY, b.maxY) && a.tagCounts == b.tagCounts;
    }

    bool isLive(const funkgui::Prim& p)
    {
        return (static_cast<uint32_t>(p.d2[3] + 0.5f) & funkgui::pflag::live) != 0u;
    }

    // The non-live primitives of both lists, in order, byte for byte (tags included); the index of the first
    // difference in `firstDiff` (-1 when equal).
    bool staticBitEqual(const funkgui::PrimList& a, const funkgui::PrimList& b, long& firstDiff)
    {
        std::vector<const funkgui::Prim*> sa, sb;
        for (const auto& p : a.prims)
            if (!isLive(p))
                sa.push_back(&p);
        for (const auto& p : b.prims)
            if (!isLive(p))
                sb.push_back(&p);
        firstDiff = -1;
        const size_t n = std::min(sa.size(), sb.size());
        for (size_t i = 0; i < n; ++i)
            if (std::memcmp(sa[i], sb[i], sizeof(funkgui::Prim)) != 0)
            {
                firstDiff = static_cast<long>(i);
                return false;
            }
        if (sa.size() != sb.size())
        {
            firstDiff = static_cast<long>(n);
            return false;
        }
        return true;
    }

    std::string readFile(const std::string& path, bool& ok)
    {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream s;
        s << in.rdbuf();
        ok = static_cast<bool>(in) || in.eof();
        return s.str();
    }

    std::vector<std::string> splitLines(const std::string& text)
    {
        std::vector<std::string> lines;
        std::string cur;
        for (const char c : text)
        {
            if (c == '\n')
            {
                lines.push_back(cur);
                cur.clear();
            }
            else
                cur += c;
        }
        if (!cur.empty())
            lines.push_back(cur);
        return lines;
    }

    bool writeText(const std::string& path, const std::string& text)
    {
        std::ofstream out(path, std::ios::binary);
        out << text;
        return static_cast<bool>(out);
    }

    // The headless reference of one case.
    struct Headless
    {
        funkgui::PrimList frame;
        int settleFrames = 0;
        bool settled = false;
        std::vector<std::string> a11y;
    };

    Headless runHeadless(const G::SectionInfo& info, const Case& c, const std::string& dumpPath)
    {
        Headless h;
        G::GalleryPanel panel(info);
        funkgui::HeadlessHost host(panel, 0, 2.0f);
        const int before = host.settle(kMaxSettle, kDt);
        if (c.keys[0] != '\0')
            host.keys(c.keys);
        // EditorHost writes A11Y_DUMP after its first frame: the keys replayed, one tick, one draw.
        host.tick(1, kDt);
        for (const funkgui::A11yItem& item : host.accessibility())
            if (item.visible)
                h.a11y.push_back(funkgui::a11yDumpLine(item));
        const int after = host.settle(kMaxSettle, kDt);
        h.settled = before <= kMaxSettle && after <= kMaxSettle;
        h.settleFrames = before + 1 + after;
        h.frame = host.draw();
        host.writeDump(dumpPath.c_str());
        return h;
    }

    // Runs `argv` (argv[0] is the program) with this process's environment; the exit status, or -1.
    int run(const std::vector<std::string>& argv)
    {
        std::vector<char*> av;
        for (const auto& a : argv)
            av.push_back(const_cast<char*>(a.c_str()));
        av.push_back(nullptr);
        pid_t pid = 0;
        if (posix_spawn(&pid, av[0], nullptr, nullptr, av.data(), environ) != 0)
            return -1;
        int status = 0;
        while (waitpid(pid, &status, 0) < 0)
            if (errno != EINTR)
                return -1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

    std::string fmtDt(float dt)
    {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.9g", static_cast<double>(dt));
        return buf;
    }
}

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;      // FontService bakes the atlas through JUCE's font stack

    // The tool's own flags, taken out before the harness parses argv (GalleryProbe's convention).
    std::string app, capture, outDir = "frames";
    std::vector<char*> args;
    args.push_back(argc > 0 ? argv[0] : nullptr);
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view a = argv[i] != nullptr ? argv[i] : "";
        if (a == "--app" || a == "--capture" || a == "--out-dir")
        {
            if (i + 1 >= argc || argv[i + 1] == nullptr || argv[i + 1][0] == '\0')
            {
                std::printf("HARNESS ERROR  %s needs a value\n", argv[i]);
                return 4;
            }
            (a == "--app" ? app : a == "--capture" ? capture : outDir) = argv[++i];
            continue;
        }
        args.push_back(argv[i]);
    }
    const int n = static_cast<int>(args.size());
    args.push_back(nullptr);
    const std::vector<std::string> pos = T::positionals(n, args.data());
    if (pos.empty())
    {
        std::fprintf(stderr, "usage: %s fg.gallery.live --app <FunkGuiGalleryApp> --capture <capture-frame.sh> "
                             "[--out-dir <dir>] --golden-root <dir> --arch arm64|x86_64 [--bless-to <dir>] "
                             "[--results <dir>]\n",
                     argc > 0 ? argv[0] : "FunkGuiGalleryLive");
        return 4;
    }
    T::Probe P(pos[0], "", n, args.data());

    if (app.empty() || capture.empty())
    {
        P.harnessError("--app and --capture are required");
        return P.finish();
    }
    const juce::File out = juce::File::getCurrentWorkingDirectory().getChildFile(juce::String(outDir));
    if (!out.createDirectory().wasOk())
    {
        P.harnessError("cannot create --out-dir " + outDir);
        return P.finish();
    }
    auto& fonts = funkgui::FontService::get();
    fonts.atlas();
    if (!P.eq("font.ok", fonts.ok(), 1))
        return P.finish();

    for (const Case& c : kCases)
    {
        const std::string key = c.key;
        const G::SectionInfo* info = G::findSection(c.section);
        if (info == nullptr)
        {
            P.harnessError(std::string("no gallery section '") + c.section + "'");
            continue;
        }
        const std::string stem = out.getChildFile(juce::String(key)).getFullPathName().toStdString();
        const std::string liveDump = stem + ".live.dump", a11yDump = stem + ".a11y.txt";

        // ---- headless reference ---------------------------------------------------------------------------------
        const Headless h = runHeadless(*info, c, stem + ".headless.dump");
        if (!h.settled)
        {
            P.harnessError("case " + key + ": the headless section did not settle within " + std::to_string(kMaxSettle)
                           + " frames");
            continue;
        }

        // ---- live capture -------------------------------------------------------------------------------------------
        std::remove(liveDump.c_str());
        std::remove(a11yDump.c_str());
        const int dumpAfter = std::max(kMinDumpAfter, h.settleFrames + 2);
        T::setEnv("FUNKGUI_GALLERY_SECTION", c.section);
        if (c.keys[0] != '\0')
            T::setEnv("FUNKGUI_UI_KEYS", c.keys);
        else
            T::unsetEnv("FUNKGUI_UI_KEYS");
        if (c.transientVbBytes != 0)
            T::setEnv("FUNKGUI_GALLERY_TRANSIENT_VB_BYTES", std::to_string(c.transientVbBytes).c_str());
        else
            T::unsetEnv("FUNKGUI_GALLERY_TRANSIENT_VB_BYTES");
        T::setEnv("FUNKGUI_CANVAS_DUMP_AFTER", std::to_string(dumpAfter).c_str());
        T::setEnv("FUNKGUI_UI_FIXED_DT", fmtDt(kDt).c_str());
        T::setEnv("FUNKGUI_UI_SCALE", "2");
        T::setEnv("FUNKGUI_UI_THEME", "0");
        T::setEnv("FUNKGUI_A11Y_DUMP", a11yDump.c_str());
        T::setEnv("FUNKGUI_GPU_LOG", "1");
        const int rc = run({ "/bin/sh", capture, app, liveDump, "FUNKGUI_" });

        bool read = false;
        const std::string liveText = rc == 0 ? readFile(liveDump, read) : std::string();
        funkgui::PrimList live;
        const bool parsed = read && funkgui::PrimList::parseText(liveText, live);
        if (!P.eq(key + ".captured", parsed, 1))
        {
            std::printf("INFO     %s: capture-frame.sh exited %d%s\n", key.c_str(), rc,
                        rc == 0 ? " but the dump did not parse" : " (no window server, or the GPU path never came up)");
            continue;
        }

        // ---- compare ------------------------------------------------------------------------------------------------
        const funkgui::Fingerprint fh = funkgui::fingerprint(h.frame), fl = funkgui::fingerprint(live);
        std::printf("INFO     %-16s settle %3d  dump after %3d  headless %016llx/%016llx  live %016llx/%016llx  "
                    "statics %d/%d  overflows %u\n",
                    key.c_str(), h.settleFrames, dumpAfter, static_cast<unsigned long long>(fh.geometry),
                    static_cast<unsigned long long>(fh.text), static_cast<unsigned long long>(fl.geometry),
                    static_cast<unsigned long long>(fl.text), fh.statics, fl.statics,
                    static_cast<unsigned>(live.info.overflows));
        P.eq(key + ".clock_fixed", live.info.fixedClock, 1);
        P.eq(key + ".dpi", sameBits(live.info.dpi, 2.0f), 1);
        P.eq(key + ".glyphs_missing", live.missingGlyphs, 0);
        P.eq(key + ".geometry_equal", fh.geometry == fl.geometry, 1);
        P.eq(key + ".text_equal", fh.text == fl.text, 1);
        P.eq(key + ".fingerprint_equal", sameFingerprint(fh, fl), 1);
        long firstDiff = -1;
        if (!P.eq(key + ".static_bit_equal", staticBitEqual(h.frame, live, firstDiff), 1))
            std::printf("INFO     %s: first differing static primitive #%ld (headless %zu prims, live %zu)\n",
                        key.c_str(), firstDiff, h.frame.prims.size(), live.prims.size());

        bool a11yRead = false;
        const std::vector<std::string> liveA11y = splitLines(readFile(a11yDump, a11yRead));
        if (!P.eq(key + ".a11y_equal", a11yRead && liveA11y == h.a11y, 1))
        {
            std::string both = "headless:\n";
            for (const auto& l : h.a11y)
                both += l + "\n";
            writeText(stem + ".a11y.headless.txt", both);
            std::printf("INFO     %s: a11y differs (live %zu lines, headless %zu); see %s.a11y.*\n", key.c_str(),
                        liveA11y.size(), h.a11y.size(), stem.c_str());
        }

        if (c.transientVbBytes != 0)
            P.ge(key + ".overflows", static_cast<double>(live.info.overflows), 1.0);
        else
            P.eq(key + ".overflows", live.info.overflows, 0);
    }
    return P.finish();
}

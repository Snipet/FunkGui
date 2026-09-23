// PrefsCheck: round-trips the machine-wide UI preferences store (funkgui::UiPreferences), against a scratch copy.
//
// The theme is chosen by clicking the editor, which no headless probe can do, so this exercises the store directly.
// HR's first version wrote the REAL file with no teardown, and every editor on the machine then read whatever it left
// behind; it also wrote the value that was already there, which setTheme() short-circuits, so it reported success
// without saving anything. So: the self-check always works in a fresh scratch directory (inside
// <ENV_PREFIX>PREFS_DIR when the caller set one, as CTest does, else in the system temp directory), points
// <ENV_PREFIX>PREFS_DIR at it before the store first reads the environment, writes a value that differs from the
// current one, reads the file back off the disk, and checks that the real store was not touched. Safe unattended, so
// it is a gate (fg.prefs.check).
//
//   FunkGuiPrefsCheck <probe> --golden-root <dir> --arch arm64|x86_64 [--bless-to <dir>] [--results <dir>]
//   FunkGuiPrefsCheck write <n> | read | clamp        by hand, in <ENV_PREFIX>PREFS_DIR or a fresh temp directory
//
// Spec rows: the store's contract. Golden rows: the on-disk format (bytes of a freshly written store) and the number
// of themes, which other processes and older builds read.
// (Seeded from HardwareReverb Tools/PrefsCheck.cpp; ported to Harness v2 and funkgui::env().)

#include <funkgui/core/Config.h>
#include <funkgui/core/Env.h>
#include <funkgui/core/Theme.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/test/Harness.h>

#include <juce_data_structures/juce_data_structures.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace T = funkgui::test;

namespace
{
    juce::File absolute(const char* path)
    {
        return juce::File::getCurrentWorkingDirectory().getChildFile(juce::String::fromUTF8(path));
    }

    // A directory no earlier run can have used.
    juce::File freshDirectory(const juce::File& parent)
    {
        const juce::File dir = parent.getNonexistentChildFile(
            "FunkGuiPrefsCheck-" + juce::String(juce::Time::currentTimeMillis()), "", false);
        dir.createDirectory();
        return dir;
    }

    // Point the store at `dir`. Must run before UiPreferences::get() first reads the environment.
    void redirectStore(const juce::File& dir)
    {
        T::setEnv(FUNKGUI_ENV_PREFIX "PREFS_DIR", dir.getFullPathName().toRawUTF8());
        funkgui::envReload();                               // funkgui::env() caches: forget the earlier read
    }

    int diskTheme(const juce::File& store)
    {
        if (const auto xml = juce::XmlDocument::parse(store))
            for (auto* e : xml->getChildIterator())
                if (e->hasTagName("VALUE") && e->getStringAttribute("name") == "theme")
                    return e->getIntAttribute("val", -1);
        return -1;
    }

    int byHand(const std::vector<std::string>& args)
    {
        const juce::File dir = funkgui::env("PREFS_DIR") != nullptr
                                   ? absolute(funkgui::env("PREFS_DIR"))
                                   : freshDirectory(juce::File::getSpecialLocation(juce::File::tempDirectory));
        dir.createDirectory();
        redirectStore(dir);
        const juce::File store = dir.getChildFile("preferences.settings");
        auto& prefs = funkgui::UiPreferences::get();
        const std::string& mode = args[0];
        if (mode == "write")
        {
            const int want = args.size() > 1 ? std::atoi(args[1].c_str()) : 0;
            prefs.setTheme(want);
            std::printf("wrote theme=%d (%s) rev=%u  [%s]\n", prefs.theme(), funkgui::Theme::name(prefs.theme()),
                        prefs.revision(), store.getFullPathName().toRawUTF8());
            return prefs.theme() == want ? 0 : 1;
        }
        if (mode == "clamp")
        {
            prefs.setTheme(99);
            const bool ok = prefs.theme() >= 0 && prefs.theme() < funkgui::Theme::kCount;
            std::printf("clamp(99) -> %d  %s\n", prefs.theme(), ok ? "OK" : "FAIL");
            return ok ? 0 : 1;
        }
        std::printf("read theme=%d (%s)  [%s]\n", prefs.theme(), funkgui::Theme::name(prefs.theme()),
                    store.getFullPathName().toRawUTF8());
        return 0;
    }
}

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    const std::vector<std::string> args = T::positionals(argc, argv);
    if (!args.empty() && (args[0] == "write" || args[0] == "read" || args[0] == "clamp"))
        return byHand(args);
    if (args.empty())
    {
        std::fprintf(stderr, "usage: %s <probe> --golden-root <dir> --arch arm64|x86_64 [--bless-to <dir>] "
                             "[--results <dir>]\n       %s write <n> | read | clamp\n",
                     argc > 0 ? argv[0] : "FunkGuiPrefsCheck", argc > 0 ? argv[0] : "FunkGuiPrefsCheck");
        return 4;
    }
    T::Probe P(args[0], "", argc, argv);

    // The real store, and its state before anything here runs.
    const juce::File realStore = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                                     .getChildFile("Application Support/" FUNKGUI_PREFS_FOLDER "/preferences.settings");
    const bool realExisted = realStore.existsAsFile();
    const juce::Time realModified = realStore.getLastModificationTime();

    // A fresh scratch store, inside the caller's sandbox when there is one.
    const char* callerDir = funkgui::env("PREFS_DIR");
    const juce::File parent = callerDir != nullptr ? absolute(callerDir)
                                                   : juce::File::getSpecialLocation(juce::File::tempDirectory);
    parent.createDirectory();
    const juce::File sandbox = freshDirectory(parent);
    redirectStore(sandbox);
    const juce::File store = sandbox.getChildFile("preferences.settings");
    P.eq("prefs.env_redirect", funkgui::env("PREFS_DIR") != nullptr
                                   && absolute(funkgui::env("PREFS_DIR")) == sandbox, 1);

    auto& prefs = funkgui::UiPreferences::get();
    std::printf("%-40s %8s   %s\n", "store", "", store.getFullPathName().toRawUTF8());

    // A fresh store starts at the default theme and has no file yet.
    const int start = prefs.theme();
    P.eq("prefs.fresh_default_theme", start, 0);
    P.eq("prefs.fresh_no_file", store.existsAsFile(), 0);

    // A write must reach the disk. A value that differs from the current one, or the early-out in setTheme() makes
    // this a test of nothing.
    const int other = (start + 1) % funkgui::Theme::kCount;
    const uint32_t rev0 = prefs.revision();
    prefs.setTheme(other);
    P.eq("prefs.write_reaches_disk", diskTheme(store), other);
    P.eq("prefs.write_bumps_revision", prefs.revision() != rev0, 1);
    juce::MemoryBlock bytes;
    P.eq("prefs.disk.readable", store.loadFileAsData(bytes), 1);
    P.hash("prefs.disk.hash", T::fnv1a(bytes.getData(), bytes.getSize()));
    P.num("prefs.disk.bytes", static_cast<double>(bytes.getSize()), T::Tol::exact());
    P.num("prefs.theme_count", funkgui::Theme::kCount, T::Tol::exact());

    // Writing the value already held is a no-op (no revision bump, no write).
    const uint32_t rev1 = prefs.revision();
    prefs.setTheme(other);
    P.eq("prefs.same_value_is_noop", prefs.revision() == rev1, 1);

    // Another host changes the file; an editor opening here must see it.
    {
        juce::XmlElement root("PROPERTIES");
        auto* v = root.createNewChildElement("VALUE");
        v->setAttribute("name", "theme");
        v->setAttribute("val", start);
        root.writeTo(store);
        const uint32_t rev = prefs.revision();
        prefs.reload();
        P.eq("prefs.reload_sees_other_process", prefs.theme() == start && prefs.revision() != rev, 1);
    }

    // An out-of-range value must not select a palette that does not exist.
    prefs.setTheme(99);
    P.in("prefs.clamp_in_range", prefs.theme(), 0, funkgui::Theme::kCount - 1);
    prefs.setTheme(-5);
    P.in("prefs.clamp_negative", prefs.theme(), 0, funkgui::Theme::kCount - 1);

    // And none of it touched the real store.
    P.eq("prefs.sandboxed", store != realStore && store.isAChildOf(parent), 1);
    P.eq("prefs.real_store_untouched", realStore.existsAsFile() == realExisted
                                           && realStore.getLastModificationTime() == realModified, 1);

    sandbox.deleteRecursively();
    return P.finish();
}

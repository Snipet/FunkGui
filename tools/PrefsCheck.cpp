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
//   FunkGuiPrefsCheck setint <key> <n> | getint <key>  the same, for a generic int key (G6)
//
// Spec rows: the store's contract, including the generic int keys of 02 §5.9 (G6: getInt / setInt, written through
// beside the theme in the product's folder, Q7). Golden rows: the on-disk format (bytes of a freshly written store) and
// the number of themes, which other processes and older builds read. The generic-key rows run after the golden rows
// are measured, so the golden bytes are still those of a store holding only the theme.
// (Seeded from HardwareReverb Tools/PrefsCheck.cpp; ported to Harness v2 and funkgui::env().)

#include <funkgui/core/Config.h>
#include <funkgui/core/Env.h>
#include <funkgui/core/Theme.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/test/Harness.h>

#include <juce_data_structures/juce_data_structures.h>

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace T = funkgui::test;

namespace
{
    // UiPreferences' real store: ~/Library/Application Support/<folder>/ on macOS, the XDG configuration directory
    // ($XDG_CONFIG_HOME, else ~/.config) on Linux (v0.11.0).
    juce::File platformStore()
    {
        const juce::File base = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
       #if JUCE_LINUX || JUCE_BSD
        return base.getChildFile(FUNKGUI_PREFS_FOLDER "/preferences.settings");
       #else
        return base.getChildFile("Application Support/" FUNKGUI_PREFS_FOLDER "/preferences.settings");
       #endif
    }

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

    // The value of the <VALUE name="<key>" val="…"/> element on disk, as the text it holds ("" when absent).
    juce::String diskText(const juce::File& store, const char* key)
    {
        if (const auto xml = juce::XmlDocument::parse(store))
            for (auto* e : xml->getChildIterator())
                if (e->hasTagName("VALUE") && e->getStringAttribute("name") == key)
                    return e->getStringAttribute("val");
        return {};
    }

    int diskInt(const juce::File& store, const char* key)
    {
        const juce::String t = diskText(store, key);
        return t.isEmpty() ? -1 : t.getIntValue();
    }

    int diskTheme(const juce::File& store)
    {
        return diskInt(store, "theme");
    }

    // Another process's write: the whole store replaced by these VALUE elements.
    void writeStore(const juce::File& store, std::initializer_list<std::pair<const char*, const char*>> values)
    {
        juce::XmlElement root("PROPERTIES");
        for (const auto& [name, val] : values)
        {
            auto* v = root.createNewChildElement("VALUE");
            v->setAttribute("name", name);
            v->setAttribute("val", val);
        }
        root.writeTo(store);
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
        if (mode == "setint" && args.size() > 2)
        {
            const int want = std::atoi(args[2].c_str());
            prefs.setInt(args[1].c_str(), want);
            const int got = prefs.getInt(args[1].c_str(), 0, INT_MIN, INT_MAX);
            std::printf("wrote %s=%d rev=%u  [%s]\n", args[1].c_str(), got, prefs.revision(),
                        store.getFullPathName().toRawUTF8());
            return got == want ? 0 : 1;
        }
        if (mode == "getint" && args.size() > 1)
        {
            std::printf("read %s=%d  [%s]\n", args[1].c_str(), prefs.getInt(args[1].c_str(), 0, INT_MIN, INT_MAX),
                        store.getFullPathName().toRawUTF8());
            return 0;
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
    if (!args.empty() && (args[0] == "write" || args[0] == "read" || args[0] == "clamp" || args[0] == "setint"
                          || args[0] == "getint"))
        return byHand(args);
    if (args.empty())
    {
        std::fprintf(stderr, "usage: %s <probe> --golden-root <dir> --arch arm64|x86_64 [--bless-to <dir>] "
                             "[--results <dir>]\n       %s write <n> | read | clamp | setint <key> <n> | getint <key>\n",
                     argc > 0 ? argv[0] : "FunkGuiPrefsCheck", argc > 0 ? argv[0] : "FunkGuiPrefsCheck");
        return 4;
    }
    T::Probe P(args[0], "", argc, argv);

    // The real store, and its state before anything here runs.
    const juce::File realStore = platformStore();
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

    // ---- Generic int keys (02 §5.9; G6). The product's folder (Q7), then getInt / setInt with setTheme's semantics. ----
    P.eq("prefs.default_file_in_product_folder", funkgui::UiPreferences::defaultFile() == platformStore(), 1);
    P.eq("prefs.file_is_redirected_store", prefs.file() == store, 1);
    writeStore(store, { { "theme", "0" } });
    prefs.reload();
    P.eq("prefs.int.missing_is_fallback", prefs.getInt("meterScaleDb", 48, 12, 72), 48);
    P.eq("prefs.int.fallback_clamped", prefs.getInt("meterScaleDb", 99, 12, 72), 72);
    P.eq("prefs.int.bounds_either_order", prefs.getInt("meterScaleDb", 5, 72, 12), 12);
    P.eq("prefs.int.empty_key_is_fallback", prefs.getInt("", 5, 0, 10), 5);
    P.eq("prefs.int.null_key_is_fallback", prefs.getInt(nullptr, 5, 0, 10), 5);
    {
        const uint32_t rev = prefs.revision();
        prefs.setInt("meterScaleDb", 24);
        P.eq("prefs.int.write_reaches_disk", diskInt(store, "meterScaleDb"), 24);
        P.eq("prefs.int.write_bumps_revision", prefs.revision() != rev, 1);
        P.eq("prefs.int.reads_back", prefs.getInt("meterScaleDb", 48, 12, 72), 24);
        P.eq("prefs.int.theme_kept_on_disk", diskTheme(store), prefs.theme());
    }
    {
        const uint32_t rev = prefs.revision();
        const juce::Time before = store.getLastModificationTime();
        prefs.setInt("meterScaleDb", 24);
        P.eq("prefs.int.same_value_is_noop", prefs.revision() == rev && store.getLastModificationTime() == before, 1);
    }
    prefs.setInt("historySpanTenths", 200);
    P.eq("prefs.int.keys_independent", prefs.getInt("meterScaleDb", 48, 12, 72) == 24
                                            && prefs.getInt("historySpanTenths", 50, 25, 200) == 200
                                            && diskInt(store, "meterScaleDb") == 24
                                            && diskInt(store, "historySpanTenths") == 200, 1);
    P.eq("prefs.int.clamped_on_read", prefs.getInt("historySpanTenths", 50, 25, 100), 100);
    prefs.setInt("offsetDb", -12);
    P.eq("prefs.int.negative_round_trip", prefs.getInt("offsetDb", 0, -100, 100), -12);
    P.eq("prefs.int.disk_is_decimal_text", diskText(store, "offsetDb") == "-12", 1);
    prefs.setInt("theme", 99);
    P.eq("prefs.int.theme_key_is_the_theme", prefs.theme() == funkgui::Theme::kCount - 1
                                                  && prefs.getInt("theme", 0, 0, 9) == funkgui::Theme::kCount - 1
                                                  && diskTheme(store) == funkgui::Theme::kCount - 1, 1);
    prefs.setInt("THEME", 0);
    P.eq("prefs.int.theme_key_ignores_case", prefs.theme(), 0);

    // Another process's store: its generic values are seen by reload(), which bumps the revision once; a damaged value
    // reads as the fallback, never as whatever digits it starts with.
    writeStore(store, { { "theme", "0" }, { "meterScaleDb", "72" }, { "historySpanTenths", "25x" },
                        { "offsetDb", "" }, { "big", "99999999999" } });
    {
        const uint32_t rev = prefs.revision();
        prefs.reload();
        P.eq("prefs.int.reload_sees_other_process", prefs.getInt("meterScaleDb", 48, 12, 72) == 72
                                                        && prefs.revision() == rev + 1, 1);
        const uint32_t rev2 = prefs.revision();
        prefs.reload();
        P.eq("prefs.int.reload_unchanged_no_bump", prefs.revision(), rev2);
    }
    P.eq("prefs.int.damaged_is_fallback", prefs.getInt("historySpanTenths", 50, 25, 200), 50);
    P.eq("prefs.int.empty_is_fallback", prefs.getInt("offsetDb", 3, -100, 100), 3);
    P.eq("prefs.int.overflow_is_fallback", prefs.getInt("big", 7, INT_MIN, INT_MAX), 7);

    // And none of it touched the real store.
    P.eq("prefs.sandboxed", store != realStore && store.isAChildOf(parent), 1);
    P.eq("prefs.real_store_untouched", realStore.existsAsFile() == realExisted
                                           && realStore.getLastModificationTime() == realModified, 1);

    sandbox.deleteRecursively();
    return P.finish();
}

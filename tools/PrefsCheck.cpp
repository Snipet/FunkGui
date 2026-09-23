// Round-trips the machine-wide UI preferences store — against a scratch copy.
//
// The theme is chosen by clicking the editor, which no headless harness can
// do, so this exercises the store directly. It used to write the REAL file,
// with no teardown, and every editor on the machine then read whatever it
// left behind; it also wrote the value that was already there, which
// setTheme() short-circuits, so it reported success without saving anything.
// Now it redirects the store (<ENV_PREFIX>PREFS_DIR, honoured by UiPreferences) to a
// fresh temporary directory unless the caller already pointed it somewhere,
// writes a value that differs from the current one, and reads the file back
// off the disk. Safe to run unattended, so it can be a gate.
//
//   HardwareReverbPrefsCheck                       self-check: table, metrics, exit code
//   HardwareReverbPrefsCheck --check <golden>
//   HardwareReverbPrefsCheck write <n> | read | clamp     by hand (sandboxed too)

#include "gui/UiPreferences.h"
#include "gui/Theme.h"
#include "Harness.h"

#include <juce_data_structures/juce_data_structures.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    // Before the singleton first reads the environment.
    juce::File sandbox;
    if (std::getenv(FUNKGUI_ENV_PREFIX "PREFS_DIR") == nullptr)
    {
        sandbox = juce::File::getSpecialLocation(juce::File::tempDirectory)
                      .getChildFile("HardwareReverbPrefsCheck-"
                                    + juce::String(juce::Time::currentTimeMillis()));
        sandbox.createDirectory();
        funkgui::test::setEnv(FUNKGUI_ENV_PREFIX "PREFS_DIR", sandbox.getFullPathName().toRawUTF8());
    }
    const juce::File store = juce::File(juce::String(std::getenv(FUNKGUI_ENV_PREFIX "PREFS_DIR")))
                                 .getChildFile("preferences.settings");
    const juce::File realStore = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                                     .getChildFile("Application Support/" FUNKGUI_PREFS_FOLDER "/preferences.settings");
    auto& prefs = funkgui::UiPreferences::get();

    const juce::String mode = (argc > 1 && argv[1][0] != '-') ? juce::String(argv[1]) : juce::String("self");
    if (mode == "write")
    {
        const int want = argc > 2 ? std::atoi(argv[2]) : 0;
        prefs.setTheme(want);
        std::printf("wrote theme=%d (%s) rev=%u  [%s]\n", prefs.theme(),
                    funkgui::Theme::name(prefs.theme()), prefs.revision(),
                    store.getFullPathName().toRawUTF8());
        return prefs.theme() == want ? 0 : 1;
    }
    if (mode == "clamp")
    {
        prefs.setTheme(99);
        const bool ok = prefs.theme() >= 0 && prefs.theme() < funkgui::Theme::kCount;
        std::printf("clamp(99) -> %d  %s\n", prefs.theme(), ok ? "OK" : "FAIL");
        return ok ? 0 : 1;
    }
    if (mode == "read")
    {
        std::printf("read theme=%d (%s)  [%s]\n", prefs.theme(),
                    funkgui::Theme::name(prefs.theme()), store.getFullPathName().toRawUTF8());
        return 0;
    }

    // ---- self-check --------------------------------------------------------
    std::vector<funkgui::test::Metric> m;
    auto diskTheme = [&]() -> int
    {
        if (auto xml = juce::XmlDocument::parse(store))
            for (auto* e : xml->getChildIterator())
                if (e->hasTagName("VALUE") && e->getStringAttribute("name") == "theme")
                    return e->getIntAttribute("val", -1);
        return -1;
    };
    std::printf("%-40s %8s\n", "check", "result");

    // A write must reach the disk. A value that differs from the current one,
    // or the early-out in setTheme() makes this a test of nothing.
    const int start = prefs.theme();
    const int other = (start + 1) % funkgui::Theme::kCount;
    prefs.setTheme(other);
    const bool wrote = diskTheme() == other;
    std::printf("%-40s %8s\n", "write reaches the file", wrote ? "ok" : "FAIL");
    funkgui::test::addNum(m, "prefs.write_reaches_disk", wrote ? 1.0 : 0.0, 0.0);

    // Another host changes the file; an editor opening here must see it.
    {
        juce::XmlElement root("PROPERTIES");
        auto* v = root.createNewChildElement("VALUE");
        v->setAttribute("name", "theme");
        v->setAttribute("val", start);
        root.writeTo(store);
        const uint32_t rev = prefs.revision();
        prefs.reload();
        const bool saw = prefs.theme() == start && prefs.revision() != rev;
        std::printf("%-40s %8s\n", "reload() sees another process's write", saw ? "ok" : "FAIL");
        funkgui::test::addNum(m, "prefs.reload_sees_other_process", saw ? 1.0 : 0.0, 0.0);
    }

    // An out-of-range value must not select a palette that does not exist.
    prefs.setTheme(99);
    const bool clamped = prefs.theme() >= 0 && prefs.theme() < funkgui::Theme::kCount;
    std::printf("%-40s %8s\n", "clamp(99) stays in range", clamped ? "ok" : "FAIL");
    funkgui::test::addNum(m, "prefs.clamp_in_range", clamped ? 1.0 : 0.0, 0.0);

    // And none of it touched the real store.
    const bool sandboxed = store != realStore;
    std::printf("%-40s %8s   %s\n", "sandboxed", sandboxed ? "ok" : "FAIL",
                store.getFullPathName().toRawUTF8());
    funkgui::test::addNum(m, "prefs.sandboxed", sandboxed ? 1.0 : 0.0, 0.0);

    if (sandbox.exists()) sandbox.deleteRecursively();
    const int rc = funkgui::test::finish(argc, argv, m);
    return (wrote && clamped && sandboxed) ? rc : 1;
}

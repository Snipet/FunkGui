// FUNKGUI_TEST name=fg.desktop.prefs timeout=120 gpu=1 links=desktop
//
// fg.desktop.prefs (v0.28.0): desktop/PrefsFile.h, the text file a desktop host keeps UiPreferences in, in a folder
// of the test's own (its sandbox, the working directory). (It links FunkGui::desktop, where the store lives; it opens
// no window and needs no display.)
//   file.*     a store over a file that is not there is empty and makes none until something is written; a write
//              makes the folder and the file; the file's text is the format's, byte for byte; nothing is left beside
//              it; a second store over the file reads the values back
//   value.*    a backslash and line breaks in a value survive the file; a key that cannot be stored is ignored
//   reload.*   reload() reports nothing when the file is as it was, and the change when another store (another
//              process) wrote it; a file by hand with comments, a blank line, a line with no '=', a repeated key and
//              Windows line ends reads by the format's rules
//   prefs.*    as UiPreferences' backend: setTheme and setInt are written through, a new process's store (a second
//              backend over the file) has them, reload() follows a change from outside and bumps the revision
// Spec rows only.

#include <funkgui/desktop/PrefsFile.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/test/Harness.h>

#include <cstdio>
#include <filesystem>
#include <string>

namespace T = funkgui::test;
namespace D = funkgui::desktop;
using funkgui::UiPreferences;

namespace
{
    std::string readText(const std::string& path)
    {
        std::string text;
        if (std::FILE* f = std::fopen(path.c_str(), "rb"))
        {
            char buf[1024];
            for (std::size_t n = std::fread(buf, 1, sizeof buf, f); n > 0; n = std::fread(buf, 1, sizeof buf, f))
                text.append(buf, n);
            std::fclose(f);
        }
        return text;
    }

    bool writeText(const std::string& path, const std::string& text)
    {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (f == nullptr)
            return false;
        const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
        return std::fclose(f) == 0 && ok;
    }

    std::string valueOf(const UiPreferences::Backend& b, const char* key)
    {
        std::string v = "<none>";
        b.read(key, v);
        return v;
    }
}

int main(int argc, char** argv)
{
    T::Probe P("fg.desktop.prefs", "", argc, argv);
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::remove_all("prefs-test", ec);
    const std::string path = "prefs-test/deeper/preferences.txt";

    // ---- the file ---------------------------------------------------------------------------------------------------
    {
        auto store = D::prefsFileBackend(path);
        std::string v = "untouched";
        P.eq("file.missing_is_empty", !store->read("theme", v) && v == "untouched", 1);
        P.eq("file.not_made_by_reading", fs::exists(path), 0);
        P.eq("reload.missing_no_change", store->reload(), 0);
        store->write("uiZoom", "125");
        store->write("theme", "1");
        P.eq("file.made_with_its_folder", fs::exists(path), 1);
        P.eq("file.text", readText(path) == "# FunkGui preferences 1\ntheme=1\nuiZoom=125\n", 1);
        P.eq("file.no_partial_left", fs::exists(path + ".partial"), 0);
        store->write("theme", "0");
        P.eq("file.rewritten", readText(path) == "# FunkGui preferences 1\ntheme=0\nuiZoom=125\n", 1);
        P.eq("reload.own_write_no_change", store->reload(), 0);

        auto second = D::prefsFileBackend(path);
        P.eq("file.read_back", valueOf(*second, "theme") == "0" && valueOf(*second, "uiZoom") == "125", 1);
        P.eq("file.missing_key", valueOf(*second, "nothing") == "<none>", 1);

        // ---- values and keys ----------------------------------------------------------------------------------------
        const std::string odd = "a\\b\nc\rd=e #f";
        store->write("odd", odd);
        P.eq("value.escaped_in_the_file", readText(path).find("odd=a\\\\b\\nc\\rd=e #f\n") != std::string::npos, 1);
        P.eq("value.survives", second->reload() && valueOf(*second, "odd") == odd, 1);
        const std::string before = readText(path);
        store->write("", "x");
        store->write("a=b", "x");
        store->write("#key", "x");
        store->write("two\nlines", "x");
        store->write(nullptr, "x");
        P.eq("value.unstorable_keys_ignored", readText(path) == before, 1);
        std::string none;
        P.eq("value.null_key_reads_nothing", store->read(nullptr, none), 0);

        // ---- another process writes ---------------------------------------------------------------------------------
        second->write("theme", "1");
        P.eq("reload.sees_the_other_store", store->reload() && valueOf(*store, "theme") == "1", 1);
        P.eq("reload.then_no_change", store->reload(), 0);
        P.eq("reload.by_hand_written",
             writeText(path, "# a comment\r\n\r\ntheme=1\r\nnot a line\r\n=no key\r\nuiZoom=150\r\nuiZoom=175\r\n"
                             "plain=a\\qb\r\n"), 1);
        P.eq("reload.by_hand_changed", store->reload(), 1);
        P.eq("reload.by_hand_rules", valueOf(*store, "theme") == "1" && valueOf(*store, "uiZoom") == "175"
                                         && valueOf(*store, "plain") == "a\\qb" && valueOf(*store, "odd") == "<none>"
                                         && valueOf(*store, "not a line") == "<none>", 1);
    }

    // ---- as UiPreferences' store ------------------------------------------------------------------------------------
    {
        fs::remove_all("prefs-test", ec);
        auto& prefs = UiPreferences::get();
        prefs.setBackend(D::prefsFileBackend(path));
        P.eq("prefs.starts_at_defaults", prefs.theme() == 0 && prefs.getInt("uiZoom", 100, 25, 400) == 100, 1);
        prefs.setTheme(1);
        prefs.setInt("uiZoom", 150);
        P.eq("prefs.written_through", readText(path) == "# FunkGui preferences 1\ntheme=1\nuiZoom=150\n", 1);

        // A new process: another store over the same file.
        prefs.setBackend(D::prefsFileBackend(path));
        P.eq("prefs.next_process_has_them", prefs.theme() == 1 && prefs.getInt("uiZoom", 100, 25, 400) == 150, 1);

        // Another process changes the file while this one runs.
        const uint32_t revision = prefs.revision();
        auto other = D::prefsFileBackend(path);
        other->write("theme", "0");
        other->write("uiZoom", "175");
        prefs.reload();
        P.eq("prefs.reload_follows", prefs.theme() == 0 && prefs.getInt("uiZoom", 100, 25, 400) == 175, 1);
        P.eq("prefs.reload_bumps_revision", prefs.revision() != revision, 1);
        const uint32_t still = prefs.revision();
        prefs.reload();
        P.eq("prefs.reload_unchanged_no_bump", prefs.revision() == still, 1);
        prefs.setBackend(nullptr);                   // the default store again: the file's is let go
    }
    fs::remove_all("prefs-test", ec);
    return P.finish();
}

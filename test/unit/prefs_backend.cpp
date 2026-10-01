// FUNKGUI_TEST name=fg.prefs.backend timeout=120 gpu=0
//
// fg.prefs.backend (v0.12.0): funkgui::UiPreferences over a storage backend. JUCE-free, so it runs in both builds; the
// properties file itself (its bytes on disk, another process's write) stays fg.prefs.check's, which needs JUCE.
//
// - UiPreferences::memoryBackend(): a fresh store is the default theme and no keys; the theme and the generic int keys
//   behave as they do over the properties file (written through, a write of the held value is nothing at all, every
//   change bumps revision(), "theme" in any case is the theme, clamps, fallbacks, a damaged value reads as the
//   fallback).
// - A host's own backend (here one that records): UiPreferences reads the theme from it when it is installed, bumps the
//   revision, writes decimal text under the key it was given, exactly once per change; reload() follows what the
//   backend reports; the theme's lenient reading (leading digits, as the properties file always read it) is pinned.
// - setBackend(nullptr) restores the default store: with JUCE the properties file in <ENV_PREFIX>PREFS_DIR (file()
//   names it again; while another backend is installed file() is a null File), without JUCE an empty in-memory store.
// The test never writes the default store. Spec rows only.

#include <funkgui/core/Config.h>
#include <funkgui/core/Env.h>
#include <funkgui/core/HasJuce.h>
#include <funkgui/core/Theme.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/test/Harness.h>

#include <climits>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>

namespace T = funkgui::test;
using funkgui::UiPreferences;

namespace
{
    // What a host would write: a store of its own, here one the test can look into and change behind the class's back.
    struct Shared
    {
        std::map<std::string, std::string> values;
        std::vector<std::pair<std::string, std::string>> writes;
        int  reloads = 0;
        bool changedOnReload = false;
    };

    class Recording final : public UiPreferences::Backend
    {
    public:
        explicit Recording(std::shared_ptr<Shared> s) : s_(std::move(s)) {}

        bool read(const char* key, std::string& value) const override
        {
            const auto it = s_->values.find(key);
            if (it == s_->values.end())
                return false;
            value = it->second;
            return true;
        }

        void write(const char* key, const std::string& value) override
        {
            s_->values[key] = value;
            s_->writes.emplace_back(key, value);
        }

        bool reload() override
        {
            ++s_->reloads;
            return s_->changedOnReload;
        }

    private:
        std::shared_ptr<Shared> s_;
    };
}

int main(int argc, char** argv)
{
    const funkgui::HeadlessGuiScope gui;                 // JUCE's GUI side, when there is JUCE: its file is the store
    T::Probe P("fg.prefs.backend", "", argc, argv);
    const int last = funkgui::Theme::kCount - 1;

    // The default store is never written here, but with JUCE it is opened: keep even that off the real one when the
    // caller (CTest sets it) gave no scratch directory.
    if (funkgui::env("PREFS_DIR") == nullptr)
    {
        std::error_code ec;
        std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        if (ec)
            base = ".";
        const std::string dir = (base / ("FunkGuiPrefsBackend-" + std::to_string(::getpid()))).string();
        T::setEnv(FUNKGUI_ENV_PREFIX "PREFS_DIR", dir.c_str());
        funkgui::envReload();
    }
    const std::string scratch = funkgui::env("PREFS_DIR") != nullptr ? funkgui::env("PREFS_DIR") : "";
    P.eq("store.redirected", !scratch.empty(), 1);

    auto& prefs = UiPreferences::get();

    // ---- the in-memory store ----------------------------------------------------------------------------------------
    {
        const uint32_t rev = prefs.revision();
        prefs.setBackend(UiPreferences::memoryBackend());
        P.eq("memory.install_bumps_revision", prefs.revision(), rev + 1);
        P.eq("memory.fresh_default_theme", prefs.theme(), 0);
        P.eq("memory.fresh_no_keys", prefs.getInt("meterScaleDb", 48, 12, 72), 48);
    }
    {
        const uint32_t rev = prefs.revision();
        prefs.setTheme(last);
        P.eq("memory.theme_written", prefs.theme() == last && prefs.revision() == rev + 1, 1);
        prefs.setTheme(last);
        P.eq("memory.theme_same_is_noop", prefs.revision(), rev + 1);
        prefs.setTheme(99);
        P.eq("memory.theme_clamped_high", prefs.theme() == last && prefs.revision() == rev + 1, 1);
        prefs.setTheme(-5);
        P.eq("memory.theme_clamped_low", prefs.theme() == 0 && prefs.revision() == rev + 2, 1);
    }
    {
        const uint32_t rev = prefs.revision();
        prefs.setInt("meterScaleDb", 24);
        P.eq("memory.int_reads_back", prefs.getInt("meterScaleDb", 48, 12, 72), 24);
        P.eq("memory.int_bumps_revision", prefs.revision(), rev + 1);
        prefs.setInt("meterScaleDb", 24);
        P.eq("memory.int_same_is_noop", prefs.revision(), rev + 1);
        prefs.setInt("historySpanTenths", 200);
        prefs.setInt("offsetDb", -12);
        P.eq("memory.int_keys_independent", prefs.getInt("meterScaleDb", 48, 12, 72) == 24
                                                && prefs.getInt("historySpanTenths", 50, 25, 200) == 200
                                                && prefs.getInt("offsetDb", 0, -100, 100) == -12, 1);
        P.eq("memory.int_clamped_on_read", prefs.getInt("historySpanTenths", 50, 25, 100), 100);
        P.eq("memory.int_bounds_either_order", prefs.getInt("historySpanTenths", 50, 100, 25), 100);
        P.eq("memory.int_keys_are_exact", prefs.getInt("METERSCALEDB", 48, 12, 72), 48);
        P.eq("memory.int_empty_key", prefs.getInt("", 5, 0, 10) == 5 && prefs.getInt(nullptr, 5, 0, 10) == 5, 1);
        const uint32_t rev2 = prefs.revision();
        prefs.setInt("", 3);
        prefs.setInt(nullptr, 3);
        P.eq("memory.int_empty_key_ignored", prefs.revision(), rev2);
        prefs.setInt("theme", 99);
        P.eq("memory.theme_key_is_the_theme", prefs.theme() == last && prefs.getInt("theme", 0, 0, 9) == last, 1);
        prefs.setInt("THEME", 0);
        P.eq("memory.theme_key_ignores_case", prefs.theme(), 0);
        prefs.setInt("extremes", INT_MIN);
        P.eq("memory.int_min_round_trip", prefs.getInt("extremes", 0, INT_MIN, INT_MAX), INT_MIN);
        const uint32_t rev3 = prefs.revision();
        prefs.reload();
        P.eq("memory.reload_changes_nothing", prefs.revision() == rev3
                                                  && prefs.getInt("meterScaleDb", 48, 12, 72) == 24, 1);
    }

    // ---- a host's backend -------------------------------------------------------------------------------------------
    const auto shared = std::make_shared<Shared>();
    shared->values = { { "theme", "1" }, { "meterScaleDb", "72" }, { "damaged", "25x" }, { "empty", "" },
                       { "big", "99999999999" }, { "spaced", " 12" } };
    {
        const uint32_t rev = prefs.revision();
        prefs.setBackend(std::make_unique<Recording>(shared));
        P.eq("host.install_reads_theme", prefs.theme(), 1);
        P.eq("host.install_bumps_revision", prefs.revision(), rev + 1);
        P.eq("host.install_writes_nothing", static_cast<int64_t>(shared->writes.size()), 0);
        P.eq("host.old_store_not_copied", prefs.getInt("historySpanTenths", 50, 25, 200), 50);
        P.eq("host.int_read", prefs.getInt("meterScaleDb", 48, 12, 72), 72);
        P.eq("host.damaged_is_fallback", prefs.getInt("damaged", 50, 25, 200), 50);
        P.eq("host.empty_is_fallback", prefs.getInt("empty", 3, -100, 100), 3);
        P.eq("host.overflow_is_fallback", prefs.getInt("big", 7, INT_MIN, INT_MAX), 7);
        P.eq("host.leading_space_is_fallback", prefs.getInt("spaced", 7, 0, 100), 7);
    }
    {
        const uint32_t rev = prefs.revision();
        prefs.setTheme(0);
        prefs.setInt("meterScaleDb", 24);
        prefs.setInt("offsetDb", -12);
        prefs.setInt("meterScaleDb", 24);                    // held already
        prefs.setTheme(0);                                   // held already
        const std::vector<std::pair<std::string, std::string>> want{ { "theme", "0" }, { "meterScaleDb", "24" },
                                                                     { "offsetDb", "-12" } };
        P.eq("host.written_through_once_as_decimal", shared->writes == want, 1);
        P.eq("host.revision_per_change", prefs.revision(), rev + 3);
        prefs.setInt("Theme", 1);
        P.eq("host.theme_key_written_as_theme", shared->writes.size() == 4 && shared->writes.back().first == "theme"
                                                    && shared->writes.back().second == "1" && prefs.theme() == 1, 1);
    }
    {
        // Another process changed the store: reload() follows what the backend reports, and re-reads the theme.
        shared->values["theme"] = "0";
        shared->values["meterScaleDb"] = "48";
        shared->changedOnReload = true;
        const uint32_t rev = prefs.revision();
        prefs.reload();
        P.eq("host.reload_sees_change", prefs.theme() == 0 && prefs.getInt("meterScaleDb", 12, 12, 72) == 48
                                            && prefs.revision() == rev + 1 && shared->reloads == 1, 1);
        shared->changedOnReload = false;
        prefs.reload();
        P.eq("host.reload_unchanged_no_bump", prefs.revision() == rev + 1 && shared->reloads == 2, 1);
    }
    {
        // The theme's own reading: the properties file's getIntValue, kept (leading white space, then digits).
        const auto themeOf = [&](const char* text)
        {
            shared->values["theme"] = text;
            prefs.reload();
            return prefs.theme();
        };
        P.eq("host.theme_text_plain", themeOf("1"), 1);
        P.eq("host.theme_text_trailing", themeOf("1abc"), 1);
        P.eq("host.theme_text_leading_space", themeOf(" \t1"), 1);
        P.eq("host.theme_text_not_a_number", themeOf("abc"), 0);
        P.eq("host.theme_text_empty", themeOf(""), 0);
        P.eq("host.theme_text_too_big_clamps", themeOf("99"), last);
        P.eq("host.theme_text_negative_clamps", themeOf("-3"), 0);
        shared->values.erase("theme");
        prefs.reload();
        P.eq("host.theme_missing_is_default", prefs.theme(), 0);
    }
#if FUNKGUI_HAS_JUCE
    P.eq("juce.file_is_null_with_another_backend", prefs.file() == juce::File(), 1);
#endif

    // ---- back to the default store ----------------------------------------------------------------------------------
    {
        shared->writes.clear();
        const uint32_t rev = prefs.revision();
        prefs.setBackend(nullptr);
        P.eq("default.restored_bumps_revision", prefs.revision(), rev + 1);
        P.eq("default.host_backend_released", shared.use_count() == 1 && shared->writes.empty(), 1);
        P.eq("default.fresh_theme", prefs.theme(), 0);
        P.eq("default.fresh_no_keys", prefs.getInt("meterScaleDb", 48, 12, 72) == 48
                                          && prefs.getInt("offsetDb", 5, -100, 100) == 5, 1);
#if FUNKGUI_HAS_JUCE
        const juce::File store = prefs.file();
        P.eq("juce.default_is_the_properties_file", store.getFileName() == "preferences.settings"
                 && store.getParentDirectory() == juce::File(juce::String::fromUTF8(scratch.c_str())), 1);
        P.eq("juce.nothing_written_to_it", store.exists(), 0);
        P.eq("juce.default_file_named", UiPreferences::defaultFile().getFileName() == "preferences.settings", 1);
#endif
    }
    return P.finish();
}

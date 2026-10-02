// FUNKGUI_TEST name=fg.web.prefs timeout=120 gpu=0
//
// fg.web.prefs (v0.13.0; FCompressor ADR-93, web Sprint C): funkgui::WebPrefsBackend, the rule of the browser's
// preferences store (web/WebPrefs.h), over a storage of the test's own. Plain C++, so it runs in every build: with
// JUCE, without, and as wasm32 under node. The localStorage side (src/web/WebPrefs.cpp) needs a browser and is
// test/web's services page.
//
// - The mirror: the backend reads the prefixed keys once when it is constructed and answers read() from memory, so a
//   value changed behind its back is not seen until reload(). Keys of another prefix, and the prefix alone, are not
//   preferences.
// - The prefix: write() stores <prefix><key>; read() and reload() speak in bare keys, and take a key by its prefix
//   whatever the storage hands over. An empty prefix takes every key.
// - Change detection: reload() reports a changed value, a new key and a removed key, and nothing when the store holds
//   what the mirror holds.
// - A refused write (a private window, a full quota): the value is held, read() gives it, reload() keeps it and
//   reports no change for it, and a later write the storage takes puts the key back under the storage's rule.
// - A storage that cannot be read, and no storage at all: the mirror alone, no change reported, nothing lost.
// - Under UiPreferences: the theme is read when the backend is installed, an int is written as decimal text under the
//   prefixed key, and reload() bumps revision() exactly when the store changed.
// Spec rows only. The default store of UiPreferences is never written.

#include <funkgui/core/Config.h>
#include <funkgui/core/Env.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/test/Harness.h>
#include <funkgui/web/WebPrefs.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>

namespace T = funkgui::test;
using funkgui::UiPreferences;
using funkgui::WebPrefsBackend;
using funkgui::WebStorage;

namespace
{
    // The page's localStorage, as the test sees it: what it holds, what was asked of it, and its two ways of failing.
    struct Store
    {
        WebStorage::Entries values;
        std::vector<std::pair<std::string, std::string>> writes;     // every write taken, in order
        int  reads = 0;                                              // readAll calls, answered or not
        int  refused = 0;                                            // writes refused
        bool refuseWrites = false;
        bool unreadable = false;
        bool unfiltered = false;                                     // readAll hands over every key, any prefix
    };

    class FakeStorage final : public WebStorage
    {
    public:
        explicit FakeStorage(std::shared_ptr<Store> s) : s_(std::move(s)) {}

        bool readAll(std::string_view prefix, Entries& out) const override
        {
            ++s_->reads;
            if (s_->unreadable)
                return false;
            Entries found;
            for (const auto& [key, value] : s_->values)
                if (s_->unfiltered || key.compare(0, prefix.size(), prefix) == 0)
                    found.emplace(key, value);
            out = std::move(found);
            return true;
        }

        bool write(const std::string& key, const std::string& value) override
        {
            if (s_->refuseWrites)
            {
                ++s_->refused;
                return false;
            }
            s_->values[key] = value;
            s_->writes.emplace_back(key, value);
            return true;
        }

    private:
        std::shared_ptr<Store> s_;
    };

    std::unique_ptr<WebPrefsBackend> backendOver(const std::shared_ptr<Store>& store, std::string prefix)
    {
        return std::make_unique<WebPrefsBackend>(std::make_unique<FakeStorage>(store), std::move(prefix));
    }

    // "<value>" when the backend holds the key, "(none)" when it does not.
    std::string held(const UiPreferences::Backend& b, const char* key)
    {
        std::string v = "untouched";
        return b.read(key, v) ? "<" + v + ">" : (v == "untouched" ? "(none)" : "(none, value changed)");
    }
}

int main(int argc, char** argv)
{
    const funkgui::HeadlessGuiScope gui;                 // JUCE's GUI side, when there is JUCE (UiPreferences' store)
    T::Probe P("fg.web.prefs", "", argc, argv);

    // ---- the mirror and the prefix ----------------------------------------------------------------------------------
    {
        const auto store = std::make_shared<Store>();
        store->values = { { "App.theme", "1" }, { "App.zoom", "150" }, { "Other.zoom", "300" }, { "App.", "x" },
                          { "Apple", "y" }, { "zoom", "75" } };
        const auto b = backendOver(store, "App.");
        P.eq("mirror.constructor_reads_once", store->reads, 1);
        P.eq("mirror.prefix_kept", b->keyPrefix() == "App.", 1);
        P.eq("mirror.prefixed_keys_only", static_cast<int64_t>(b->size()), 2);
        P.eq("mirror.reads_bare_keys", held(*b, "theme") == "<1>" && held(*b, "zoom") == "<150>", 1);
        P.eq("mirror.other_prefix_not_seen", held(*b, "Other.zoom") == "(none)" && held(*b, "le") == "(none)", 1);
        P.eq("mirror.whole_key_is_not_a_key", held(*b, "App.zoom") == "(none)", 1);
        P.eq("mirror.prefix_alone_is_not_a_key", held(*b, "") == "(none)", 1);
        P.eq("mirror.null_key", held(*b, nullptr) == "(none)", 1);
        P.eq("mirror.missing_leaves_value_untouched", held(*b, "missing") == "(none)", 1);

        // A getter never goes to the storage: what another tab wrote is seen at reload(), not before.
        store->values["App.zoom"] = "125";
        P.eq("mirror.read_is_from_memory", held(*b, "zoom") == "<150>" && store->reads == 1, 1);

        // ---- write --------------------------------------------------------------------------------------------------
        b->write("meterScaleDb", "24");
        b->write("zoom", "175");
        const std::vector<std::pair<std::string, std::string>> want{ { "App.meterScaleDb", "24" },
                                                                     { "App.zoom", "175" } };
        P.eq("write.stored_under_prefixed_key", store->writes == want, 1);
        P.eq("write.mirror_holds_it", held(*b, "meterScaleDb") == "<24>" && held(*b, "zoom") == "<175>", 1);
        P.eq("write.other_keys_untouched", store->values.at("Other.zoom") == "300" && store->values.at("zoom") == "75"
                                               && store->values.at("App.theme") == "1", 1);
        b->write("", "nothing");
        b->write(nullptr, "nothing");
        P.eq("write.empty_key_ignored", store->writes.size() == 2 && b->size() == 3, 1);
        b->write("empty", "");
        P.eq("write.empty_value_is_a_value", held(*b, "empty") == "<>" && store->values.at("App.empty").empty(), 1);
        P.eq("write.none_unsaved", static_cast<int64_t>(b->unsavedCount()), 0);

        // ---- reload: what it reports --------------------------------------------------------------------------------
        P.eq("reload.same_store_no_change", b->reload(), 0);
        store->values["App.zoom"] = "100";
        P.eq("reload.changed_value_reported", b->reload() && held(*b, "zoom") == "<100>", 1);
        P.eq("reload.then_no_change", b->reload(), 0);
        store->values["App.historySpanTenths"] = "50";
        P.eq("reload.new_key_reported", b->reload() && held(*b, "historySpanTenths") == "<50>", 1);
        store->values.erase("App.theme");
        P.eq("reload.removed_key_reported", b->reload() && held(*b, "theme") == "(none)", 1);
        store->values["Other.zoom"] = "301";
        store->values["Elsewhere"] = "1";
        P.eq("reload.other_prefix_is_no_change", b->reload(), 0);
        store->values["App.empty"] = "";
        store->values.erase("App.zoom");
        store->values["App.zoom"] = "100";
        P.eq("reload.rewritten_same_is_no_change", b->reload(), 0);

        // The prefix is the backend's rule, not the storage's: a storage that hands over every key changes nothing.
        store->unfiltered = true;
        P.eq("reload.other_keys_handed_over_are_not_taken", !b->reload() && held(*b, "Other.zoom") == "(none)"
                                                                && held(*b, "Elsewhere") == "(none)"
                                                                && held(*b, "zoom") == "<100>", 1);
    }

    // ---- a storage that refuses a write ----------------------------------------------------------------------------
    {
        const auto store = std::make_shared<Store>();
        store->values = { { "P:zoom", "100" } };
        const auto b = backendOver(store, "P:");
        store->refuseWrites = true;
        b->write("zoom", "150");
        b->write("theme", "1");
        P.eq("refused.asked_the_storage", store->refused, 2);
        P.eq("refused.value_is_held", held(*b, "zoom") == "<150>" && held(*b, "theme") == "<1>", 1);
        P.eq("refused.storage_keeps_the_old", store->values.at("P:zoom") == "100" && store->values.size() == 1, 1);
        P.eq("refused.counted_unsaved", static_cast<int64_t>(b->unsavedCount()), 2);
        P.eq("refused.reload_keeps_it_no_change", !b->reload() && held(*b, "zoom") == "<150>"
                                                      && held(*b, "theme") == "<1>", 1);
        // Another tab's write of a key held here does not take the page's own choice back; its other keys arrive.
        store->values["P:zoom"] = "125";
        store->values["P:meterScaleDb"] = "24";
        P.eq("refused.reload_follows_other_keys", b->reload() && held(*b, "zoom") == "<150>"
                                                      && held(*b, "meterScaleDb") == "<24>", 1);
        // The storage takes writes again: the key is the storage's from the next write of it.
        store->refuseWrites = false;
        b->write("zoom", "175");
        P.eq("refused.later_write_is_stored", store->values.at("P:zoom") == "175" && b->unsavedCount() == 1, 1);
        store->values["P:zoom"] = "200";
        P.eq("refused.then_reload_follows_the_store", b->reload() && held(*b, "zoom") == "<200>"
                                                          && held(*b, "theme") == "<1>", 1);
    }

    // ---- a storage that cannot be read, and none at all ------------------------------------------------------------
    {
        const auto store = std::make_shared<Store>();
        store->values = { { "P:zoom", "100" } };
        store->unreadable = true;
        const auto b = backendOver(store, "P:");
        P.eq("unreadable.starts_empty", static_cast<int64_t>(b->size()), 0);
        b->write("zoom", "150");
        P.eq("unreadable.write_still_tried_and_held", store->values.at("P:zoom") == "150"
                                                          && held(*b, "zoom") == "<150>", 1);
        store->values["P:zoom"] = "125";
        P.eq("unreadable.reload_no_change_mirror_kept", !b->reload() && held(*b, "zoom") == "<150>", 1);
        store->unreadable = false;
        P.eq("unreadable.readable_again_follows", b->reload() && held(*b, "zoom") == "<125>", 1);

        WebPrefsBackend alone(nullptr, "P:");
        alone.write("zoom", "150");
        P.eq("nostorage.memory_only", held(alone, "zoom") == "<150>" && !alone.reload()
                                          && held(alone, "zoom") == "<150>" && alone.unsavedCount() == 1, 1);
    }

    // ---- an empty prefix: every key of the store -------------------------------------------------------------------
    {
        const auto store = std::make_shared<Store>();
        store->values = { { "zoom", "100" }, { "App.zoom", "150" } };
        const auto b = backendOver(store, "");
        P.eq("noprefix.every_key", b->size() == 2 && held(*b, "zoom") == "<100>" && held(*b, "App.zoom") == "<150>", 1);
        b->write("theme", "1");
        P.eq("noprefix.bare_key_stored", store->values.at("theme") == "1", 1);
    }

    // ---- under UiPreferences ----------------------------------------------------------------------------------------
    // The default store is never written here, but with JUCE it is opened: keep even that off the real one when the
    // caller (CTest sets it) gave no scratch directory.
    if (funkgui::env("PREFS_DIR") == nullptr)
    {
        std::error_code ec;
        std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        if (ec)
            base = ".";
        const std::string dir = (base / ("FunkGuiWebPrefs-" + std::to_string(::getpid()))).string();
        T::setEnv(FUNKGUI_ENV_PREFIX "PREFS_DIR", dir.c_str());
        funkgui::envReload();
    }
    {
        auto& prefs = UiPreferences::get();
        const auto store = std::make_shared<Store>();
        store->values = { { "FunkGui.theme", "1" }, { "FunkGui.uiZoom", "150" }, { "theme", "0" } };
        const uint32_t rev = prefs.revision();
        prefs.setBackend(backendOver(store, "FunkGui."));
        P.eq("prefs.install_reads_the_prefixed_theme", prefs.theme() == 1 && prefs.revision() == rev + 1, 1);
        P.eq("prefs.int_read", prefs.getInt("uiZoom", 100, 100, 175), 150);
        P.eq("prefs.install_writes_nothing", static_cast<int64_t>(store->writes.size()), 0);

        prefs.setInt("uiZoom", 125);
        prefs.setTheme(0);
        prefs.setInt("uiZoom", 125);                         // held already: no write
        const std::vector<std::pair<std::string, std::string>> want{ { "FunkGui.uiZoom", "125" },
                                                                     { "FunkGui.theme", "0" } };
        P.eq("prefs.written_through_as_decimal_once", store->writes == want && prefs.revision() == rev + 3, 1);

        // Another tab chose a theme and a zoom: reload() follows, one revision for the lot; again, none.
        store->values["FunkGui.theme"] = "1";
        store->values["FunkGui.uiZoom"] = "175";
        prefs.reload();
        P.eq("prefs.reload_follows_another_tab", prefs.theme() == 1 && prefs.getInt("uiZoom", 100, 100, 175) == 175
                                                     && prefs.revision() == rev + 4, 1);
        prefs.reload();
        P.eq("prefs.reload_unchanged_no_bump", prefs.revision(), rev + 4);

        // A private window: the choice is the page's while it lives, and a reload does not undo it.
        store->refuseWrites = true;
        prefs.setTheme(0);
        prefs.setInt("uiZoom", 100);
        const uint32_t afterRefused = prefs.revision();
        prefs.reload();
        P.eq("prefs.refused_write_survives_reload", prefs.theme() == 0 && prefs.getInt("uiZoom", 175, 100, 175) == 100
                                                        && prefs.revision() == afterRefused
                                                        && store->values.at("FunkGui.theme") == "1", 1);

        prefs.setBackend(nullptr);                           // the default store again; the test's is released
        P.eq("prefs.backend_released", static_cast<int64_t>(store.use_count()), 1);
    }
    return P.finish();
}

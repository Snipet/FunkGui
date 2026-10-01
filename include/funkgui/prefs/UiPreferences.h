#pragma once

#include <juce_data_structures/juce_data_structures.h>
#include <cstdint>
#include <memory>

namespace funkgui
{
    // Machine-wide UI preferences, shared by every instance in every host.
    //
    // Deliberately NOT part of the plugin state: the theme is a property of the
    // person sitting at the machine, not of the sound. Storing it in the state
    // tree meant a recalled preset — or a session from a collaborator — carried
    // its author's palette and silently restyled the user's screen.
    //
    // Backed by a properties file under
    //   ~/Library/Application Support/<PREFS_FOLDER>/preferences.settings
    // (on Linux, v0.11.0: ~/.config/<PREFS_FOLDER>/preferences.settings; the
    // $XDG_CONFIG_HOME variable is not read), which is a different file from the Standalone
    // wrapper's own settings.
    // <PREFS_FOLDER> is the product's (funkgui_configure_product PREFS_FOLDER,
    // 02 §1.8), so each product keeps its own preferences (Q7).
    //
    // Generic integer keys (02 §5.9, G6): any product preference that is an
    // int (FCompressor: meterScaleDb, historySpanTenths) lives in the same
    // file, as <VALUE name="<key>" val="<int>"/> beside the theme, with
    // setTheme()'s semantics: written through at once, a write of the value
    // already held is a no-op, and every change bumps revision(). "theme" is
    // the theme's own key: getInt/setInt on it go through theme()/setTheme().
    //
    // Message thread only. Nothing here reads the file except the first get()
    // and reload(): the getters read the in-memory copy.
    class UiPreferences
    {
    public:
        static UiPreferences& get();

        int  theme() const noexcept { return theme_; }
        void setTheme(int idx);

        // The int held under `key`, clamped to [lo, hi] (bounds given in
        // either order); `fallback` (clamped the same way) when the key is
        // missing or its value is not a decimal integer that fits an int.
        // A null or empty key reads as the fallback. (G6 addition.)
        int  getInt(const char* key, int fallback, int lo, int hi) const;

        // Stores `value` under `key` and saves immediately, ++revision (HR
        // setTheme semantics); nothing at all when the key already holds
        // exactly this value. A null or empty key is ignored. (G6 addition.)
        void setInt(const char* key, int value);

        // Re-read the file. The value held here is a snapshot from when this
        // process first asked; another host on the same machine may have
        // changed the file since. Called when an editor opens, which is the
        // moment a stale palette would be shown. Bumps revision() when any
        // key changed.
        void reload();

        // Bumped on every change. Editors watch this so that switching the
        // theme in one open window updates the others in the same process;
        // across processes the file is read when an editor opens.
        uint32_t revision() const noexcept { return revision_; }

        // The file this store reads and writes: <ENV_PREFIX>PREFS_DIR's
        // preferences.settings when that variable was set at the first get(),
        // else defaultFile(). (G6 addition.)
        juce::File file() const;

        // ~/Library/Application Support/<PREFS_FOLDER>/preferences.settings for
        // the product this source is compiled into; on Linux ~/.config/
        // <PREFS_FOLDER>/preferences.settings ($XDG_CONFIG_HOME is not read).
        // (G6 addition; Linux v0.11.0.)
        static juce::File defaultFile();

    private:
        UiPreferences();

        std::unique_ptr<juce::PropertiesFile> file_;
        int      theme_ = 0;
        uint32_t revision_ = 0;
    };
}

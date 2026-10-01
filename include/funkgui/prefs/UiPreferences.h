#pragma once

#include <funkgui/core/HasJuce.h>

#if FUNKGUI_HAS_JUCE
  #include <juce_data_structures/juce_data_structures.h>
#endif
#include <cstdint>
#include <memory>
#include <string>

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
    //
    // Storage backends (v0.12.0). The store behind the keys is a Backend. With
    // JUCE (FUNKGUI_HAS_JUCE) the default one is the properties file above:
    // format, path and behaviour as they always were. Without JUCE the default
    // holds the keys in memory for the life of the process, and a host gives
    // the class somewhere durable with setBackend() (the browser: localStorage).
    // file() and defaultFile() exist only with JUCE.
    class UiPreferences
    {
    public:
        static UiPreferences& get();

        // Where the keys live (v0.12.0): text values under text keys. Keys are
        // compared exactly as given; "theme" is the theme's.
        class Backend
        {
        public:
            virtual ~Backend() = default;

            // The text held under `key` into `value`; false, and `value`
            // untouched, when the store has no such key.
            virtual bool read(const char* key, std::string& value) const = 0;

            // Stores `value` under `key` and makes it durable before
            // returning: a preference is written through, never batched.
            virtual void write(const char* key, const std::string& value) = 0;

            // Re-reads the store from wherever it lives (another process may
            // have written it); true when any key or value changed.
            virtual bool reload() = 0;
        };

        // Replaces the store (v0.12.0): the theme is read from the new one and
        // revision() is bumped, so open editors follow. nullptr restores the
        // default backend (the properties file with JUCE, a fresh in-memory
        // store without). The values of the old store are not copied.
        void setBackend(std::unique_ptr<Backend>);

        // A store that lives in memory only (v0.12.0): the default without
        // JUCE, and a sandbox for a test or a probe in any build. reload()
        // reports no change.
        static std::unique_ptr<Backend> memoryBackend();

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

#if FUNKGUI_HAS_JUCE
        // The file this store reads and writes: <ENV_PREFIX>PREFS_DIR's
        // preferences.settings when that variable was set at the first get(),
        // else defaultFile(). (G6 addition.) A null File when setBackend()
        // replaced the properties file with another store (v0.12.0).
        juce::File file() const;

        // ~/Library/Application Support/<PREFS_FOLDER>/preferences.settings for
        // the product this source is compiled into; on Linux ~/.config/
        // <PREFS_FOLDER>/preferences.settings ($XDG_CONFIG_HOME is not read).
        // (G6 addition; Linux v0.11.0.)
        static juce::File defaultFile();
#endif

    private:
        UiPreferences();

        int readTheme() const;                           // the store's theme, clamped to the themes there are

        std::unique_ptr<Backend> backend_;               // never null after construction
        int      theme_ = 0;
        uint32_t revision_ = 0;
    };
}

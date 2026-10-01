#include <funkgui/prefs/UiPreferences.h>

#include <funkgui/core/Config.h>
#include <funkgui/core/Env.h>
#include <funkgui/core/Theme.h>

#include <charconv>
#include <string>
#include <system_error>
#include <utility>

namespace funkgui
{
    namespace
    {
        constexpr const char* kThemeKey = "theme";

        juce::PropertiesFile::Options storeOptions()
        {
            juce::PropertiesFile::Options o;
            o.applicationName     = "preferences";
            o.filenameSuffix      = "settings";
            o.folderName          = FUNKGUI_PREFS_FOLDER;
            o.osxLibrarySubFolder = "Application Support";
            o.commonToAllUsers    = false;
            o.doNotSave           = false;
            return o;
        }

        // The store's file when <ENV_PREFIX>PREFS_DIR is not set: JUCE's default for these options on macOS,
        // ~/Library/Application Support/<folder>/preferences.settings. On Linux JUCE's default is ~/<folder>/, a
        // visible folder in the home directory, so FunkGui uses the configuration directory JUCE resolves instead,
        // ~/.config, where FunkPresets keeps its database too (v0.11.0). JUCE 8.0.4 looks for an XDG_CONFIG_HOME line
        // in ~/.config/user-dirs.dirs, which holds none, and never reads the environment variable: a relocated
        // $XDG_CONFIG_HOME is not followed yet.
        juce::File defaultStoreFile()
        {
           #if JUCE_LINUX || JUCE_BSD
            return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                       .getChildFile(FUNKGUI_PREFS_FOLDER).getChildFile("preferences.settings");
           #else
            return storeOptions().getDefaultFile();
           #endif
        }

        // The store's keys ignore case (juce::PropertiesFile::Options::ignoreCaseOfKeyNames), so "Theme" is the theme.
        bool isThemeKey(const char* key)
        {
            return juce::String(key).equalsIgnoreCase(kThemeKey);
        }

        int clampTo(int v, int lo, int hi) noexcept
        {
            return v < lo ? lo : (v > hi ? hi : v);
        }

        // A decimal integer that fits an int ("-12", "48"), and nothing else: juce::String::getIntValue() reads "48abc"
        // as 48 and "abc" as 0, which would turn a damaged file into a real setting.
        bool parseInt(const juce::String& s, int& out)
        {
            const std::string t = s.toStdString();
            if (t.empty())
                return false;
            const char* first = t.data();
            const char* last = t.data() + t.size();
            const auto [end, ec] = std::from_chars(first, last, out);
            return ec == std::errc{} && end == last;
        }
    }

    UiPreferences& UiPreferences::get()
    {
        // Deliberately never destroyed, for the same reason the frame pump
        // isn't: a plugin binary is unloaded while other statics are being
        // torn down in an order nothing here controls, and a PropertiesFile
        // destructor writes to disk. Every change is saved eagerly instead, so
        // there is nothing left to flush at shutdown.
        static UiPreferences* prefs = new UiPreferences();
        return *prefs;
    }

    juce::File UiPreferences::defaultFile()
    {
        return defaultStoreFile();
    }

    UiPreferences::UiPreferences()
    {
        const juce::PropertiesFile::Options o = storeOptions();

        // <ENV_PREFIX>PREFS_DIR redirects the store to a directory of the caller's
        // choosing. It exists so the preferences harness can run against a
        // scratch file instead of the real one — which it used to write, with
        // no teardown, and every editor on the machine then read.
        if (const char* dir = funkgui::env("PREFS_DIR"))
            file_ = std::make_unique<juce::PropertiesFile>(
                juce::File(juce::String(dir)).getChildFile("preferences.settings"), o);
        else
            file_ = std::make_unique<juce::PropertiesFile>(defaultStoreFile(), o);
        theme_ = juce::jlimit(0, Theme::kCount - 1,
                              file_->getIntValue(kThemeKey, 0));
    }

    juce::File UiPreferences::file() const
    {
        return file_ != nullptr ? file_->getFile() : juce::File();
    }

    void UiPreferences::reload()
    {
        if (file_ == nullptr) return;
        const juce::StringPairArray before = file_->getAllProperties();
        file_->reload();
        theme_ = juce::jlimit(0, Theme::kCount - 1,
                              file_->getIntValue(kThemeKey, 0));
        // Any key, not just the theme: an editor follows every preference.
        if (file_->getAllProperties() != before) ++revision_;
    }

    void UiPreferences::setTheme(int idx)
    {
        const int clamped = juce::jlimit(0, Theme::kCount - 1, idx);
        if (clamped == theme_) return;

        theme_ = clamped;
        ++revision_;

        if (file_ != nullptr)
        {
            file_->setValue(kThemeKey, theme_);
            // Written through immediately rather than at destruction: a host
            // that is force-quit, or a plugin unloaded mid-teardown, must not
            // lose the choice the user just made. Two hosts open at once are
            // last-writer-wins, which is the right semantics for a preference.
            file_->saveIfNeeded();
        }
    }

    int UiPreferences::getInt(const char* key, int fallback, int lo, int hi) const
    {
        if (lo > hi)
            std::swap(lo, hi);
        if (key == nullptr || key[0] == '\0')
            return clampTo(fallback, lo, hi);
        if (isThemeKey(key))
            return clampTo(theme_, lo, hi);
        int v = fallback;
        if (file_ != nullptr && file_->containsKey(key) && !parseInt(file_->getValue(key), v))
            v = fallback;
        return clampTo(v, lo, hi);
    }

    void UiPreferences::setInt(const char* key, int value)
    {
        if (key == nullptr || key[0] == '\0')
            return;
        if (isThemeKey(key))
        {
            setTheme(value);                             // the theme keeps its clamp and its cached copy
            return;
        }
        if (file_ == nullptr)
            return;
        const juce::String text(value);
        if (file_->containsKey(key) && file_->getValue(key) == text)
            return;                                      // already held: no write, no revision
        file_->setValue(key, text);
        file_->saveIfNeeded();                           // written through, as setTheme()
        ++revision_;
    }
}

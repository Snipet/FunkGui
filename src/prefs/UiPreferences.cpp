#include "UiPreferences.h"
#include "Theme.h"

#include <cstdlib>

namespace funkgui
{
    namespace
    {
        constexpr const char* kThemeKey = "theme";
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

    UiPreferences::UiPreferences()
    {
        juce::PropertiesFile::Options o;
        o.applicationName     = "preferences";
        o.filenameSuffix      = "settings";
        o.folderName          = FUNKGUI_PREFS_FOLDER;
        o.osxLibrarySubFolder = "Application Support";
        o.commonToAllUsers    = false;
        o.doNotSave           = false;

        // <ENV_PREFIX>PREFS_DIR redirects the store to a directory of the caller's
        // choosing. It exists so the preferences harness can run against a
        // scratch file instead of the real one — which it used to write, with
        // no teardown, and every editor on the machine then read.
        if (const char* dir = funkgui::env("PREFS_DIR"))
            file_ = std::make_unique<juce::PropertiesFile>(
                juce::File(juce::String(dir)).getChildFile("preferences.settings"), o);
        else
            file_ = std::make_unique<juce::PropertiesFile>(o);
        theme_ = juce::jlimit(0, Theme::kCount - 1,
                              file_->getIntValue(kThemeKey, 0));
    }

    void UiPreferences::reload()
    {
        if (file_ == nullptr) return;
        file_->reload();
        const int fresh = juce::jlimit(0, Theme::kCount - 1,
                                       file_->getIntValue(kThemeKey, 0));
        if (fresh != theme_) { theme_ = fresh; ++revision_; }
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
}

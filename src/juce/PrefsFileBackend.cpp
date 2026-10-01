#include <funkgui/prefs/UiPreferences.h>

#include "../prefs/PrefsBackend.h"

#include <funkgui/core/Config.h>
#include <funkgui/core/Env.h>

#include <memory>
#include <string>

// UiPreferences' properties-file backend (v0.12.0: the JUCE half of src/prefs/UiPreferences.cpp, which now holds the
// class's JUCE-free logic). The file, its path, its options and every juce::PropertiesFile call are the ones
// UiPreferences made directly until v0.11.1: getValue / containsKey to read, setValue then saveIfNeeded to write,
// reload and a comparison of all properties to see another process's change. Compiled only with FUNKGUI_WITH_JUCE
// (src/juce/**).

namespace funkgui
{
    namespace
    {
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

        class FileBackend final : public UiPreferences::Backend
        {
        public:
            // <ENV_PREFIX>PREFS_DIR redirects the store to a directory of the caller's
            // choosing. It exists so the preferences harness can run against a
            // scratch file instead of the real one — which it used to write, with
            // no teardown, and every editor on the machine then read.
            FileBackend()
                : file_(storeFile(), storeOptions())
            {
            }

            bool read(const char* key, std::string& value) const override
            {
                if (!file_.containsKey(key))
                    return false;
                value = file_.getValue(key).toStdString();
                return true;
            }

            void write(const char* key, const std::string& value) override
            {
                file_.setValue(key, juce::String::fromUTF8(value.c_str()));
                file_.saveIfNeeded();
            }

            bool reload() override
            {
                const juce::StringPairArray before = file_.getAllProperties();
                file_.reload();
                return file_.getAllProperties() != before;
            }

            juce::File file() const { return file_.getFile(); }

        private:
            static juce::File storeFile()
            {
                if (const char* dir = funkgui::env("PREFS_DIR"))
                    return juce::File(juce::String(dir)).getChildFile("preferences.settings");
                return defaultStoreFile();
            }

            juce::PropertiesFile file_;
        };
    }

    std::unique_ptr<UiPreferences::Backend> detail::makeDefaultPrefsBackend()
    {
        return std::make_unique<FileBackend>();
    }

    juce::File UiPreferences::defaultFile()
    {
        return defaultStoreFile();
    }

    juce::File UiPreferences::file() const
    {
        const auto* fileBackend = dynamic_cast<const FileBackend*>(backend_.get());
        return fileBackend != nullptr ? fileBackend->file() : juce::File();
    }
}

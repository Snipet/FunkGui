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
    // which is a different file from the Standalone wrapper's own settings.
    class UiPreferences
    {
    public:
        static UiPreferences& get();

        int  theme() const noexcept { return theme_; }
        void setTheme(int idx);

        // Re-read the file. The value held here is a snapshot from when this
        // process first asked; another host on the same machine may have
        // changed the file since. Called when an editor opens, which is the
        // moment a stale palette would be shown.
        void reload();

        // Bumped on every change. Editors watch this so that switching the
        // theme in one open window updates the others in the same process;
        // across processes the file is read when an editor opens.
        uint32_t revision() const noexcept { return revision_; }

    private:
        UiPreferences();

        std::unique_ptr<juce::PropertiesFile> file_;
        int      theme_ = 0;
        uint32_t revision_ = 0;
    };
}

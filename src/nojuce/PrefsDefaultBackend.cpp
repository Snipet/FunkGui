#include "../prefs/PrefsBackend.h"

// UiPreferences' default store without JUCE (v0.12.0): in memory, for the life of the process. A host with somewhere
// durable to keep the keys (the browser's localStorage) installs its own with UiPreferences::setBackend(). Compiled
// only with FUNKGUI_WITH_JUCE=OFF (src/nojuce/**); src/juce/PrefsFileBackend.cpp is the properties file.

namespace funkgui
{
    std::unique_ptr<UiPreferences::Backend> detail::makeDefaultPrefsBackend()
    {
        return UiPreferences::memoryBackend();
    }
}

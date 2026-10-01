#pragma once

// Internal to FunkGui's sources (not API): the default store of funkgui::UiPreferences (v0.12.0). One definition per
// build: src/juce/PrefsFileBackend.cpp (the juce::PropertiesFile every release so far has written) with
// FUNKGUI_WITH_JUCE, src/nojuce/PrefsDefaultBackend.cpp (UiPreferences::memoryBackend()) without.

#include <funkgui/prefs/UiPreferences.h>

#include <memory>

namespace funkgui::detail
{
    std::unique_ptr<UiPreferences::Backend> makeDefaultPrefsBackend();
}

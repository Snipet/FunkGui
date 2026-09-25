#pragma once

// The factory bank, compiled into the plugin. Also what hosts see as the
// plugin's programs (AU factory presets, VST3 program list): index 0 is
// "Init", every parameter at its default — the state a fresh instance is in.

#include "PresetTypes.h"

namespace hrvb::presets
{
    const std::vector<Preset>& factoryBank();
    int factoryBankRevision();                    // bump whenever the bank's contents change
    const Preset* findFactory(const juce::String& uuid);
    int factoryIndexOf(const juce::String& uuid); // -1 if not a factory preset
}

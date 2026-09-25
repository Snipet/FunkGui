#pragma once

// A preset as a file on disk, for sharing and backup: `.hrvbpreset`, XML.
// Carries identity, metadata and parameter values; never tags (personal) and
// never timestamps (the importing store assigns its own).

#include "PresetTypes.h"
#include <optional>

namespace hrvb::presets::PresetFile
{
    constexpr const char* kExtension = ".hrvbpreset";

    juce::String toXmlString(const Preset&);
    std::optional<Preset> fromXmlString(const juce::String&, juce::String* error = nullptr);

    bool write(const Preset&, const juce::File&, juce::String* error = nullptr);
    std::optional<Preset> read(const juce::File&, juce::String* error = nullptr);

    // A filename-safe version of a preset name.
    juce::String safeFileName(const juce::String& presetName);
}

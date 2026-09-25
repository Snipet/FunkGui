#pragma once

// A preset as a file on disk, for sharing and backup: `<ProductConfig::fileExtension>`, XML (01 §9.2):
//
//   <FCompressorPreset format="1" plugin="FCompressor" uuid="…" name="…" category="…" author="…" notes="…">
//     <ATTR key="modeId" value="fet-76"/>
//     <PARAM id="thr" value="-24"/>
//     …
//   </FCompressorPreset>
//
// The root element is ProductConfig::xmlRoot and plugin="…" its productName. A file carries identity, metadata, the
// attributes and the parameter values; never tags (personal) and never timestamps (the importing store assigns its
// own). (HardwareReverb Source/presets/PresetFile.h; G8: the product literals come from ProductConfig, and the
// attributes are ATTR children.)

#include <funkgui/presets/PresetTypes.h>
#include <optional>

namespace funkgui::presets::PresetFile
{
    // Every function refuses (empty string, nullopt or false, with *error set) when !config.isValid().
    juce::String toXmlString(const ProductConfig& config, const Preset&);
    std::optional<Preset> fromXmlString(const ProductConfig& config, const juce::String&, juce::String* error = nullptr);

    bool write(const ProductConfig& config, const Preset&, const juce::File&, juce::String* error = nullptr);
    std::optional<Preset> read(const ProductConfig& config, const juce::File&, juce::String* error = nullptr);

    // A filename-safe version of a preset name (without the extension).
    juce::String safeFileName(const juce::String& presetName);
}

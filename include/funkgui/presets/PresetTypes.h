#pragma once

// FunkPresets (FunkGui::presets, namespace funkgui::presets): the preset data layer every product shares
// (FCompressor docs/design/01-core-contracts.md §9.2; 02 §1.2). It does not depend on the GUI targets.
//
// Shared types for the preset system. This header is the contract between the store (PresetStore, SQLite), the
// product's factory bank, the per-instance manager (PresetManager), the file format (PresetFile) and the product's
// browser — change it only by adding. (HardwareReverb Source/presets/PresetTypes.h; G8 added Attribute and
// ProductConfig.)
//
// Values are stored in PLAIN units (milliseconds, hertz, 0..1 where the parameter itself is 0..1), keyed by parameter
// id, never as normalised host values: a stored preset then survives a change to a parameter's range or skew. A
// parameter a preset does not mention loads at its default — which is what keeps presets saved before a parameter
// existed deterministic after it is added.

#include <juce_core/juce_core.h>
#include <cstdint>
#include <vector>

namespace funkgui::presets
{
    // ---- product identity -------------------------------------------------------------------------------------------
    // Everything HardwareReverb's preset code spelled as a literal (01 §9.2, B §4.1). One constant per product, given
    // to PresetStore and PresetFile; FunkPresets itself names no product.
    struct ProductConfig
    {
        juce::String productName;     // "FCompressor": ~/Library/Application Support/<productName>/Presets.db, the
                                      // plugin="…" attribute of an exported file, and the product named in messages
        juce::String fileExtension;   // ".fcmppreset" (with the dot)
        juce::String xmlRoot;         // "FCompressorPreset": the root element of an exported file
        juce::String dbEnvVar;        // "FCMP_PRESETS_DB": the FULL name of the variable whose value, when set and not
                                      // empty, replaces the database path (harnesses, frame captures); empty = none

        // productName is one folder name (not empty, no surrounding space, no '/', ':' or '\', not "." or ".."),
        // fileExtension is '.' followed by ASCII letters and digits, xmlRoot is a valid XML element name. A store given
        // an invalid configuration runs in memory; PresetFile refuses to read or write with one.
        bool isValid() const
        {
            const bool nameOk = productName.isNotEmpty() && productName.trim() == productName
                             && productName != "." && productName != ".." && !productName.containsAnyOf("/:\\");
            const bool extOk = fileExtension.length() >= 2 && fileExtension.startsWithChar('.')
                            && fileExtension.substring(1).containsOnly(
                                   "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
            return nameOk && extOk && juce::XmlElement::isValidXmlName(xmlRoot);
        }
    };

    // ---- colour tags ------------------------------------------------------------------------------------------------
    // Finder-style: a fixed palette, any subset per preset. A tagged preset is a favourite and its colour says which
    // kind — the user decides what red means, and may name it (PresetStore::setTagLabel).
    enum class Tag : uint8_t { red = 0, orange, yellow, green, blue, purple, grey };
    constexpr int kNumTags = 7;

    using TagMask = uint8_t;                               // bit i = Tag(i)
    constexpr TagMask tagBit(Tag t) noexcept { return static_cast<TagMask>(1u << static_cast<unsigned>(t)); }
    constexpr TagMask kAllTags = 0x7f;

    inline const char* defaultTagName(Tag t) noexcept
    {
        constexpr const char* names[kNumTags] = { "Red", "Orange", "Yellow", "Green", "Blue", "Purple", "Grey" };
        return names[static_cast<int>(t) % kNumTags];
    }

    // Payload format. Bump only if the MEANING of a stored value changes; adding a parameter does not need it (absent =
    // default, by rule).
    constexpr int kFormat = 1;

    struct ParamValue
    {
        juce::String id;
        float value = 0.0f;          // plain units
    };

    // A product-defined string attribute of a preset (01 §9.2; G8). FCompressor: { "modeId", "fet-76" },
    // { "modeRev", "1" }. Keys are non-empty and unique per preset (the first of a repeated key wins, as for params);
    // a value is any text, the empty string included. Stored, exported, imported and restored with the preset;
    // FunkPresets never interprets one.
    struct Attribute
    {
        juce::String key, value;
    };

    struct Preset
    {
        juce::String uuid;           // stable identity; factory UUIDs are fixed in source
        juce::String name;
        juce::String category;       // free text, grouped case-insensitively ("Hall", "Room", ...)
        juce::String author;
        juce::String notes;
        bool   isFactory = false;
        int    format    = kFormat;
        std::vector<ParamValue> params;
        juce::int64 createdMs = 0, modifiedMs = 0, lastUsedMs = 0;
        TagMask tags = 0;            // personal: never in a factory definition, never exported
        std::vector<Attribute> attributes;   // G8: product-defined (Attribute)

        const ParamValue* find(const juce::String& id) const noexcept
        {
            for (const auto& p : params) if (p.id == id) return &p;
            return nullptr;
        }
        bool isValid() const noexcept { return uuid.isNotEmpty() && name.isNotEmpty(); }

        // The attribute named `key` (the first, if the key repeats), or nullptr. (G8.)
        const Attribute* attr(const juce::String& key) const noexcept
        {
            for (const auto& a : attributes) if (a.key == key) return &a;
            return nullptr;
        }

        // Sets the first attribute named `key` to `value`, or appends one. An empty key is ignored. (G8.)
        void setAttr(const juce::String& key, const juce::String& value)
        {
            if (key.isEmpty()) return;
            for (auto& a : attributes)
                if (a.key == key) { a.value = value; return; }
            attributes.push_back({ key, value });
        }
    };

    enum class Source : uint8_t { all, factory, user };
    enum class Sort   : uint8_t { name, category, recent, created };

    struct Query
    {
        juce::String text;           // case-insensitive substring of name, category, author or notes
        juce::String category;       // empty = any; matched case-insensitively
        Source  source    = Source::all;
        TagMask anyOfTags = 0;       // 0 = no tag filter; else presets carrying at least one of these
        Sort    sort      = Sort::name;
    };
}

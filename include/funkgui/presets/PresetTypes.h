#pragma once

// Shared types for the preset system. This header is the contract between the
// store (PresetStore, SQLite), the factory bank (FactoryPresets), the
// per-instance manager (PresetManager) and the editor's browser — change it
// only by adding.
//
// Values are stored in PLAIN units (milliseconds, hertz, 0..1 where the
// parameter itself is 0..1), keyed by parameter id, never as normalised host
// values: a stored preset then survives a change to a parameter's range or
// skew. A parameter a preset does not mention loads at its default — which is
// what keeps presets saved before a parameter existed deterministic after it
// is added.

#include <juce_core/juce_core.h>
#include <cstdint>
#include <vector>

namespace hrvb::presets
{
    // ---- colour tags ------------------------------------------------------
    // Finder-style: a fixed palette, any subset per preset. A tagged preset is
    // a favourite and its colour says which kind — the user decides what red
    // means, and may name it (PresetStore::setTagLabel).
    enum class Tag : uint8_t { red = 0, orange, yellow, green, blue, purple, grey };
    constexpr int kNumTags = 7;

    using TagMask = uint8_t;                               // bit i = Tag(i)
    constexpr TagMask tagBit(Tag t) noexcept { return static_cast<TagMask>(1u << static_cast<unsigned>(t)); }
    constexpr TagMask kAllTags = 0x7f;

    inline const char* defaultTagName(Tag t) noexcept
    {
        static const char* names[kNumTags] = { "Red", "Orange", "Yellow", "Green", "Blue", "Purple", "Grey" };
        return names[static_cast<int>(t) % kNumTags];
    }

    // Payload format. Bump only if the MEANING of a stored value changes;
    // adding a parameter does not need it (absent = default, by rule).
    constexpr int kFormat = 1;

    struct ParamValue
    {
        juce::String id;
        float value = 0.0f;          // plain units
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

        const ParamValue* find(const juce::String& id) const noexcept
        {
            for (const auto& p : params) if (p.id == id) return &p;
            return nullptr;
        }
        bool isValid() const noexcept { return uuid.isNotEmpty() && name.isNotEmpty(); }
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

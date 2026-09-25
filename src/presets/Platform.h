#pragma once

// FunkPresets-private. The two places the preset layer needs the operating system: reading a float in the "C" locale,
// and folding text for keys. Everything else in presets/ is portable C++ and JUCE. Both live in Platform.cpp so that
// <xlocale.h>, CoreFoundation and <windows.h> are included by one translation unit only (<windows.h> defines min, max
// and small as macros). (HardwareReverb Source/presets/Platform.h.)

#include <juce_core/juce_core.h>

namespace funkgui::presets::detail
{
    // strtof in the "C" locale whatever the host set the process locale to: a host running under de_DE must not read
    // "0.15" as 0. The text is written with std::to_chars (shortest exact), and strtof on it round trips bit-exactly
    // (HardwareReverb's PresetProbe measured it; fg.presets.file checks it); strtod then a narrowing cast does not, for
    // some values.
    float strtofC(const char* s, char** end);

    // Folding for the database's *_key columns. Locale-independent on purpose: every process that opens the file must
    // compute the same key for the same name, whatever locale its host set. The result is NFC.
    enum FoldFlags
    {
        foldCase       = 1,   // case-insensitive (the name and category keys)
        foldDiacritics = 2,   // "Ä" sorts with "A" (the sort key)
        foldWidth      = 4,   // full-width forms sort with ASCII (the sort key)
    };
    juce::String foldText(const juce::String& s, int flags);
}

#pragma once

// The two places the preset layer needs the operating system: reading a float
// in the "C" locale, and folding text for keys. Everything else in presets/ is
// portable C++ and JUCE. The folding lives in Platform.cpp so that
// CoreFoundation and <windows.h> are included by one translation unit only
// (<windows.h> defines min, max and small as macros).

#include <juce_core/juce_core.h>

#if defined(_WIN32)
  #include <locale.h>
  #include <stdlib.h>
#elif defined(__APPLE__)
  #include <xlocale.h>
#else
  #include <locale.h>
  #include <stdlib.h>
#endif

namespace hrvb::presets
{
    // strtof in the "C" locale whatever the host set the process locale to:
    // a host running under de_DE must not read "0.15" as 0. The text is
    // written with std::to_chars (shortest exact), and strtof on it round
    // trips bit-exactly (PresetProbe measures it); strtod then a narrowing
    // cast does not, for some values.
    inline float strtofC(const char* s, char** end)
    {
       #if defined(_WIN32)
        static const _locale_t loc = _create_locale(LC_ALL, "C");
        return _strtof_l(s, end, loc);
       #else
        static const locale_t loc = newlocale(LC_ALL_MASK, "C", nullptr);
        return strtof_l(s, end, loc);
       #endif
    }

    // Folding for the database's *_key columns. Locale-independent on
    // purpose: every process that opens the file must compute the same key
    // for the same name, whatever locale its host set. The result is NFC.
    enum FoldFlags
    {
        foldCase       = 1,   // case-insensitive (the name and category keys)
        foldDiacritics = 2,   // "Ä" sorts with "A" (the sort key)
        foldWidth      = 4,   // full-width forms sort with ASCII (the sort key)
    };
    juce::String foldText(const juce::String& s, int flags);
}

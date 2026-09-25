#include "Platform.h"

// FunkPresets-private (HardwareReverb Source/presets/Platform.cpp; G8: namespace, strtofC moved here from the header).
//
// Why the folded *_key columns exist rather than COLLATE NOCASE and LIKE:
// SQLite's NOCASE and LIKE fold ASCII only, so "Äther" and "äther" would be
// two different names to the unique index while the UI calls them the same.
// juce's toLowerCase is not a substitute: it is towlower(), which on macOS
// folds only ASCII in the "C" locale (measured: towlower(U+00C4) = U+00C4) and
// whatever the HOST set the locale to otherwise, so two processes would
// compute different keys for the same name. If a fold ever changes, the keys
// in existing files are stale: that is a migration step that recomputes
// them (PresetStore.cpp, the ladder).
//
// Keys only ever meet keys computed on the same operating system: the
// database is per user, per machine. The two implementations below agree on
// what the UI needs (case, NFC, diacritics and full-width forms for sorting)
// and may differ at the edges of Unicode case folding (CoreFoundation folds
// "ß" to "ss"; LCMapStringEx lower-cases it to itself).

#if defined(__APPLE__)
  #include <CoreFoundation/CoreFoundation.h>
  #include <cstring>
  #include <xlocale.h>
#elif defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <locale.h>
  #include <stdlib.h>
  #include <string>
  #include <vector>
#else
  #include <locale.h>
  #include <stdlib.h>
#endif

namespace funkgui::presets::detail
{
    float strtofC(const char* s, char** end)
    {
       #if defined(_WIN32)
        static const _locale_t loc = _create_locale(LC_ALL, "C");
        return _strtof_l(s, end, loc);
       #else
        static const locale_t loc = newlocale(LC_ALL_MASK, "C", nullptr);
        return strtof_l(s, end, loc);
       #endif
    }

#if defined(__APPLE__)

    // CFStringFold with a NULL locale is the Unicode case fold,
    // locale-independent; NFC afterwards makes a decomposed "e + combining
    // acute" equal the precomposed one the keyboard types.
    juce::String foldText(const juce::String& s, int flags)
    {
        if (s.isEmpty()) return {};
        CFStringCompareFlags cf = 0;
        if ((flags & foldCase) != 0)       cf |= kCFCompareCaseInsensitive;
        if ((flags & foldDiacritics) != 0) cf |= kCFCompareDiacriticInsensitive;
        if ((flags & foldWidth) != 0)      cf |= kCFCompareWidthInsensitive;

        const char* utf8 = s.toRawUTF8();
        CFStringRef src = CFStringCreateWithBytes(kCFAllocatorDefault, (const UInt8*) utf8,
                                                  (CFIndex) std::strlen(utf8), kCFStringEncodingUTF8, false);
        if (src == nullptr) return s.toLowerCase();
        CFMutableStringRef m = CFStringCreateMutableCopy(kCFAllocatorDefault, 0, src);
        CFRelease(src);
        if (m == nullptr) return s.toLowerCase();
        CFStringFold(m, cf, nullptr);
        CFStringNormalize(m, kCFStringNormalizationFormC);

        const CFIndex len = CFStringGetLength(m);
        CFIndex bytes = 0;
        CFStringGetBytes(m, CFRangeMake(0, len), kCFStringEncodingUTF8, 0, false, nullptr, 0, &bytes);
        juce::HeapBlock<char> buf((size_t) bytes + 1, true);
        CFStringGetBytes(m, CFRangeMake(0, len), kCFStringEncodingUTF8, 0, false,
                         (UInt8*) buf.get(), bytes, nullptr);
        CFRelease(m);
        return juce::String::fromUTF8(buf.get(), (int) bytes);
    }

#elif defined(_WIN32)

    namespace
    {
        // NormalizeString's first answer is an estimate; the documented
        // pattern is to retry with the size a short buffer reports (as a
        // negative return with ERROR_INSUFFICIENT_BUFFER).
        std::wstring normalise(NORM_FORM form, const std::wstring& in)
        {
            if (in.empty()) return in;
            int n = NormalizeString(form, in.c_str(), (int) in.size(), nullptr, 0);
            for (int attempt = 0; attempt < 4 && n > 0; ++attempt)
            {
                std::wstring out((size_t) n, L'\0');
                const int got = NormalizeString(form, in.c_str(), (int) in.size(), out.data(), n);
                if (got > 0) { out.resize((size_t) got); return out; }
                if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) break;
                n = -got;
            }
            return in;   // invalid UTF-16 in, or the API refused: fold what we have
        }

        // The invariant locale, so the host's locale can never change a key.
        std::wstring lowerCase(const std::wstring& in)
        {
            if (in.empty()) return in;
            const DWORD how = LCMAP_LOWERCASE | LCMAP_LINGUISTIC_CASING;
            const int n = LCMapStringEx(LOCALE_NAME_INVARIANT, how, in.c_str(), (int) in.size(),
                                        nullptr, 0, nullptr, nullptr, 0);
            if (n <= 0) return in;
            std::wstring out((size_t) n, L'\0');
            if (LCMapStringEx(LOCALE_NAME_INVARIANT, how, in.c_str(), (int) in.size(),
                              out.data(), n, nullptr, nullptr, 0) <= 0)
                return in;
            return out;
        }

        // Decompose, then drop the combining marks: "Ä" -> "A" + U+0308 -> "A".
        // Nonspacing marks only; C3_DIACRITIC would also take spacing
        // characters such as '^' and '`' out of names.
        std::wstring stripDiacritics(const std::wstring& in)
        {
            const std::wstring d = normalise(NormalizationD, in);
            if (d.empty()) return d;
            std::vector<WORD> type(d.size(), 0);
            if (!GetStringTypeW(CT_CTYPE3, d.c_str(), (int) d.size(), type.data())) return d;
            std::wstring out;
            out.reserve(d.size());
            for (size_t i = 0; i < d.size(); ++i)
                if ((type[i] & C3_NONSPACING) == 0) out += d[i];
            return out;
        }

        // Full-width ASCII (U+FF01..U+FF5E) and the ideographic space: the
        // forms a Japanese or Chinese keyboard produces for Latin text.
        std::wstring narrow(std::wstring s)
        {
            for (auto& c : s)
            {
                if (c >= 0xFF01 && c <= 0xFF5E) c = (wchar_t) (c - 0xFEE0);
                else if (c == 0x3000)           c = L' ';
            }
            return s;
        }
    }

    juce::String foldText(const juce::String& s, int flags)
    {
        if (s.isEmpty()) return {};
        std::wstring w = s.toWideCharPointer();          // UTF-16 on Windows
        if ((flags & foldCase) != 0)       w = lowerCase(w);
        if ((flags & foldDiacritics) != 0) w = stripDiacritics(w);
        if ((flags & foldWidth) != 0)      w = narrow(w);
        w = normalise(NormalizationC, w);
        return juce::String(w.c_str());
    }

#else

    // No platform fold (neither target builds this): ASCII only, which keeps
    // the store usable and makes the limitation obvious.
    juce::String foldText(const juce::String& s, int flags)
    {
        return (flags & foldCase) != 0 ? s.toLowerCase() : s;
    }

#endif
}

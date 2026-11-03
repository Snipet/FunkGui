#pragma once

// Number formatting and parsing in the C locale, whatever setlocale() the host made (v0.11.0). A dump, a fingerprint or
// a capture setting written under de_DE must still read "0.15", never "0,15".
//
// Until v0.10.0 FunkGui called macOS's xlocale(3) functions directly, with a null locale_t as the C locale. That is
// Apple's: glibc has strtof_l and strtol_l but needs a real locale_t, and has no printf-family *_l at all. These wrap
// both, so the text is byte for byte what the xlocale calls wrote on macOS (they are still what runs there):
//   macOS  the *_l functions with a null locale_t
//   Linux  strtof_l / strtol_l with a C locale_t made once; the printf family between uselocale() calls, which set this
//          thread's locale only
//   Emscripten (v0.12.0)  the plain functions: its libc has no other numeric locale (and no strtol_l)
//   Windows (v0.24.0)     the C runtime's _strtof_l, _strtol_l, _vsnprintf_s_l and _vfprintf_l over a C locale made
//                         once (_create_locale)
// Thread-safe; no allocation after the first call.

#include <cstddef>
#include <cstdio>

namespace funkgui
{
    float strtofC(const char* s, char** end) noexcept;
    long  strtolC(const char* s, char** end, int base) noexcept;
    int   snprintfC(char* buf, std::size_t size, const char* format, ...) noexcept
        __attribute__((format(printf, 3, 4)));
    int   fprintfC(std::FILE* f, const char* format, ...) noexcept __attribute__((format(printf, 2, 3)));
}

#include <funkgui/core/CLocale.h>

// include/funkgui/core/CLocale.h (v0.11.0).

#include <cstdarg>
#include <cstdlib>

#if defined(__EMSCRIPTEN__)
  // Emscripten (v0.12.0): its libc, musl, formats and parses numbers in the C locale whatever setlocale() was given
  // (it has no other LC_NUMERIC), and declares no strtol_l at all. The plain functions are the C-locale ones there.
#elif defined(__APPLE__)
  #include <xlocale.h>                               // after <cstdio>/<cstdlib>: the *_l functions
#elif defined(_WIN32)
  // Windows (v0.24.0): the C runtime's own *_l functions over one C locale (_create_locale), as glibc's newlocale.
  #include <locale.h>
  #include <stdio.h>
  #include <stdlib.h>
#else
  #include <locale.h>
#endif

namespace funkgui
{
#if !defined(__EMSCRIPTEN__)
    namespace
    {
        // macOS: xlocale(3) takes a null locale_t as the C locale. glibc: a C locale made once and never freed (a
        // plug-in can be unloaded while other threads still format); newlocale failing leaves (locale_t) 0, which
        // uselocale() treats as "keep the current one", the best there is then.
#if defined(_WIN32)
        _locale_t cLocale() noexcept
        {
            static const _locale_t c = _create_locale(LC_ALL, "C");
            return c;
        }
#else
        locale_t cLocale() noexcept
        {
#if defined(__APPLE__)
            return nullptr;
#else
            static const locale_t c = newlocale(LC_ALL_MASK, "C", static_cast<locale_t>(nullptr));
            return c;
#endif
        }
#endif
    }
#endif

    float strtofC(const char* s, char** end) noexcept
    {
#if defined(__EMSCRIPTEN__)
        return std::strtof(s, end);
#elif defined(_WIN32)
        return _strtof_l(s, end, cLocale());
#else
        return strtof_l(s, end, cLocale());
#endif
    }

    long strtolC(const char* s, char** end, int base) noexcept
    {
#if defined(__EMSCRIPTEN__)
        return std::strtol(s, end, base);
#elif defined(_WIN32)
        return _strtol_l(s, end, base, cLocale());
#else
        return strtol_l(s, end, base, cLocale());
#endif
    }

    int snprintfC(char* buf, std::size_t size, const char* format, ...) noexcept
    {
        va_list args;
        va_start(args, format);
#if defined(__EMSCRIPTEN__)
        const int n = std::vsnprintf(buf, size, format, args);
#elif defined(__APPLE__)
        const int n = vsnprintf_l(buf, size, cLocale(), format, args);
#elif defined(_WIN32)
        // C99's rules from the C runtime's: _vsnprintf_s_l with _TRUNCATE always terminates and says -1 when the text
        // was cut, and _vscprintf_l gives the length it would have had.
        va_list again;
        va_copy(again, args);
        int n = buf != nullptr && size > 0 ? _vsnprintf_s_l(buf, size, _TRUNCATE, format, cLocale(), args) : -1;
        if (n < 0)
            n = _vscprintf_l(format, cLocale(), again);
        va_end(again);
#else
        const locale_t previous = uselocale(cLocale());
        const int n = std::vsnprintf(buf, size, format, args);
        uselocale(previous);
#endif
        va_end(args);
        return n;
    }

    int fprintfC(std::FILE* f, const char* format, ...) noexcept
    {
        va_list args;
        va_start(args, format);
#if defined(__EMSCRIPTEN__)
        const int n = std::vfprintf(f, format, args);
#elif defined(__APPLE__)
        const int n = vfprintf_l(f, cLocale(), format, args);
#elif defined(_WIN32)
        const int n = _vfprintf_l(f, format, cLocale(), args);
#else
        const locale_t previous = uselocale(cLocale());
        const int n = std::vfprintf(f, format, args);
        uselocale(previous);
#endif
        va_end(args);
        return n;
    }
}

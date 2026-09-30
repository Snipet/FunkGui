#include <funkgui/core/CLocale.h>

// include/funkgui/core/CLocale.h (v0.11.0).

#include <cstdarg>
#include <cstdlib>

#if defined(__APPLE__)
  #include <xlocale.h>                               // after <cstdio>/<cstdlib>: the *_l functions
#else
  #include <locale.h>
#endif

namespace funkgui
{
    namespace
    {
        // macOS: xlocale(3) takes a null locale_t as the C locale. glibc: a C locale made once and never freed (a
        // plug-in can be unloaded while other threads still format); newlocale failing leaves (locale_t) 0, which
        // uselocale() treats as "keep the current one", the best there is then.
        locale_t cLocale() noexcept
        {
#if defined(__APPLE__)
            return nullptr;
#else
            static const locale_t c = newlocale(LC_ALL_MASK, "C", static_cast<locale_t>(nullptr));
            return c;
#endif
        }
    }

    float strtofC(const char* s, char** end) noexcept
    {
        return strtof_l(s, end, cLocale());
    }

    long strtolC(const char* s, char** end, int base) noexcept
    {
        return strtol_l(s, end, base, cLocale());
    }

    int snprintfC(char* buf, std::size_t size, const char* format, ...) noexcept
    {
        va_list args;
        va_start(args, format);
#if defined(__APPLE__)
        const int n = vsnprintf_l(buf, size, cLocale(), format, args);
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
#if defined(__APPLE__)
        const int n = vfprintf_l(f, cLocale(), format, args);
#else
        const locale_t previous = uselocale(cLocale());
        const int n = std::vfprintf(f, format, args);
        uselocale(previous);
#endif
        va_end(args);
        return n;
    }
}

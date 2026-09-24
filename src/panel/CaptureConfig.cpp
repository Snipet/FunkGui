#include <funkgui/panel/CaptureConfig.h>

#include <funkgui/core/Env.h>
#include <funkgui/core/Theme.h>

#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>

#include <xlocale.h>                                     // after <cstdlib>: strtol_l, strtof_l (macOS)

// CaptureConfig::fromEnv (02 §5.1 "Diagnostics environment"; G7). Every value is read once through funkgui::env(), so
// the names are FUNKGUI_ENV_PREFIX + the suffix (FCMP_CANVAS_DUMP in FCompressor, FUNKGUI_CANVAS_DUMP in FunkGui's own
// gallery app). Choices where 02 §5.1 is silent:
// - Numbers are parsed in the C locale, whole string only (surrounding spaces are not accepted); a value that does not
//   parse or is out of range leaves the field at its default, as an unset variable does.
// - CANVAS_DUMP and A11Y_DUMP are paths: an empty value is "no dump". UI_KEYS keeps its value verbatim.
// - CANVAS_DUMP_AFTER and UI_SCALE_AFTER are frame counts >= 0. UI_THEME is a theme index 0..Theme::kCount-1.
// - UI_SCALE is a backing scale in (0, 8]. UI_FIXED_DT is seconds in (0, 1] (EditorHost's live clock clamps its own dt
//   to [1 ms, 100 ms]; a capture may ask for a coarser fixed step).
// - GPU_LOG is on for any value but "" and "0".

namespace funkgui
{
    namespace
    {
        constexpr locale_t kCLocale = nullptr;           // the *_l functions take a null locale_t as the C locale

        bool parseInt(const char* s, int lo, int hi, int& out)
        {
            if (s == nullptr || *s == '\0' || *s == ' ' || *s == '\t')
                return false;
            char* end = nullptr;
            errno = 0;
            const long v = strtol_l(s, &end, 10, kCLocale);
            if (errno != 0 || end == s || *end != '\0' || v < lo || v > hi)
                return false;
            out = static_cast<int>(v);
            return true;
        }

        bool parseFloat(const char* s, float lo, float hi, float& out)
        {
            if (s == nullptr || *s == '\0' || *s == ' ' || *s == '\t')
                return false;
            char* end = nullptr;
            errno = 0;
            const float v = strtof_l(s, &end, kCLocale);
            if (errno != 0 || end == s || *end != '\0' || !std::isfinite(v) || !(v > lo) || v > hi)
                return false;
            out = v;
            return true;
        }

        const char* nonEmpty(const char* s) { return s != nullptr && *s != '\0' ? s : nullptr; }
    }

    CaptureConfig CaptureConfig::fromEnv()
    {
        CaptureConfig c;
        if (const char* v = nonEmpty(env("CANVAS_DUMP")))
            c.canvasDump = v;
        parseInt(env("CANVAS_DUMP_AFTER"), 0, INT_MAX, c.canvasDumpAfter);
        parseInt(env("UI_THEME"), 0, Theme::kCount - 1, c.uiTheme);
        parseFloat(env("UI_SCALE"), 0.0f, 8.0f, c.uiScale);
        parseInt(env("UI_SCALE_AFTER"), 0, INT_MAX, c.uiScaleAfter);
        if (const char* v = nonEmpty(env("UI_KEYS")))
            c.uiKeys = v;
        parseFloat(env("UI_FIXED_DT"), 0.0f, 1.0f, c.fixedDt);
        if (const char* v = nonEmpty(env("A11Y_DUMP")))
            c.a11yDump = v;
        if (const char* v = nonEmpty(env("GPU_LOG")))
            c.gpuLog = !(v[0] == '0' && v[1] == '\0');
        return c;
    }
}

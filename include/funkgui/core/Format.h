#pragma once

// Numeric formatting for readouts (02 §5.8): fixed caller buffers, no heap, no locale, and U+2212 MINUS SIGN for every
// negative number (02 §4.6 minus policy; "-" stays only inside words). The value text never contains its unit: units
// come back separately so the caller sets them in their own style (01 §4.6, K1 #15).
//
// Common rules for every function below (G2's choices where 02 is silent; fg.format pins them):
// - Rounding is to the printed precision, half away from zero, done in double on the float's exact value; the text is
//   built digit by digit, so a host that changes the C locale cannot turn "2.5" into "2,5".
// - A value that rounds to zero prints without a sign ("0.0", never "−0.0").
// - NaN prints U+2013 EN DASH ("–", the n/a glyph, 01 §4.6); +inf prints U+221E ("∞"); −inf prints "−∞".
// - Return value: the length in bytes of the text written to `out` (without the terminating NUL), or -1 when the text
//   does not fit in `n` bytes including the NUL; then `out` holds "" (when n > 0). A number is never truncated.
// - A `unit` out-pointer, when not null, receives a static UTF-8 string ("" for non-finite values).

#include <cstddef>

namespace funkgui::fmt
{
    // Decibels with dp decimals (dp is clamped to 0..6): db(-18.f, 1) -> "−18.0". No unit: callers print "DB".
    int db(float v, int dp, char* out, size_t n) noexcept;

    // A time in seconds, in the unit that keeps it readable: µS below 1 ms, MS below 1 s, else S (01 §3.1 atk "µs below
    // 1 ms, else ms"). Within each unit: 1 decimal below 10, whole numbers from 10 ("5.0" µS, "250" µS, "2.5" MS,
    // "30" MS, "1.2" S, "20" S). The unit is chosen after rounding, so 999.7 µs prints "1.0" MS, never "1000" µS.
    // *unit = "µS" | "MS" | "S".
    int seconds(float s, char* out, size_t n, const char** unit) noexcept;

    // A 0..1 fraction as a whole-number percentage: percent(0.425f) -> "43". No unit: callers print "%".
    int percent(float v01, char* out, size_t n) noexcept;

    // A frequency: whole hertz below 1 kHz ("120", *unit = "HZ"), else kilohertz with 1 decimal ("1.2", "12.5",
    // *unit = "K"). The unit is chosen after rounding, so 999.6 Hz prints "1.0" K.
    int hz(float hz, char* out, size_t n, const char** unit) noexcept;
}

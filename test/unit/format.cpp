// FUNKGUI_TEST name=fg.format timeout=300 gpu=0
//
// fg.format: funkgui::fmt (core/Format.h; 02 §5.8, §4.6 minus policy): exact texts and units for db, seconds, percent
// and hz, U+2212 for every negative number and never an ASCII '-', no sign on a rounded zero, the non-finite texts, the
// buffer contract (-1 and "" when it does not fit, never a truncated number) and independence from the C locale.
// Spec rows only.

#include <funkgui/core/Format.h>
#include <funkgui/test/Harness.h>

#include <clocale>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

namespace T = funkgui::test;
namespace fmt = funkgui::fmt;

namespace
{
    constexpr const char* kMinus = "\xE2\x88\x92";
    constexpr const char* kMicroS = "\xC2\xB5S";

    int asciiHyphens = 0;                                // every text checked must be free of '-' (02 §4.6)

    void check(T::Probe& P, std::string_view key, int rc, const char* got, const std::string& want,
               const char* unit = nullptr, const char* wantUnit = nullptr)
    {
        if (std::strchr(got, '-') != nullptr)
            ++asciiHyphens;
        const bool unitOk = wantUnit == nullptr || (unit != nullptr && std::strcmp(unit, wantUnit) == 0);
        const bool ok = want == got && rc == static_cast<int>(want.size()) && unitOk;
        if (!ok)
            std::printf("INFO     %.*s: got \"%s\" (rc %d, unit \"%s\"), want \"%s\" (unit \"%s\")\n",
                        static_cast<int>(key.size()), key.data(), got, rc, unit != nullptr ? unit : "(null)",
                        want.c_str(), wantUnit != nullptr ? wantUnit : "(any)");
        P.eq(key, ok, 1);
    }

    void db(T::Probe& P, std::string_view key, float v, int dp, const std::string& want)
    {
        char buf[64];
        const int rc = fmt::db(v, dp, buf, sizeof buf);
        check(P, key, rc, buf, want);
    }

    void sec(T::Probe& P, std::string_view key, float s, const std::string& want, const char* wantUnit)
    {
        char buf[64];
        const char* unit = nullptr;
        const int rc = fmt::seconds(s, buf, sizeof buf, &unit);
        check(P, key, rc, buf, want, unit, wantUnit);
    }

    void pct(T::Probe& P, std::string_view key, float v, const std::string& want)
    {
        char buf[64];
        const int rc = fmt::percent(v, buf, sizeof buf);
        check(P, key, rc, buf, want);
    }

    void hz(T::Probe& P, std::string_view key, float f, const std::string& want, const char* wantUnit)
    {
        char buf[64];
        const char* unit = nullptr;
        const int rc = fmt::hz(f, buf, sizeof buf, &unit);
        check(P, key, rc, buf, want, unit, wantUnit);
    }
}

int main(int argc, char** argv)
{
    T::Probe P("fg.format", "", argc, argv);
    const std::string m = kMinus;
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();

    // ---- db ---------------------------------------------------------------------------------------------------------
    db(P, "db.negative", -18.0f, 1, m + "18.0");
    db(P, "db.positive", 6.0f, 1, "6.0");
    db(P, "db.zero", 0.0f, 1, "0.0");
    db(P, "db.minus_zero", -0.0f, 1, "0.0");
    db(P, "db.rounds_to_zero_unsigned", -0.04f, 1, "0.0");
    db(P, "db.rounds_away_from_zero", -0.05f, 1, m + "0.1");
    db(P, "db.half_away_positive", 2.5f, 0, "3");
    db(P, "db.half_away_negative", -2.5f, 0, m + "3");
    db(P, "db.float_below_half", 1.005f, 2, "1.00");
    db(P, "db.two_decimals", 12.345f, 2, "12.35");
    db(P, "db.integer", -42.0f, 0, m + "42");
    db(P, "db.leading_zero", -0.25f, 2, m + "0.25");
    db(P, "db.dp_clamped_low", 3.7f, -2, "4");
    db(P, "db.dp_clamped_high", 1.0f, 9, "1.000000");
    db(P, "db.large", -1234567.0f, 1, m + "1234567.0");
    db(P, "db.nan_is_en_dash", nan, 1, "\xE2\x80\x93");
    db(P, "db.inf", inf, 1, "\xE2\x88\x9E");
    db(P, "db.minus_inf", -inf, 1, m + "\xE2\x88\x9E");
    db(P, "db.absurd_magnitude", 1152921504606846976.0f, 1, "1152921504606846976.0");   // 2^60, exact in a float

    // ---- the buffer contract ----------------------------------------------------------------------------------------
    {
        char buf[16];
        P.eq("buffer.exact_fit", fmt::db(-18.0f, 1, buf, 8), 7);               // 3-byte minus + "18.0" + NUL
        P.eq("buffer.exact_fit_text", std::string(buf) == m + "18.0", 1);
        std::strcpy(buf, "junk");
        P.eq("buffer.one_short", fmt::db(-18.0f, 1, buf, 7), -1);
        P.eq("buffer.one_short_empty", buf[0] == 0, 1);
        P.eq("buffer.zero_size", fmt::db(1.0f, 1, buf, 0), -1);
        P.eq("buffer.null_out", fmt::db(1.0f, 1, nullptr, 16), -1);
        const char* unit = "unchanged";
        P.eq("buffer.seconds_one_short", fmt::seconds(0.25f, buf, 3, &unit), -1);   // "250" needs 4 bytes
        P.eq("buffer.seconds_null_unit_ok", fmt::seconds(0.25f, buf, sizeof buf, nullptr), 3);
        P.eq("buffer.hz_null_unit_ok", fmt::hz(120.0f, buf, sizeof buf, nullptr), 3);
    }

    // ---- seconds: µS < 1 ms <= MS < 1 s <= S; 1 decimal below 10, whole from 10; the unit decided after rounding ----
    sec(P, "sec.us_one_decimal", 0.000005f, "5.0", kMicroS);
    sec(P, "sec.us_whole", 0.00025f, "250", kMicroS);
    sec(P, "sec.us_20", 0.00002f, "20", kMicroS);
    sec(P, "sec.us_9.96_rounds_to_whole", 0.00000996f, "10", kMicroS);
    sec(P, "sec.us_rounding_up_becomes_ms", 0.0009997f, "1.0", "MS");
    sec(P, "sec.ms_one_decimal", 0.0025f, "2.5", "MS");
    sec(P, "sec.ms_whole", 0.03f, "30", "MS");
    sec(P, "sec.ms_250", 0.25f, "250", "MS");
    sec(P, "sec.ms_rounding_up_becomes_s", 0.9999f, "1.0", "S");
    sec(P, "sec.s_one_decimal", 1.2f, "1.2", "S");
    sec(P, "sec.s_whole", 20.0f, "20", "S");
    sec(P, "sec.s_large", 3600.0f, "3600", "S");
    sec(P, "sec.zero_is_0_ms", 0.0f, "0", "MS");
    sec(P, "sec.tiny_is_0_ms", 1.0e-8f, "0", "MS");
    sec(P, "sec.negative", -0.0025f, m + "2.5", "MS");
    sec(P, "sec.nan", nan, "\xE2\x80\x93", "");
    sec(P, "sec.inf", inf, "\xE2\x88\x9E", "");

    // ---- percent ----------------------------------------------------------------------------------------------------
    pct(P, "pct.half", 0.5f, "50");
    pct(P, "pct.rounds_half_up", 0.425f, "43");
    pct(P, "pct.zero", 0.0f, "0");
    pct(P, "pct.one", 1.0f, "100");
    pct(P, "pct.over", 2.0f, "200");
    pct(P, "pct.negative", -0.25f, m + "25");
    pct(P, "pct.rounds_to_zero_unsigned", -0.004f, "0");
    pct(P, "pct.nan", nan, "\xE2\x80\x93");

    // ---- hz: whole HZ below 1 kHz, else K with 1 decimal; the unit decided after rounding ---------------------------
    hz(P, "hz.whole", 120.0f, "120", "HZ");
    hz(P, "hz.rounds", 80.4f, "80", "HZ");
    hz(P, "hz.999", 999.4f, "999", "HZ");
    hz(P, "hz.rounding_up_becomes_k", 999.6f, "1.0", "K");
    hz(P, "hz.k_one_decimal", 1200.0f, "1.2", "K");
    hz(P, "hz.k_12.5", 12500.0f, "12.5", "K");
    hz(P, "hz.k_20", 20000.0f, "20.0", "K");
    hz(P, "hz.zero", 0.0f, "0", "HZ");
    hz(P, "hz.negative", -50.0f, m + "50", "HZ");
    hz(P, "hz.nan", nan, "\xE2\x80\x93", "");

    // ---- no locale: a decimal-comma C locale changes nothing --------------------------------------------------------
    const std::string saved = std::setlocale(LC_NUMERIC, nullptr);
    const char* comma = nullptr;
    for (const char* name : { "de_DE.UTF-8", "fr_FR.UTF-8", "de_DE", "fr_FR" })
        if ((comma = std::setlocale(LC_NUMERIC, name)) != nullptr)
            break;
    if (comma != nullptr)
    {
        char probe[16];
        std::snprintf(probe, sizeof probe, "%.1f", 2.5);
        std::printf("INFO     locale %s formats 2.5 as \"%s\" through printf\n", comma, probe);
        db(P, "locale.db", 2.5f, 1, "2.5");
        hz(P, "locale.hz", 1200.0f, "1.2", "K");
        sec(P, "locale.seconds", 0.0025f, "2.5", "MS");
        std::setlocale(LC_NUMERIC, saved.c_str());
    }
    else
        std::printf("INFO     no decimal-comma locale installed; the locale rows are not run\n");

    P.eq("minus.never_ascii_hyphen", asciiHyphens, 0);
    return P.finish();
}

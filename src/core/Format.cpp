#include <funkgui/core/Format.h>

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace funkgui::fmt
{
    namespace
    {
        constexpr const char* kMinus    = "\xE2\x88\x92";    // U+2212 MINUS SIGN
        constexpr const char* kEnDash   = "\xE2\x80\x93";    // U+2013 EN DASH (n/a)
        constexpr const char* kInfinity = "\xE2\x88\x9E";    // U+221E INFINITY
        constexpr const char* kMicroS   = "\xC2\xB5S";       // "µS" (U+00B5 MICRO SIGN)

        constexpr double kPow10[] = { 1.0, 10.0, 100.0, 1000.0, 10000.0, 100000.0, 1000000.0 };
        constexpr int    kMaxDp = 6;

        // Above this a scaled magnitude is printed through %.0f (llround's range ends at 2^63; readouts never do).
        constexpr double kLargest = 9.0e15;

        // A bounded writer: a text that does not fit leaves "" and reports -1, never a truncated number.
        class Out
        {
        public:
            Out(char* p, size_t n) noexcept : p_(p), n_(p == nullptr ? 0 : n) {}

            void put(char c) noexcept
            {
                if (len_ + 1 < n_)
                    p_[len_++] = c;
                else
                    ok_ = false;
            }

            void put(const char* s) noexcept
            {
                while (*s != 0)
                    put(*s++);
            }

            void digits(uint64_t v, int minDigits = 1) noexcept
            {
                char buf[24];
                int k = 0;
                do
                {
                    buf[k++] = static_cast<char>('0' + static_cast<int>(v % 10u));
                    v /= 10u;
                } while (v != 0u || k < minDigits);
                while (k > 0)
                    put(buf[--k]);
            }

            int finish() noexcept
            {
                if (n_ == 0)
                    return -1;
                if (!ok_)
                {
                    p_[0] = 0;
                    return -1;
                }
                p_[len_] = 0;
                return static_cast<int>(len_);
            }

        private:
            char*  p_;
            size_t n_;
            size_t len_ = 0;
            bool   ok_ = true;
        };

        // NaN / ±inf per Format.h; true when v was not finite and has been written.
        bool putNonFinite(Out& o, double v) noexcept
        {
            if (std::isnan(v))
            {
                o.put(kEnDash);
                return true;
            }
            if (std::isinf(v))
            {
                if (v < 0.0)
                    o.put(kMinus);
                o.put(kInfinity);
                return true;
            }
            return false;
        }

        // |v| * 10^dp rounded half away from zero: an integer count of 10^-dp units (v finite, |v|*10^dp < kLargest).
        uint64_t scaledRound(double v, int dp) noexcept
        {
            return static_cast<uint64_t>(std::llround(std::fabs(v) * kPow10[dp]));
        }

        // Writes q (a count of 10^-dp units) with dp decimals, preceded by the minus when negative and q != 0.
        void putUnits(Out& o, bool negative, uint64_t q, int dp) noexcept
        {
            if (negative && q != 0u)
                o.put(kMinus);
            const auto scale = static_cast<uint64_t>(kPow10[dp]);
            o.digits(q / scale);
            if (dp > 0)
            {
                o.put('.');
                o.digits(q % scale, dp);
            }
        }

        // A finite v with exactly dp decimals.
        void putFixed(Out& o, double v, int dp) noexcept
        {
            const double a = std::fabs(v) * kPow10[dp];
            if (a < kLargest)
            {
                putUnits(o, v < 0.0, scaledRound(v, dp), dp);
                return;
            }
            // Only for absurd magnitudes: the integer digits of |v| (no decimal point, so no locale), then zeros.
            char buf[64];
            std::snprintf(buf, sizeof buf, "%.0f", std::fabs(v));
            if (v < 0.0)
                o.put(kMinus);
            o.put(buf);
            if (dp > 0)
            {
                o.put('.');
                for (int i = 0; i < dp; ++i)
                    o.put('0');
            }
        }

        // The readout rule inside one unit: 1 decimal below 10, whole numbers from 10, decided after rounding. Returns
        // false when the rounded value reaches `limit` (the next unit takes it); limit <= 0 means no limit.
        bool putInUnit(Out& o, double x, double limit) noexcept
        {
            const double a = std::fabs(x);
            if (a * 10.0 < kLargest)
            {
                const uint64_t tenths = scaledRound(x, 1);
                if (tenths < 100u)
                {
                    putUnits(o, x < 0.0, tenths, 1);
                    return true;
                }
                const uint64_t whole = scaledRound(x, 0);
                if (limit > 0.0 && static_cast<double>(whole) >= limit)
                    return false;
                putUnits(o, x < 0.0, whole, 0);
                return true;
            }
            if (limit > 0.0)
                return false;
            putFixed(o, x, 0);
            return true;
        }

        void setUnit(const char** unit, const char* u) noexcept
        {
            if (unit != nullptr)
                *unit = u;
        }
    }

    int db(float v, int dp, char* out, size_t n) noexcept
    {
        Out o(out, n);
        const int d = dp < 0 ? 0 : (dp > kMaxDp ? kMaxDp : dp);
        if (!putNonFinite(o, static_cast<double>(v)))
            putFixed(o, static_cast<double>(v), d);
        return o.finish();
    }

    int seconds(float s, char* out, size_t n, const char** unit) noexcept
    {
        Out o(out, n);
        const double v = static_cast<double>(s);
        if (putNonFinite(o, v))
        {
            setUnit(unit, "");
            return o.finish();
        }
        if (scaledRound(v * 1.0e6, 1) == 0u)
        {
            // Rounds to zero even in µS: a plain "0" MS, as HR's pre-delay printed its neutral default.
            o.put('0');
            setUnit(unit, "MS");
            return o.finish();
        }
        if (putInUnit(o, v * 1.0e6, 1000.0))
            setUnit(unit, kMicroS);
        else if (putInUnit(o, v * 1.0e3, 1000.0))
            setUnit(unit, "MS");
        else
        {
            putInUnit(o, v, 0.0);
            setUnit(unit, "S");
        }
        return o.finish();
    }

    int percent(float v01, char* out, size_t n) noexcept
    {
        Out o(out, n);
        const double v = static_cast<double>(v01) * 100.0;
        if (!putNonFinite(o, v))
            putFixed(o, v, 0);
        return o.finish();
    }

    int hz(float f, char* out, size_t n, const char** unit) noexcept
    {
        Out o(out, n);
        const double v = static_cast<double>(f);
        if (putNonFinite(o, v))
        {
            setUnit(unit, "");
            return o.finish();
        }
        const double a = std::fabs(v);
        if (a < kLargest && scaledRound(v, 0) < 1000u)
        {
            putUnits(o, v < 0.0, scaledRound(v, 0), 0);
            setUnit(unit, "HZ");
        }
        else
        {
            putFixed(o, v / 1000.0, 1);
            setUnit(unit, "K");
        }
        return o.finish();
    }
}

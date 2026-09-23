#include <funkgui/a11y/A11yItem.h>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace funkgui
{
    namespace
    {
        constexpr size_t kTitleColumns  = 24;
        constexpr size_t kBoundsColumns = 20;
        constexpr int    kMaxDecimals   = 9;

        // Codepoints, counted by their lead bytes (the columns a monospaced dump shows).
        size_t columns(const std::string& s)
        {
            size_t n = 0;
            for (const char c : s)
                if ((static_cast<unsigned char>(c) & 0xC0u) != 0x80u)
                    ++n;
            return n;
        }

        // s with CR, LF and tab written as spaces, so an item is exactly one line.
        void appendOneLine(std::string& out, const std::string& s)
        {
            for (const char c : s)
                out += (c == '\r' || c == '\n' || c == '\t') ? ' ' : c;
        }

        void appendPadded(std::string& out, const std::string& s, size_t width)
        {
            appendOneLine(out, s);
            for (size_t n = columns(s); n < width; ++n)
                out += ' ';
        }

        void appendDigits(std::string& out, uint64_t v, int minDigits)
        {
            char buf[24];
            int k = 0;
            do
            {
                buf[k++] = static_cast<char>('0' + static_cast<int>(v % 10u));
                v /= 10u;
            } while (v != 0u || k < minDigits);
            while (k > 0)
                out += buf[--k];
        }

        // A float as the plain decimal with the fewest decimals (at most kMaxDecimals) that converts back to it; no
        // exponent, no locale. Magnitudes too large for that, and values no such decimal reproduces, fall back to
        // "%.9g", which is exact for every float.
        std::string number(float x)
        {
            if (std::isnan(x))
                return "nan";
            if (std::isinf(x))
                return x < 0.0f ? "-inf" : "inf";

            const double a = std::fabs(static_cast<double>(x));
            double scale = 1.0;
            for (int d = 0; d <= kMaxDecimals; ++d, scale *= 10.0)
            {
                const double scaled = a * scale;
                if (scaled >= 9.0e15)
                    break;
                const auto q = static_cast<uint64_t>(std::llround(scaled));
                const float back = static_cast<float>(static_cast<double>(q) / scale);
                if (std::bit_cast<uint32_t>(back) != std::bit_cast<uint32_t>(std::fabs(x)))
                    continue;
                std::string s;
                if (x < 0.0f && q != 0u)
                    s += '-';
                const auto unit = static_cast<uint64_t>(scale);
                appendDigits(s, q / unit, 1);
                if (d > 0)
                {
                    s += '.';
                    appendDigits(s, q % unit, d);
                }
                return s;
            }
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.9g", static_cast<double>(x));
            return buf;
        }
    }

    std::string a11yDumpLine(const A11yItem& item)
    {
        std::string line;
        const int role = static_cast<int>(item.role);
        if (role < 10)
            line += ' ';
        appendDigits(line, static_cast<uint64_t>(role), 1);
        line += ' ';
        appendPadded(line, item.title, kTitleColumns);
        line += ' ';

        const std::string bounds = number(item.bounds.x) + "," + number(item.bounds.y) + ","
                                 + number(item.bounds.w) + "," + number(item.bounds.h);
        appendPadded(line, bounds, kBoundsColumns);

        std::string tail;
        const auto part = [&tail](const char* text)
        {
            tail += ' ';
            tail += text;
        };
        if (item.checkable)
            part(item.checked ? "checked" : "unchecked");
        else if (!item.value.empty())
        {
            part("value=");
            appendOneLine(tail, item.value);
        }
        if (item.readOnly)
            part("[ro]");
        if (!item.enabled)
            part("[off]");
        if (!item.help.empty())
        {
            part("help=");
            appendOneLine(tail, item.help);
        }

        if (tail.empty())
        {
            while (!line.empty() && line.back() == ' ')
                line.pop_back();                         // the padding never trails
            return line;
        }
        // Every tail part starts with its separating space, so exactly one space follows the padded bounds column.
        return line + tail;
    }
}

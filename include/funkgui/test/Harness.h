#pragma once

// FunkGui Harness v2: FunkGui::harness, namespace funkgui::test.
// Specification: FCompressor docs/design/03-build-verify-process.md §3.2.1–§3.2.4 (API, command line, golden
// format v2, exit codes). Header-only, C++20, JUCE-free. It replaces HardwareReverb's Tools/Harness.h (v1: addNum /
// addHash / --check / --bless), which is the seed baseline of this file (SEED.tsv, README.md -> Provenance).
//
// One probe subcommand per process:
//
//   <exe> <layer>.<name> [--mode <key>] --golden-root <dir> --arch arm64|x86_64 [--bless-to <dir>]
//         [--results <dir>] [--only <glob>] [--quick] [--verbose]
//
//   int main(int argc, char** argv)          // (FCompressor dispatches through ProbeMain.cpp / FCMP_PROBE)
//   {
//       const funkgui::test::ScopedFtz ftz;
//       funkgui::test::Probe P("dsp.static", "fet-76", argc, argv);
//       P.near("static.r4.t-30.out_db", got, want, 0.05);                  // spec row: judged now, never stored
//       P.num ("static.r4.t-30.k6.x-20.out_db", got, funkgui::test::Tol::abs(0.01));   // golden row
//       return P.finish();                                                  // exit code 0..4 (03 §3.2.4)
//   }
//
// Rules this header enforces (03 §3.2):
// - Spec rows (near/le/ge/in/eq) are evaluated at once and printed; any failure makes the run spec_fail (exit 1),
//   whatever the golden says. They are never stored, so blessing cannot launder them.
// - Golden rows (num/hash/text/lines) are compared with <root>/base/<scope>/<probe>.txt merged with the overlay
//   <root>/<arch>/<scope>/<probe>.txt; `lines` rows with the sidecar <root>/base/<scope>/<probe>.<key>.lines.
//   <scope> is "global" or "modes/<mode>". <probe> is the name given to the constructor ("<layer>.<name>").
// - There is no --bless. --bless-to <dir> writes the full measured row set as <dir>/<arch>/<scope>/<probe>.txt
//   (plus <probe>.<key>.lines sidecars and, on drift, <probe>.diff), each through a temp file and rename().
//   Only the lead's golden.py adopt moves candidates into the golden tree.
// - Harness errors (exit 4, blocking): invalid or duplicate key, malformed golden, overlay without base, overlay-only
//   key, xarch.* key in an overlay, unknown flag, bad or missing --golden-root/--arch, harnessError() from the probe.
//
// Choices where 03 §3.2 is silent (recorded in the v0.0.1 handoff; G1's fg.harness.self pins them):
// - Positional arguments (the subcommand itself, a tool's own inputs) are ignored; every "--" token must be one of
//   the flags above. --mode, when present, must equal the constructor's mode. --only may repeat (union of globs;
//   `*` and `?`).
// - Spec and golden keys are separate namespaces (one quantity may carry both a spec row and a golden row); a key
//   repeated within either namespace is a harness error. Rows whose key --only does not select are dropped.
// - A golden row is compared with the golden's own tolerance; a run whose tolerance text differs from the golden's
//   counts that row as drift, so a tolerance change is re-blessed like a value change.
// - golden_missing (exit 3) needs at least one golden row: a spec-only probe with no golden file passes.
// - Candidates are not written under --only or --quick (the row set is partial), nor on a harness error; under
//   --quick, golden rows the run did not produce are not counted as missing.
// - Values are non-empty, at most 64 bytes, without whitespace or control characters. `lines` entries may hold any
//   text except CR/LF. `lines` sidecars are base-only (03 §3.2.2 names no overlay for them); a sidecar's key is the
//   file name between "<probe>." and ".lines", so a probe name must not be a dot-prefix of another probe's name in
//   the same scope if either writes `lines` (FCompressor's <layer>.<name> names never are).
// - Results JSON: <results>/<probe>.json (global) or <probe>.<mode>.json, written atomically; the same object is
//   printed as the final "RESULT {...}" line.
// - harnessError(why) is the one addition to the 03 §3.2.1 API: it lets a probe report a harness-level failure
//   (e.g. an unsettled UI ease, 03 §3.2.4) as exit 4.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>

#if defined(__x86_64__) || defined(_M_X64)
  #include <xmmintrin.h>
#endif

namespace funkgui::test
{
    // Flush-to-zero, matching what juce::ScopedNoDenormals does on each
    // architecture (juce_FloatVectorOperations.cpp, JUCE 8.0.4):
    //
    //   arm64   FPCR |= 1 << 24            (FZ: flush denormal results)
    //   x86-64  MXCSR |= 0x8040            (FTZ 0x8000: flush denormal
    //                                       results; DAZ 0x0040: treat
    //                                       denormal inputs as zero)
    //
    // via mrs/msr fpcr and _mm_getcsr/_mm_setcsr respectively. Written out
    // rather than pulled in so the DSP harnesses keep their small link line
    // — they deliberately do not depend on the audio modules. Both bits on
    // x86 matter: with -mavx2 -mfma the whole engine runs in SSE/AVX
    // registers, and MXCSR is the only denormal control those have.
    // (HardwareReverb Tools/Harness.h, unchanged.)
    struct ScopedFtz
    {
#if defined(__aarch64__)
        ScopedFtz() noexcept
        {
            __asm__ volatile("mrs %0, fpcr" : "=r"(saved_));
            const uint64_t v = saved_ | (1ull << 24);
            __asm__ volatile("msr fpcr, %0" : : "r"(v));
        }
        ~ScopedFtz() noexcept { __asm__ volatile("msr fpcr, %0" : : "r"(saved_)); }
        uint64_t saved_ = 0;
#elif defined(__x86_64__) || defined(_M_X64)
        ScopedFtz() noexcept
        {
            saved_ = _mm_getcsr();
            _mm_setcsr(saved_ | 0x8040u);
        }
        ~ScopedFtz() noexcept { _mm_setcsr(saved_); }
        uint32_t saved_ = 0;
#else
  #error "ScopedFtz: no flush-to-zero shim for this architecture"
#endif
    };

    // Golden-row tolerance, stored in the golden file's third column.
    // Grammar: exact | abs:<a> | rel:<r> | absrel:<a>:<r> | le:<slack> | ge:<slack> | <a> (a bare number reads as
    // abs:, so HR's v1 files parse). Numbers are finite and >= 0; text() writes them with "%.9g".
    struct Tol
    {
        enum class Kind : uint8_t { exact, abs, rel, absrel, le, ge };
        Kind kind = Kind::exact; double a = 0, b = 0;

        static constexpr Tol exact() noexcept                    { return {}; }
        // |got - golden| <= a
        static constexpr Tol abs(double x) noexcept              { return { Kind::abs, x, 0 }; }
        // |got - golden| <= r * |golden|
        static constexpr Tol rel(double r) noexcept              { return { Kind::rel, r, 0 }; }
        // |got - golden| <= max(a, r * |golden|)
        static constexpr Tol absrel(double x, double r) noexcept { return { Kind::absrel, x, r }; }
        // got <= golden + slack  (a regression guard; improvements pass)
        static constexpr Tol le(double slack) noexcept           { return { Kind::le, slack, 0 }; }
        // got >= golden - slack
        static constexpr Tol ge(double slack) noexcept           { return { Kind::ge, slack, 0 }; }
        std::string text() const;
        static std::optional<Tol> parse(std::string_view);
    };

    // FNV-1a 64 (HR's hash: offset 1469598103934665603, prime 1099511628211).
    inline uint64_t fnv1a(const void* p, std::size_t n, uint64_t h = 1469598103934665603ull) noexcept
    {
        const auto* bytes = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i)
        {
            h ^= bytes[i];
            h *= 1099511628211ull;
        }
        return h;
    }

    // FNV-1a over the little-endian bytes of each float's bit pattern (HR IrAnalyse.cpp's fnv1a), whatever the
    // host byte order.
    inline uint64_t hashFloats(std::span<const float> v) noexcept
    {
        uint64_t h = 1469598103934665603ull;
        for (const float x : v)
        {
            uint32_t bits = 0;
            std::memcpy(&bits, &x, sizeof bits);
            for (int k = 0; k < 4; ++k)
            {
                h ^= (bits >> (k * 8)) & 0xffu;
                h *= 1099511628211ull;
            }
        }
        return h;
    }

    class Probe
    {
    public:
        Probe(std::string_view probe, std::string_view mode /* "" = global */, int argc, char** argv);
        ~Probe();
        Probe(const Probe&) = delete;
        Probe& operator=(const Probe&) = delete;

        // golden rows: drift detection, blessable
        void num  (std::string_view key, double v, Tol t);                   // written with "%.9g"
        void hash (std::string_view key, uint64_t h);                        // "%016llx", exact
        void text (std::string_view key, std::string_view v);                // exact; no whitespace
        void lines(std::string_view key, std::span<const std::string> v);    // multi-line text (a11y model, tab
                                                                             // order): sidecar <probe>.<key>.lines,
                                                                             // line-by-line diff
        // spec rows: evaluated now, printed, never stored; any failure -> exit 1, whatever the golden says
        bool near(std::string_view key, double got, double want, double absTol, double relTol = 0);
        bool le  (std::string_view key, double got, double bound);
        bool ge  (std::string_view key, double got, double bound);
        bool in  (std::string_view key, double got, double lo, double hi);
        bool eq  (std::string_view key, int64_t got, int64_t want);          // counts, flags, bit patterns
        bool quick() const noexcept;                 // --quick: reduced grids, inner loop only (verify.sh forbids it)
        bool wants(std::string_view key) const;      // --only <glob>
        int  finish();                               // compare, write candidates + results, print RESULT, exit code

        void harnessError(std::string_view why);     // addition to 03 §3.2.1: fail the run with exit 4

    private:
        struct Row      { std::string key, value; Tol tol; double num = 0; bool numeric = false; };
        struct LinesRow { std::string key; std::vector<std::string> v; };
        struct Golden   { std::string value; Tol tol; std::string tolText; double num = 0; };

        bool claimGolden(std::string_view key);
        bool spec(std::string_view key, bool ok, const std::string& what);
        bool loadGolden(const std::filesystem::path& p, std::vector<std::pair<std::string, Golden>>& out);

        std::string probe_, mode_, scope_, arch_;
        std::filesystem::path root_, blessTo_, results_;
        std::vector<std::string> only_;
        bool quick_ = false, verbose_ = false;

        std::vector<Row> rows_;
        std::vector<LinesRow> lines_;
        std::set<std::string, std::less<>> goldenKeys_, specKeys_;
        std::vector<std::string> errors_;
        int specPass_ = 0, specFail_ = 0;

        std::chrono::steady_clock::time_point t0_;
        bool finished_ = false;
        int exit_ = 4;
    };

    // ------------------------------------------------------------------------------------------------------------
    // Implementation
    // ------------------------------------------------------------------------------------------------------------

    namespace detail
    {
        inline std::string fmtNum(double v)
        {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%.9g", v);
            return buf;
        }

        inline std::string fmtHash(uint64_t h)
        {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
            return buf;
        }

        // The whole string must be a number (strtod grammar, no leading space).
        inline bool parseNum(std::string_view s, double& out)
        {
            if (s.empty() || s.size() > 64 || s.front() == ' ' || s.front() == '\t')
                return false;
            const std::string z(s);
            char* end = nullptr;
            out = std::strtod(z.c_str(), &end);
            return end == z.c_str() + z.size();
        }

        inline bool parseTolNum(std::string_view s, double& out)
        {
            if (!parseNum(s, out) || !std::isfinite(out) || out < 0.0)
                return false;
            out += 0.0;                                                // -0 -> +0
            return true;
        }

        // ^[a-z0-9][a-z0-9._:+-]{0,119}$
        inline bool validKey(std::string_view k)
        {
            if (k.empty() || k.size() > 120)
                return false;
            for (std::size_t i = 0; i < k.size(); ++i)
            {
                const char c = k[i];
                const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
                if (i == 0 ? !alnum : !(alnum || c == '.' || c == '_' || c == ':' || c == '+' || c == '-'))
                    return false;
            }
            return true;
        }

        // Modes.def keys: [a-z0-9-]+ (at most 64 here).
        inline bool validMode(std::string_view m)
        {
            if (m.empty() || m.size() > 64)
                return false;
            return std::all_of(m.begin(), m.end(),
                               [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
        }

        // nullptr when the value is legal: non-empty, <= 64 bytes, no whitespace or control characters.
        inline const char* valueProblem(std::string_view v)
        {
            if (v.empty())
                return "empty value";
            if (v.size() > 64)
                return "value longer than 64 bytes";
            for (const char c : v)
            {
                const auto u = static_cast<unsigned char>(c);
                if (u <= 0x20 || u == 0x7f)
                    return "value contains whitespace or a control character";
            }
            return nullptr;
        }

        // Glob with `*` (any run, dots included) and `?` (one character).
        inline bool globMatch(std::string_view pat, std::string_view s)
        {
            std::size_t pi = 0, si = 0, star = std::string_view::npos, mark = 0;
            while (si < s.size())
            {
                if (pi < pat.size() && (pat[pi] == '?' || pat[pi] == s[si])) { ++pi; ++si; }
                else if (pi < pat.size() && pat[pi] == '*')                  { star = pi++; mark = si; }
                else if (star != std::string_view::npos)                       { pi = star + 1; si = ++mark; }
                else                                                           return false;
            }
            while (pi < pat.size() && pat[pi] == '*')
                ++pi;
            return pi == pat.size();
        }

        inline std::string jsonString(std::string_view s)
        {
            std::string o = "\"";
            for (const char c : s)
            {
                const auto u = static_cast<unsigned char>(c);
                if (c == '"' || c == '\\') { o += '\\'; o += c; }
                else if (u < 0x20)
                {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(u));
                    o += buf;
                }
                else o += c;
            }
            return o + "\"";
        }

        inline bool tolAccepts(const Tol& t, double got, double golden) noexcept
        {
            const double d = std::fabs(got - golden);
            switch (t.kind)
            {
                case Tol::Kind::exact:  return got == golden;
                case Tol::Kind::abs:    return d <= t.a;
                case Tol::Kind::rel:    return d <= t.a * std::fabs(golden);
                case Tol::Kind::absrel: return d <= std::max(t.a, t.b * std::fabs(golden));
                case Tol::Kind::le:     return got <= golden + t.a;
                case Tol::Kind::ge:     return got >= golden - t.a;
            }
            return false;
        }

        inline bool validTol(const Tol& t)
        {
            const auto ok = [](double x) { return std::isfinite(x) && x >= 0.0; };
            switch (t.kind)
            {
                case Tol::Kind::exact:  return true;
                case Tol::Kind::absrel: return ok(t.a) && ok(t.b);
                case Tol::Kind::abs:
                case Tol::Kind::rel:
                case Tol::Kind::le:
                case Tol::Kind::ge:     return ok(t.a);
            }
            return false;
        }

        inline bool readFile(const std::filesystem::path& p, std::string& out)
        {
            std::ifstream f(p, std::ios::binary);
            if (!f)
                return false;
            out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            return !f.bad();
        }

        // Write <file>.tmp.<pid>, then rename() it over the target (atomic within one filesystem).
        inline bool writeAtomic(const std::filesystem::path& p, std::string_view content, std::string& err)
        {
            std::error_code ec;
            std::filesystem::create_directories(p.parent_path(), ec);
            if (ec)
            {
                err = "cannot create " + p.parent_path().string() + ": " + ec.message();
                return false;
            }
            std::filesystem::path tmp = p;
            tmp += ".tmp." + std::to_string(static_cast<long long>(::getpid()));
            {
                std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                if (f)
                {
                    f.write(content.data(), static_cast<std::streamsize>(content.size()));
                    f.flush();
                }
                if (!f)
                {
                    err = "cannot write " + tmp.string();
                    f.close();
                    std::filesystem::remove(tmp, ec);
                    return false;
                }
            }
            std::filesystem::rename(tmp, p, ec);
            if (ec)
            {
                err = "cannot rename " + tmp.string() + " -> " + p.string() + ": " + ec.message();
                std::error_code ignored;
                std::filesystem::remove(tmp, ignored);
                return false;
            }
            return true;
        }

        // A sidecar's lines; a final '\n' does not start another line.
        inline std::vector<std::string> splitLines(const std::string& s)
        {
            std::vector<std::string> out;
            std::size_t start = 0;
            while (start < s.size())
            {
                const std::size_t nl = s.find('\n', start);
                if (nl == std::string::npos) { out.push_back(s.substr(start)); break; }
                out.push_back(s.substr(start, nl - start));
                start = nl + 1;
            }
            return out;
        }

        inline std::string joinLines(const std::vector<std::string>& v)
        {
            std::string s;
            for (const auto& l : v) { s += l; s += '\n'; }
            return s;
        }

        // Line diff: common prefix/suffix trimmed, LCS on the rest (positional when the table would be too big).
        // Output entries are "-<golden line no>\t<text>" and "+<run line no>\t<text>".
        inline std::vector<std::string> diffLines(const std::vector<std::string>& g, const std::vector<std::string>& r)
        {
            std::size_t pre = 0;
            while (pre < g.size() && pre < r.size() && g[pre] == r[pre])
                ++pre;
            std::size_t suf = 0;
            while (suf < g.size() - pre && suf < r.size() - pre && g[g.size() - 1 - suf] == r[r.size() - 1 - suf])
                ++suf;
            const std::size_t n = g.size() - pre - suf, m = r.size() - pre - suf;
            std::vector<std::string> out;
            const auto minus = [&](std::size_t i) { out.push_back("-" + std::to_string(i + 1) + "\t" + g[i]); };
            const auto plus  = [&](std::size_t j) { out.push_back("+" + std::to_string(j + 1) + "\t" + r[j]); };
            if (n * m > 4000000)
            {
                for (std::size_t i = 0; i < n; ++i) minus(pre + i);
                for (std::size_t j = 0; j < m; ++j) plus(pre + j);
                return out;
            }
            std::vector<uint32_t> L((n + 1) * (m + 1), 0);         // L(i,j) = LCS of g[pre+i..), r[pre+j..)
            const auto at = [&](std::size_t i, std::size_t j) -> uint32_t& { return L[i * (m + 1) + j]; };
            for (std::size_t i = n; i-- > 0;)
                for (std::size_t j = m; j-- > 0;)
                    at(i, j) = g[pre + i] == r[pre + j] ? at(i + 1, j + 1) + 1 : std::max(at(i + 1, j), at(i, j + 1));
            std::size_t i = 0, j = 0;
            while (i < n || j < m)
            {
                if (i < n && j < m && g[pre + i] == r[pre + j]) { ++i; ++j; }
                else if (i < n && (j == m || at(i + 1, j) >= at(i, j + 1))) minus(pre + i++);
                else plus(pre + j++);
            }
            return out;
        }
    } // namespace detail

    inline std::string Tol::text() const
    {
        switch (kind)
        {
            case Kind::exact:  return "exact";
            case Kind::abs:    return "abs:" + detail::fmtNum(a + 0.0);
            case Kind::rel:    return "rel:" + detail::fmtNum(a + 0.0);
            case Kind::absrel: return "absrel:" + detail::fmtNum(a + 0.0) + ":" + detail::fmtNum(b + 0.0);
            case Kind::le:     return "le:" + detail::fmtNum(a + 0.0);
            case Kind::ge:     return "ge:" + detail::fmtNum(a + 0.0);
        }
        return "exact";
    }

    inline std::optional<Tol> Tol::parse(std::string_view s)
    {
        if (s == "exact")
            return Tol::exact();
        const auto one = [&](std::size_t skip, Tol (*make)(double)) -> std::optional<Tol>
        {
            double x = 0;
            if (detail::parseTolNum(s.substr(skip), x))
                return make(x);
            return std::nullopt;
        };
        if (s.starts_with("abs:")) return one(4, &Tol::abs);
        if (s.starts_with("rel:")) return one(4, &Tol::rel);
        if (s.starts_with("le:"))  return one(3, &Tol::le);
        if (s.starts_with("ge:"))  return one(3, &Tol::ge);
        if (s.starts_with("absrel:"))
        {
            const std::string_view rest = s.substr(7);
            const std::size_t colon = rest.find(':');
            double x = 0, r = 0;
            if (colon != std::string_view::npos && detail::parseTolNum(rest.substr(0, colon), x)
                && detail::parseTolNum(rest.substr(colon + 1), r))
                return Tol::absrel(x, r);
            return std::nullopt;
        }
        double x = 0;                                                  // bare number = abs (HR compatibility)
        if (detail::parseTolNum(s, x))
            return Tol::abs(x);
        return std::nullopt;
    }

    inline Probe::Probe(std::string_view probe, std::string_view mode, int argc, char** argv)
        : probe_(probe), mode_(mode), t0_(std::chrono::steady_clock::now())
    {
        if (!detail::validKey(probe_))
            harnessError("invalid probe name '" + probe_ + "' (it names the golden file; key grammar)");
        if (!mode_.empty() && !detail::validMode(mode_))
            harnessError("invalid mode key '" + mode_ + "'");
        scope_ = mode_.empty() ? std::string("global") : "modes/" + mode_;

        std::string argMode, root;
        bool haveMode = false, haveRoot = false, haveArch = false;
        for (int i = 1; i < argc; ++i)
        {
            const std::string_view a = argv[i] != nullptr ? argv[i] : "";
            if (!a.starts_with("--"))
                continue;                                              // positional: not the harness's
            const auto value = [&](std::string& out) -> bool
            {
                if (i + 1 >= argc || argv[i + 1] == nullptr || argv[i + 1][0] == '\0'
                    || std::string_view(argv[i + 1]).starts_with("--"))
                {
                    harnessError("flag " + std::string(a) + " needs a value");
                    return false;
                }
                out = argv[++i];
                return true;
            };
            std::string v;
            if      (a == "--mode")        { if (value(argMode)) haveMode = true; }
            else if (a == "--golden-root") { if (value(root)) haveRoot = true; }
            else if (a == "--arch")        { if (value(arch_)) haveArch = true; }
            else if (a == "--bless-to")    { if (value(v)) blessTo_ = v; }
            else if (a == "--results")     { if (value(v)) results_ = v; }
            else if (a == "--only")        { if (value(v)) only_.push_back(v); }
            else if (a == "--quick")       quick_ = true;
            else if (a == "--verbose")     verbose_ = true;
            else                           harnessError("unknown flag " + std::string(a));
        }
        if (haveMode && argMode != mode_)
            harnessError("--mode " + argMode + " disagrees with the probe's mode '" + mode_ + "'");
        if (!haveRoot)
            harnessError("--golden-root <dir> is required");
        else
            root_ = root;
        if (!haveArch)
            harnessError("--arch arm64|x86_64 is required");
        else if (arch_ != "arm64" && arch_ != "x86_64")
            harnessError("--arch must be arm64 or x86_64, not '" + arch_ + "'");
    }

    inline Probe::~Probe()
    {
        if (!finished_)
            std::fprintf(stderr, "HARNESS ERROR  probe %s ended without finish()\n", probe_.c_str());
    }

    inline void Probe::harnessError(std::string_view why)
    {
        errors_.emplace_back(why);
        std::printf("HARNESS ERROR  %.*s\n", static_cast<int>(why.size()), why.data());
    }

    inline bool Probe::quick() const noexcept { return quick_; }

    inline bool Probe::wants(std::string_view key) const
    {
        if (only_.empty())
            return true;
        return std::any_of(only_.begin(), only_.end(),
                           [&](const std::string& glob) { return detail::globMatch(glob, key); });
    }

    inline bool Probe::claimGolden(std::string_view key)
    {
        if (!detail::validKey(key))
        {
            harnessError("invalid golden key '" + std::string(key) + "'");
            return false;
        }
        if (!goldenKeys_.insert(std::string(key)).second)
        {
            harnessError("duplicate golden key '" + std::string(key) + "' in this run");
            return false;
        }
        return wants(key);
    }

    inline void Probe::num(std::string_view key, double v, Tol t)
    {
        if (!claimGolden(key))
            return;
        if (!detail::validTol(t))
        {
            harnessError("invalid tolerance for '" + std::string(key) + "'");
            return;
        }
        rows_.push_back({ std::string(key), detail::fmtNum(v), t, v, true });
    }

    inline void Probe::hash(std::string_view key, uint64_t h)
    {
        if (claimGolden(key))
            rows_.push_back({ std::string(key), detail::fmtHash(h), Tol::exact(), 0, false });
    }

    inline void Probe::text(std::string_view key, std::string_view v)
    {
        if (!claimGolden(key))
            return;
        if (const char* why = detail::valueProblem(v))
        {
            harnessError("text '" + std::string(key) + "': " + why);
            return;
        }
        rows_.push_back({ std::string(key), std::string(v), Tol::exact(), 0, false });
    }

    inline void Probe::lines(std::string_view key, std::span<const std::string> v)
    {
        if (!claimGolden(key))
            return;
        for (const auto& l : v)
            if (l.find_first_of("\r\n") != std::string::npos)
            {
                harnessError("lines '" + std::string(key) + "': an entry contains CR or LF");
                return;
            }
        lines_.push_back({ std::string(key), std::vector<std::string>(v.begin(), v.end()) });
    }

    inline bool Probe::spec(std::string_view key, bool ok, const std::string& what)
    {
        if (!detail::validKey(key))
        {
            harnessError("invalid spec key '" + std::string(key) + "'");
            return false;
        }
        if (!specKeys_.insert(std::string(key)).second)
        {
            harnessError("duplicate spec key '" + std::string(key) + "' in this run");
            return false;
        }
        if (!wants(key))
            return true;
        ok ? ++specPass_ : ++specFail_;
        std::printf("SPEC %s  %.*s  %s\n", ok ? "PASS" : "FAIL", static_cast<int>(key.size()), key.data(),
                    what.c_str());
        return ok;
    }

    inline bool Probe::near(std::string_view key, double got, double want, double absTol, double relTol)
    {
        if (!(std::isfinite(absTol) && absTol >= 0.0 && std::isfinite(relTol) && relTol >= 0.0))
        {
            harnessError("near '" + std::string(key) + "': tolerances must be finite and >= 0");
            return false;
        }
        const double tol = std::max(absTol, relTol * std::fabs(want));
        return spec(key, std::fabs(got - want) <= tol,
                    "got " + detail::fmtNum(got) + "  want " + detail::fmtNum(want) + " +/- " + detail::fmtNum(tol));
    }

    inline bool Probe::le(std::string_view key, double got, double bound)
    {
        return spec(key, got <= bound, "got " + detail::fmtNum(got) + "  <= " + detail::fmtNum(bound));
    }

    inline bool Probe::ge(std::string_view key, double got, double bound)
    {
        return spec(key, got >= bound, "got " + detail::fmtNum(got) + "  >= " + detail::fmtNum(bound));
    }

    inline bool Probe::in(std::string_view key, double got, double lo, double hi)
    {
        return spec(key, lo <= got && got <= hi,
                    "got " + detail::fmtNum(got) + "  in [" + detail::fmtNum(lo) + ", " + detail::fmtNum(hi) + "]");
    }

    inline bool Probe::eq(std::string_view key, int64_t got, int64_t want)
    {
        return spec(key, got == want, "got " + std::to_string(got) + "  want " + std::to_string(want));
    }

    inline bool Probe::loadGolden(const std::filesystem::path& p, std::vector<std::pair<std::string, Golden>>& out)
    {
        std::ifstream f(p, std::ios::binary);
        if (!f)
        {
            harnessError("cannot read " + p.string());
            return false;
        }
        const std::size_t before = errors_.size();
        std::set<std::string, std::less<>> seen;
        std::string line;
        std::size_t lineNo = 0;
        while (std::getline(f, line))
        {
            ++lineNo;
            if (line.empty() || line[0] == '#')
                continue;
            const std::string where = p.string() + ":" + std::to_string(lineNo) + ": ";
            const std::size_t t1 = line.find('\t');
            const std::size_t t2 = t1 == std::string::npos ? std::string::npos : line.find('\t', t1 + 1);
            if (t2 == std::string::npos || line.find('\t', t2 + 1) != std::string::npos)
            {
                harnessError(where + "malformed golden row (need key<TAB>value<TAB>tolerance)");
                continue;
            }
            std::string key = line.substr(0, t1);
            Golden g;
            g.value = line.substr(t1 + 1, t2 - t1 - 1);
            const std::string tolField = line.substr(t2 + 1);
            const auto tol = Tol::parse(tolField);
            const char* why = !detail::validKey(key)              ? "invalid key"
                            : detail::valueProblem(g.value) != nullptr ? detail::valueProblem(g.value)
                            : !tol                                   ? "bad tolerance"
                                                                     : nullptr;
            if (why != nullptr)
            {
                harnessError(where + why + " (row '" + line + "')");
                continue;
            }
            g.tol = *tol;
            g.tolText = tol->text();
            if (g.tol.kind != Tol::Kind::exact && !detail::parseNum(g.value, g.num))
            {
                harnessError(where + "numeric tolerance on the non-numeric value '" + g.value + "'");
                continue;
            }
            if (!seen.insert(key).second)
            {
                harnessError(where + "duplicate key '" + key + "'");
                continue;
            }
            out.emplace_back(std::move(key), std::move(g));
        }
        if (f.bad())
            harnessError("error reading " + p.string());
        return errors_.size() == before;
    }

    inline int Probe::finish()
    {
        namespace fs = std::filesystem;
        if (finished_)
            return exit_;
        finished_ = true;

        int goldenRows = 0, goldenFail = 0, goldenNew = 0, goldenMissingRows = 0;
        bool haveGolden = false;
        std::vector<std::string> diff;                                 // body of <probe>.diff
        const std::string prefix = probe_ + ".", suffix = ".lines";
        const bool locatable = !root_.empty() && (arch_ == "arm64" || arch_ == "x86_64") && detail::validKey(probe_);
        std::error_code ec;

        if (locatable)
        {
            const fs::path baseDir = root_ / "base" / scope_;
            const fs::path basePath = baseDir / (probe_ + ".txt");
            const fs::path overPath = root_ / arch_ / scope_ / (probe_ + ".txt");
            const bool baseExists = fs::exists(basePath, ec);
            const bool overExists = fs::exists(overPath, ec);

            // 1. base, then the arch overlay on top (overlay rows must exist in base; never xarch.*)
            std::vector<std::pair<std::string, Golden>> base, over;
            if (baseExists)
                loadGolden(basePath, base);
            if (overExists)
            {
                if (!baseExists)
                    harnessError("overlay " + overPath.string() + " has no base " + basePath.string());
                else
                    loadGolden(overPath, over);
            }
            std::map<std::string, std::size_t, std::less<>> index;
            for (std::size_t i = 0; i < base.size(); ++i)
                index.emplace(base[i].first, i);
            for (auto& [key, g] : over)
            {
                if (key.starts_with("xarch."))
                    harnessError(overPath.string() + ": xarch. key '" + key + "' is not allowed in an overlay");
                else if (const auto it = index.find(key); it == index.end())
                    harnessError(overPath.string() + ": overlay-only key '" + key + "' (not in base)");
                else
                    base[it->second].second = g;
            }

            // 2. every <probe>.<key>.lines sidecar in the base scope directory
            std::map<std::string, std::vector<std::string>, std::less<>> sidecars;
            if (fs::is_directory(baseDir, ec))
                for (const auto& e : fs::directory_iterator(baseDir, ec))
                {
                    const std::string name = e.path().filename().string();
                    if (name.size() <= prefix.size() + suffix.size() || !name.starts_with(prefix)
                        || !name.ends_with(suffix))
                        continue;
                    const std::string key = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
                    if (!detail::validKey(key))
                        continue;
                    std::string content;
                    if (!detail::readFile(e.path(), content))
                        harnessError("cannot read " + e.path().string());
                    else
                        sidecars.emplace(key, detail::splitLines(content));
                }
            haveGolden = baseExists || overExists || !sidecars.empty();

            // 3. compare
            for (const auto& r : rows_)
            {
                ++goldenRows;
                if (!haveGolden)
                    continue;
                const auto it = index.find(r.key);
                const std::string runTol = r.tol.text();
                if (it == index.end())
                {
                    ++goldenNew;
                    std::printf("NEW      %s  got %s  (%s)\n", r.key.c_str(), r.value.c_str(), runTol.c_str());
                    diff.push_back("+" + r.key + "\t" + r.value + "\t" + runTol);
                    continue;
                }
                const Golden& g = base[it->second].second;
                bool ok = false;
                if (g.tolText != runTol)
                    std::printf("DRIFT    %s  tolerance: golden %s, this run %s  (golden %s, got %s)\n", r.key.c_str(),
                                g.tolText.c_str(), runTol.c_str(), g.value.c_str(), r.value.c_str());
                else if (g.tol.kind == Tol::Kind::exact)
                {
                    ok = r.value == g.value;
                    if (!ok)
                        std::printf("DRIFT    %s  golden %s  got %s\n", r.key.c_str(), g.value.c_str(),
                                    r.value.c_str());
                }
                else
                {
                    ok = r.numeric && detail::tolAccepts(g.tol, r.num, g.num);
                    if (!ok)
                        std::printf("DRIFT    %s  golden %s (%s)  got %s  (delta %+.4g)\n", r.key.c_str(),
                                    g.value.c_str(), g.tolText.c_str(), r.value.c_str(), r.num - g.num);
                }
                if (ok && verbose_)
                    std::printf("OK       %s  %s (%s)\n", r.key.c_str(), r.value.c_str(), runTol.c_str());
                if (!ok)
                {
                    ++goldenFail;
                    diff.push_back("-" + r.key + "\t" + g.value + "\t" + g.tolText);
                    diff.push_back("+" + r.key + "\t" + r.value + "\t" + runTol);
                }
            }
            for (const auto& lr : lines_)
            {
                ++goldenRows;
                if (!haveGolden)
                    continue;
                const auto it = sidecars.find(lr.key);
                if (it == sidecars.end())
                {
                    ++goldenNew;
                    std::printf("NEW      %s  lines (%zu)\n", lr.key.c_str(), lr.v.size());
                    diff.push_back("+lines " + lr.key + " (" + std::to_string(lr.v.size()) + " lines)");
                    continue;
                }
                if (it->second == lr.v)
                {
                    if (verbose_)
                        std::printf("OK       %s  lines (%zu)\n", lr.key.c_str(), lr.v.size());
                    continue;
                }
                ++goldenFail;
                const auto d = detail::diffLines(it->second, lr.v);
                std::printf("DRIFT    %s  lines: %zu golden, %zu this run, %zu diff lines\n", lr.key.c_str(),
                            it->second.size(), lr.v.size(), d.size());
                for (std::size_t k = 0; k < d.size() && k < 20; ++k)
                    std::printf("           %s\n", d[k].c_str());
                diff.push_back("@@ lines " + lr.key + "  (" + (baseDir / (prefix + lr.key + suffix)).string() + ")");
                diff.insert(diff.end(), d.begin(), d.end());
            }
            if (!quick_)
            {
                for (const auto& [key, g] : base)
                    if (!goldenKeys_.contains(key) && wants(key))
                    {
                        ++goldenMissingRows;
                        std::printf("MISSING  %s  golden %s (%s): not produced by this run\n", key.c_str(),
                                    g.value.c_str(), g.tolText.c_str());
                        diff.push_back("-" + key + "\t" + g.value + "\t" + g.tolText);
                    }
                for (const auto& [key, v] : sidecars)
                    if (!goldenKeys_.contains(key) && wants(key))
                    {
                        ++goldenMissingRows;
                        std::printf("MISSING  %s  lines (%zu): not produced by this run\n", key.c_str(), v.size());
                        diff.push_back("-lines " + key + " (" + std::to_string(v.size()) + " lines)");
                    }
            }
            if (!haveGolden && goldenRows > 0)
            {
                goldenNew = goldenRows;
                std::printf("MISSING GOLDEN  %s: %d rows are new\n", basePath.string().c_str(), goldenRows);
            }
            else if (!diff.empty())
                diff.insert(diff.begin(),
                            "# golden " + basePath.string() + (overExists ? " + " + overPath.string() : ""));
        }
        const bool drift = haveGolden && (goldenFail + goldenNew + goldenMissingRows) > 0;

        // 4. candidates: the full measured row set, each file atomically (not under --only/--quick or an error)
        if (locatable && !blessTo_.empty())
        {
            const fs::path dir = blessTo_ / arch_ / scope_;
            if (!errors_.empty() || quick_ || !only_.empty())
                std::printf("CANDIDATE not written (%s)\n",
                            !errors_.empty() ? "harness error" : "partial row set under --only/--quick");
            else if (goldenRows > 0)
            {
                std::string err;
                const fs::path txt = dir / (probe_ + ".txt");
                if (!rows_.empty())
                {
                    std::string body = "# funkgui-golden 2  probe=" + probe_ + "  scope=" + scope_ + "\n"
                                     + "# key\tvalue\ttolerance\n";
                    for (const auto& r : rows_)
                        body += r.key + "\t" + r.value + "\t" + r.tol.text() + "\n";
                    if (!detail::writeAtomic(txt, body, err))
                        harnessError(err);
                }
                for (const auto& lr : lines_)
                    if (!detail::writeAtomic(dir / (prefix + lr.key + suffix), detail::joinLines(lr.v), err))
                        harnessError(err);
                const fs::path diffPath = dir / (probe_ + ".diff");
                if (drift)
                {
                    std::string body = "# funkgui-golden-diff 2  probe=" + probe_ + "  scope=" + scope_
                                     + "  arch=" + arch_ + "\n";
                    for (const auto& l : diff)
                        body += l + "\n";
                    if (!detail::writeAtomic(diffPath, body, err))
                        harnessError(err);
                }
                else
                    fs::remove(diffPath, ec);
                if (errors_.empty())
                    std::printf("CANDIDATE %s%s\n", (rows_.empty() ? dir : txt).string().c_str(),
                                drift ? " (+ .diff)" : "");
            }
        }

        // 5. status, results JSON, RESULT line
        const auto statusOf = [&]() -> std::pair<int, const char*>
        {
            if (!errors_.empty())              return { 4, "harness_error" };
            if (specFail_ > 0)                 return { 1, "spec_fail" };
            if (!haveGolden && goldenRows > 0) return { 3, "golden_missing" };
            if (drift)                         return { 2, "golden_drift" };
            return { 0, "pass" };
        };
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t0_).count();
        const auto json = [&]()
        {
            return std::string("{\"probe\":") + detail::jsonString(probe_) + ",\"mode\":" + detail::jsonString(mode_)
                 + ",\"arch\":" + detail::jsonString(arch_) + ",\"status\":\"" + statusOf().second + "\""
                 + ",\"spec_pass\":" + std::to_string(specPass_) + ",\"spec_fail\":" + std::to_string(specFail_)
                 + ",\"golden_rows\":" + std::to_string(goldenRows) + ",\"golden_fail\":" + std::to_string(goldenFail)
                 + ",\"golden_new\":" + std::to_string(goldenNew)
                 + ",\"golden_missing_rows\":" + std::to_string(goldenMissingRows)
                 + ",\"ms\":" + std::to_string(ms) + "}";
        };
        if (!results_.empty())
        {
            std::string err;
            const fs::path out = results_ / (probe_ + (mode_.empty() ? std::string() : "." + mode_) + ".json");
            if (!detail::validKey(probe_))
                harnessError("results not written: invalid probe name");
            else if (!detail::writeAtomic(out, json() + "\n", err))
                harnessError(err);
        }
        std::printf("SUMMARY  %s%s%s  spec %d pass / %d fail  golden %d rows: %d differ, %d new, %d missing\n",
                    probe_.c_str(), mode_.empty() ? "" : " --mode ", mode_.c_str(), specPass_, specFail_, goldenRows,
                    goldenFail, goldenNew, goldenMissingRows);
        std::printf("RESULT %s\n", json().c_str());
        std::fflush(stdout);
        exit_ = statusOf().first;
        return exit_;
    }
} // namespace funkgui::test

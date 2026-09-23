#pragma once

// Shared plumbing for the checkable harnesses.
//
// Two jobs:
//
//   1. Put the tools in the SAME floating-point mode as the plugin. Every
//      processBlock opens with juce::ScopedNoDenormals (PluginProcessor.cpp),
//      so the shipped engine runs with flush-to-zero on. The harnesses did
//      not, and it is not cosmetic: nine of IrAnalyse's configurations
//      fingerprint differently with FZ clear than with FZ set, because the
//      recursive state reaches denormal magnitudes long before the audible
//      tail does. A golden captured in the wrong mode certifies arithmetic
//      the product never executes.
//
//   2. Turn a printed table into a pass/fail. Each tool collects the numbers
//      it already computes as named metrics and hands them to run(); the
//      golden file is the record of what those numbers were when someone last
//      looked at them and agreed. This project is not a git repository, so
//      the golden file IS the history — it is a source artefact, not a build
//      output, and --bless is a deliberate act.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

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

    // setenv/unsetenv are POSIX; the Windows runtime spells both _putenv_s
    // (an empty value removes the variable). Harnesses point the stores at
    // scratch files through the environment, so this must work everywhere.
    inline void setEnv(const char* name, const char* value)
    {
#if defined(_WIN32)
        _putenv_s(name, value);
#else
        ::setenv(name, value, 1);
#endif
    }
    inline void unsetEnv(const char* name)
    {
#if defined(_WIN32)
        _putenv_s(name, "");
#else
        ::unsetenv(name);
#endif
    }

    // tol < 0 means "compare the text exactly" — used for hashes, where any
    // difference at all is the whole point of the measurement.
    static constexpr double kExact = -1.0;

    struct Metric
    {
        std::string key;
        std::string value;
        double      tol = kExact;
    };

    inline void addNum(std::vector<Metric>& m, const std::string& key, double v, double tol)
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.6g", v);
        m.push_back({ key, buf, tol });
    }

    inline void addHash(std::vector<Metric>& m, const std::string& key, uint64_t h)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long) h);
        m.push_back({ key, buf, kExact });
    }

    inline bool writeGolden(const char* path, const std::vector<Metric>& m)
    {
        std::FILE* f = std::fopen(path, "w");
        if (f == nullptr) { std::printf("cannot write %s\n", path); return false; }
        std::fprintf(f, "# key\tvalue\ttolerance ('exact' or an absolute number)\n");
        std::fprintf(f, "# Regenerate deliberately with --bless; review the diff.\n");
        for (const auto& x : m)
        {
            if (x.tol < 0.0) std::fprintf(f, "%s\t%s\texact\n", x.key.c_str(), x.value.c_str());
            else             std::fprintf(f, "%s\t%s\t%g\n", x.key.c_str(), x.value.c_str(), x.tol);
        }
        std::fclose(f);
        std::printf("blessed %zu metrics -> %s\n", m.size(), path);
        return true;
    }

    // Returns a process exit code. Every mismatch is printed, not just the
    // first: a run that changed six numbers should say so in one pass.
    inline int checkGolden(const char* path, const std::vector<Metric>& m)
    {
        std::FILE* f = std::fopen(path, "r");
        if (f == nullptr)
        {
            std::printf("FAIL  no golden at %s (run --bless once, review, keep it)\n", path);
            return 2;
        }
        std::vector<Metric> want;
        char line[512];
        while (std::fgets(line, sizeof(line), f) != nullptr)
        {
            if (line[0] == '#' || line[0] == '\n') continue;
            char k[256], v[128], t[64];
            if (std::sscanf(line, "%255s\t%127s\t%63s", k, v, t) != 3) continue;
            want.push_back({ k, v, std::strcmp(t, "exact") == 0 ? kExact : std::atof(t) });
        }
        std::fclose(f);

        int bad = 0;
        for (const auto& w : want)
        {
            const Metric* got = nullptr;
            for (const auto& x : m) if (x.key == w.key) { got = &x; break; }
            if (got == nullptr)
            {
                std::printf("FAIL  %-34s missing from this run (golden expects %s)\n",
                            w.key.c_str(), w.value.c_str());
                ++bad;
                continue;
            }
            if (w.tol < 0.0)
            {
                if (got->value != w.value)
                {
                    std::printf("FAIL  %-34s expected %s  got %s\n",
                                w.key.c_str(), w.value.c_str(), got->value.c_str());
                    ++bad;
                }
            }
            else
            {
                const double a = std::atof(got->value.c_str());
                const double b = std::atof(w.value.c_str());
                if (!(std::fabs(a - b) <= w.tol))
                {
                    std::printf("FAIL  %-34s expected %g +/- %g  got %g  (drift %+.4g)\n",
                                w.key.c_str(), b, w.tol, a, a - b);
                    ++bad;
                }
            }
        }
        for (const auto& x : m)
        {
            bool known = false;
            for (const auto& w : want) if (w.key == x.key) { known = true; break; }
            if (!known)
                std::printf("FAIL  %-34s not in the golden (new metric: --bless to adopt)\n",
                            x.key.c_str());
            if (!known) ++bad;
        }

        if (bad == 0) std::printf("PASS  %zu metrics match %s\n", m.size(), path);
        else          std::printf("FAIL  %d of %zu metrics differ from %s\n",
                                  bad, m.size(), path);
        return bad == 0 ? 0 : 1;
    }

    // Call at the end of main() with everything the tool measured.
    inline int finish(int argc, char** argv, const std::vector<Metric>& m)
    {
        for (int i = 1; i < argc; ++i)
        {
            if (std::strcmp(argv[i], "--check") == 0 && i + 1 < argc)
                return checkGolden(argv[i + 1], m);
            if (std::strcmp(argv[i], "--bless") == 0 && i + 1 < argc)
                return writeGolden(argv[i + 1], m) ? 0 : 2;
        }
        return 0;
    }
}

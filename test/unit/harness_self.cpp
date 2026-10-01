// FUNKGUI_TEST name=fg.harness.self timeout=600 gpu=0 links=harness
//
// fg.harness.self: Harness v2 against its own contract (FCompressor docs/design/03-build-verify-process.md §3.2.1-§3.2.4,
// 02 §3.11) and the G1 additions (note, setEnv/unsetEnv, positionals, jsonArray): the tolerance grammar, duplicate
// keys, golden files, arch overlays, `lines` sidecars, exit codes and their order, --only/--quick, the results JSON and
// the RESULT line, and atomic candidate writes; since the S0 review (R-G1 #3, #4, #6, #7), --bless-to outside the
// golden root, stale candidate removal, num() compared as printed and non-finite values refused. Every check is a
// spec row of this probe; the probes under test run in-process against scratch golden trees under <cwd>/work (ctest
// runs this in <build>/sandbox/fg.harness.self), with their stdout captured so that only this probe's RESULT line
// reaches CTest.

#include <funkgui/test/Harness.h>

#include <fcntl.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
namespace T = funkgui::test;

namespace
{
    // Redirects stdout (fd 1) into a file for the lifetime of the object.
    class CaptureStdout
    {
    public:
        explicit CaptureStdout(const fs::path& log)
        {
            std::fflush(stdout);
            saved_ = ::dup(1);
            const int fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0)
            {
                ::dup2(fd, 1);
                ::close(fd);
            }
        }
        ~CaptureStdout()
        {
            std::fflush(stdout);
            if (saved_ >= 0)
            {
                ::dup2(saved_, 1);
                ::close(saved_);
            }
        }
        CaptureStdout(const CaptureStdout&) = delete;
        CaptureStdout& operator=(const CaptureStdout&) = delete;

    private:
        int saved_ = -1;
    };

    std::string slurp(const fs::path& p)
    {
        std::ifstream f(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }

    void put(const fs::path& p, std::string_view content)
    {
        fs::create_directories(p.parent_path());
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    bool has(std::string_view hay, std::string_view needle) { return hay.find(needle) != std::string_view::npos; }

    // One scratch golden tree, candidate directory and results directory per case.
    struct Sandbox
    {
        fs::path root;
        fs::path golden() const { return root / "golden"; }
        fs::path cand() const { return root / "cand"; }
        fs::path res() const { return root / "res"; }
        fs::path base(std::string_view probe, std::string_view scope = "global") const
        {
            return golden() / "base" / std::string(scope) / (std::string(probe) + ".txt");
        }
        fs::path overlay(std::string_view arch, std::string_view probe, std::string_view scope = "global") const
        {
            return golden() / std::string(arch) / std::string(scope) / (std::string(probe) + ".txt");
        }
        // The standard command line of a probe executable (03 §3.2.2), plus extra tokens.
        std::vector<std::string> args(std::vector<std::string> extra = {}, std::string_view arch = "arm64") const
        {
            std::vector<std::string> a{ "harness_self", "self.case", "--golden-root", golden().string(), "--arch",
                                        std::string(arch), "--bless-to", cand().string(), "--results", res().string() };
            a.insert(a.end(), extra.begin(), extra.end());
            return a;
        }
    };

    struct Outcome
    {
        int exit = -1;
        std::string json;     // the RESULT line's object
        std::string out;      // everything the probe printed
    };

    class Suite
    {
    public:
        explicit Suite(T::Probe& self) : self_(self), work_(fs::current_path() / "work")
        {
            std::error_code ec;
            fs::remove_all(work_, ec);
            fs::create_directories(work_);
        }

        Sandbox fresh(std::string_view name) const
        {
            Sandbox s{ work_ / std::string(name) };
            fs::create_directories(s.root);
            return s;
        }

        template <class Body>
        Outcome run(const Sandbox& s, std::string_view probe, std::string_view mode, std::vector<std::string> args,
                    Body&& body) const
        {
            std::vector<char*> argv;
            for (auto& a : args)
                argv.push_back(a.data());
            argv.push_back(nullptr);
            Outcome o;
            const fs::path log = s.root / "stdout.log";
            {
                const CaptureStdout capture(log);
                T::Probe P(probe, mode, static_cast<int>(args.size()), argv.data());
                body(P);
                o.exit = P.finish();
            }
            o.out = slurp(log);
            if (const auto at = o.out.rfind("RESULT "); at != std::string::npos)
                o.json = o.out.substr(at + 7, o.out.find('\n', at) - at - 7);
            return o;
        }

        bool eq(std::string_view key, long long got, long long want) { return self_.eq(key, got, want); }
        bool yes(std::string_view key, bool ok) { return self_.eq(key, ok ? 1 : 0, 1); }

    private:
        T::Probe& self_;
        fs::path work_;
    };

    // ---- Tol grammar ------------------------------------------------------------------------------------------------
    void tolerances(Suite& S)
    {
        struct Good { const char* key; const char* in; const char* text; };
        static constexpr Good good[] = {
            { "tol.exact",   "exact",             "exact" },
            { "tol.abs",     "abs:0.01",          "abs:0.01" },
            { "tol.rel",     "rel:0.1",           "rel:0.1" },
            { "tol.absrel",  "absrel:0.001:0.01", "absrel:0.001:0.01" },
            { "tol.le",      "le:0",              "le:0" },
            { "tol.ge",      "ge:2.5",            "ge:2.5" },
            { "tol.bare",    "0.5",               "abs:0.5" },      // HR v1 files: a bare number is abs
            { "tol.negzero", "abs:-0",            "abs:0" },
        };
        for (const auto& g : good)
        {
            const auto t = T::Tol::parse(g.in);
            S.yes(g.key, t.has_value() && t->text() == g.text);
        }
        static constexpr const char* bad[] = { "", "abs:", "abs:-1", "abs:nan", "abs:inf", "rel:", "absrel:1",
                                               "absrel:1:", "absrel::1", "le:x", "abs:1 ", " abs:1", "foo", "-1",
                                               "Exact", "ge:1e999" };
        int rejected = 0;
        for (const char* b : bad)
            rejected += T::Tol::parse(b).has_value() ? 0 : 1;
        S.eq("tol.bad.rejected", rejected, static_cast<long long>(std::size(bad)));
        S.yes("tol.text.format", T::Tol::abs(1.0 / 3.0).text() == "abs:0.333333333");
    }

    // ---- hashes -----------------------------------------------------------------------------------------------------
    void hashes(Suite& S)
    {
        // HR's FNV-1a variant (offset 1469598103934665603, not the standard 14695981039346656037): kept bit for bit,
        // because HR's golden hashes (and its migration onto FunkGui) depend on it.
        S.yes("hash.fnv1a.empty", T::fnv1a("", 0) == 0x14650fb0739d0383ull);
        S.yes("hash.fnv1a.a", T::fnv1a("a", 1) == 0x44bd8ad473cd9906ull);
        const float one[] = { 1.0f };
        const unsigned char le[] = { 0x00, 0x00, 0x80, 0x3f };
        S.yes("hash.floats.le", T::hashFloats(one) == T::fnv1a(le, sizeof le));
    }

    // ---- exit codes, statuses and their order (03 §3.2.4) -----------------------------------------------------------
    void exitCodes(Suite& S)
    {
        {
            const Sandbox sb = S.fresh("pass-spec-only");
            const auto o = S.run(sb, "self.pass", "", sb.args(), [](T::Probe& P) { P.near("x", 1.0, 1.0, 0); });
            S.eq("exit.pass.spec_only", o.exit, 0);
            S.yes("exit.pass.status", has(o.json, "\"status\":\"pass\""));
            S.yes("exit.pass.no_candidate", !fs::exists(sb.cand()));      // no golden rows: nothing to bless
        }
        {
            const Sandbox sb = S.fresh("spec-fail");
            const auto o = S.run(sb, "self.fail", "", sb.args(), [](T::Probe& P) {
                P.le("a", 2.0, 1.0);
                P.ge("b", 2.0, 1.0);
            });
            S.eq("exit.spec_fail", o.exit, 1);
            S.yes("exit.spec_fail.counts", has(o.json, "\"spec_pass\":1,\"spec_fail\":1"));
        }
        {
            const Sandbox sb = S.fresh("missing");
            const auto o = S.run(sb, "self.missing", "", sb.args(), [](T::Probe& P) {
                P.num("a", 0.1, T::Tol::abs(0.01));
            });
            S.eq("exit.golden_missing", o.exit, 3);
            S.yes("exit.golden_missing.status",
                  has(o.json, "\"status\":\"golden_missing\",") && has(o.json, "\"golden_new\":1"));
            const std::string cand = slurp(sb.cand() / "arm64" / "global" / "self.missing.txt");
            S.yes("candidate.missing.content", cand == "# funkgui-golden 2  probe=self.missing  scope=global\n"
                                                      "# key\tvalue\ttolerance\na\t0.1\tabs:0.01\n");
        }
        {
            const Sandbox sb = S.fresh("drift");
            put(sb.base("self.drift"), "# funkgui-golden 2\na\t1\texact\nb\t2\texact\n");
            const auto o = S.run(sb, "self.drift", "", sb.args(), [](T::Probe& P) {
                P.num("a", 1, T::Tol::exact());
                P.num("b", 3, T::Tol::exact());
            });
            S.eq("exit.golden_drift", o.exit, 2);
            S.yes("exit.golden_drift.counts", has(o.json, "\"golden_rows\":2,\"golden_fail\":1,\"golden_new\":0"));
            const std::string diff = slurp(sb.cand() / "arm64" / "global" / "self.drift.diff");
            S.yes("candidate.drift.diff", has(diff, "-b\t2\texact\n+b\t3\texact\n"));
            // The same run matching the golden passes, and the stale .diff is removed.
            const auto again = S.run(sb, "self.drift", "", sb.args(), [](T::Probe& P) {
                P.num("a", 1, T::Tol::exact());
                P.num("b", 2, T::Tol::exact());
            });
            S.eq("exit.golden_pass", again.exit, 0);
            S.yes("candidate.pass.diff_removed", !fs::exists(sb.cand() / "arm64" / "global" / "self.drift.diff"));
        }
        {
            const Sandbox sb = S.fresh("order");
            put(sb.base("self.order"), "a\t1\texact\n");
            const auto specOverDrift = S.run(sb, "self.order", "", sb.args(), [](T::Probe& P) {
                P.num("a", 2, T::Tol::exact());
                P.eq("n", 1, 2);
            });
            S.eq("exit.order.spec_over_drift", specOverDrift.exit, 1);
            S.yes("exit.order.drift_still_reported", has(specOverDrift.json, "\"golden_fail\":1"));
            const auto errorOverSpec = S.run(sb, "self.order", "", sb.args(), [](T::Probe& P) {
                P.num("a", 1, T::Tol::exact());
                P.eq("n", 1, 2);
                P.harnessError("forced by the self-test");
            });
            S.eq("exit.order.error_over_spec", errorOverSpec.exit, 4);
            S.yes("exit.order.error_no_candidate", has(errorOverSpec.out, "CANDIDATE not written (harness error)"));
        }
        {
            const Sandbox sb = S.fresh("new-and-missing-rows");
            put(sb.base("self.rows"), "a\t1\texact\nb\t2\texact\n");
            const auto o = S.run(sb, "self.rows", "", sb.args(), [](T::Probe& P) {
                P.num("a", 1, T::Tol::exact());
                P.num("c", 3, T::Tol::exact());
            });
            S.eq("rows.new_and_missing.exit", o.exit, 2);
            S.yes("rows.new_and_missing.counts", has(o.json, "\"golden_new\":1,\"golden_missing_rows\":1"));
        }
    }

    // ---- tolerance semantics against a golden -----------------------------------------------------------------------
    void goldenTolerances(Suite& S)
    {
        const Sandbox sb = S.fresh("tolerances");
        put(sb.base("self.tol"), "x.exact\t1.5\texact\nx.abs\t1\tabs:0.1\nx.rel\t100\trel:0.01\n"
                                 "x.absrel\t0\tabsrel:0.5:0.01\nx.le\t10\tle:1\nx.ge\t10\tge:1\n"
                                 "x.hash\t00000000000000ff\texact\nx.text\tGRAPHITE\texact\n");
        const auto within = S.run(sb, "self.tol", "", sb.args(), [](T::Probe& P) {
            P.num("x.exact", 1.5, T::Tol::exact());
            P.num("x.abs", 1.05, T::Tol::abs(0.1));
            P.num("x.rel", 100.9, T::Tol::rel(0.01));
            P.num("x.absrel", 0.4, T::Tol::absrel(0.5, 0.01));
            P.num("x.le", 3, T::Tol::le(1));                    // an improvement passes
            P.num("x.ge", 9.5, T::Tol::ge(1));
            P.hash("x.hash", 0xff);
            P.text("x.text", "GRAPHITE");
        });
        S.eq("golden.tol.within", within.exit, 0);
        const auto outside = S.run(sb, "self.tol", "", sb.args(), [](T::Probe& P) {
            P.num("x.exact", 1.5000001, T::Tol::exact());
            P.num("x.abs", 1.2, T::Tol::abs(0.1));
            P.num("x.rel", 102, T::Tol::rel(0.01));
            P.num("x.absrel", 0.6, T::Tol::absrel(0.5, 0.01));
            P.num("x.le", 11.5, T::Tol::le(1));
            P.num("x.ge", 8.5, T::Tol::ge(1));
            P.hash("x.hash", 0xfe);
            P.text("x.text", "PAPER");
        });
        S.eq("golden.tol.outside", outside.exit, 2);
        S.yes("golden.tol.outside.count", has(outside.json, "\"golden_rows\":8,\"golden_fail\":8"));
        const auto retol = S.run(sb, "self.tol", "", sb.args(), [](T::Probe& P) {
            P.num("x.exact", 1.5, T::Tol::exact());
            P.num("x.abs", 1, T::Tol::abs(0.2));                // same value, different tolerance: drift
            P.num("x.rel", 100, T::Tol::rel(0.01));
            P.num("x.absrel", 0, T::Tol::absrel(0.5, 0.01));
            P.num("x.le", 10, T::Tol::le(1));
            P.num("x.ge", 10, T::Tol::ge(1));
            P.hash("x.hash", 0xff);
            P.text("x.text", "GRAPHITE");
        });
        S.yes("golden.tol.changed_tolerance_drifts", retol.exit == 2 && has(retol.json, "\"golden_fail\":1"));
    }

    // ---- keys and values --------------------------------------------------------------------------------------------
    void keys(Suite& S)
    {
        const Sandbox sb = S.fresh("keys");
        const auto error = [&](std::string_view key, auto body) {
            S.eq(key, S.run(sb, "self.keys", "", sb.args(), body).exit, 4);
        };
        error("key.upper_case", [](T::Probe& P) { P.num("Bad", 1, T::Tol::exact()); });
        error("key.space", [](T::Probe& P) { P.num("a b", 1, T::Tol::exact()); });
        error("key.leading_dot", [](T::Probe& P) { P.eq(".a", 1, 1); });
        error("key.too_long", [](T::Probe& P) { P.num(std::string(121, 'a'), 1, T::Tol::exact()); });
        error("key.dup_golden", [](T::Probe& P) { P.num("a", 1, T::Tol::exact()); P.hash("a", 1); });
        error("key.dup_spec", [](T::Probe& P) { P.eq("a", 1, 1); P.le("a", 1, 2); });
        error("value.whitespace", [](T::Probe& P) { P.text("t", "two words"); });
        error("value.too_long", [](T::Probe& P) { P.text("t", std::string(65, 'x')); });
        error("value.empty", [](T::Probe& P) { P.text("t", ""); });
        error("value.bad_tolerance", [](T::Probe& P) { P.num("a", 1, T::Tol::abs(-1)); });
        error("lines.newline", [](T::Probe& P) {
            const std::string v[] = { "ok", "bad\nline" };
            P.lines("l", v);
        });
        error("probe.harness_error", [](T::Probe& P) { P.harnessError("from the probe"); });
        const auto maxKey = S.run(sb, "self.keys", "", sb.args(), [](T::Probe& P) {
            P.eq(std::string(120, 'k'), 1, 1);
        });
        S.eq("key.max_length_ok", maxKey.exit, 0);
        // Spec and golden keys are separate namespaces: one quantity may carry both.
        put(sb.base("self.both"), "q\t5\tabs:0.5\n");
        const auto both = S.run(sb, "self.both", "", sb.args(), [](T::Probe& P) {
            P.near("q", 5, 5, 0.1);
            P.num("q", 5, T::Tol::abs(0.5));
        });
        S.eq("key.spec_and_golden_same_key", both.exit, 0);
    }

    // ---- golden file errors and overlays (03 §3.2.3, §3.3) ----------------------------------------------------------
    void goldenFiles(Suite& S)
    {
        const auto body = [](T::Probe& P) {
            P.num("a", 1, T::Tol::exact());
            P.num("b", 2, T::Tol::exact());
        };
        const auto errorWith = [&](std::string_view key, std::string_view content) {
            const Sandbox sb = S.fresh(key);
            put(sb.base("self.file"), content);
            S.eq(key, S.run(sb, "self.file", "", sb.args(), body).exit, 4);
        };
        errorWith("file.two_fields", "a\t1\nb\t2\texact\n");
        errorWith("file.four_fields", "a\t1\texact\textra\nb\t2\texact\n");
        errorWith("file.duplicate_key", "a\t1\texact\na\t1\texact\nb\t2\texact\n");
        errorWith("file.bad_key", "A\t1\texact\nb\t2\texact\n");
        errorWith("file.bad_tolerance", "a\t1\tabs:x\nb\t2\texact\n");
        errorWith("file.numeric_tolerance_on_text", "a\tone\tabs:0.1\nb\t2\texact\n");
        {
            const Sandbox sb = S.fresh("file-comments");
            put(sb.base("self.file"), "# funkgui-golden 2\n\n# key\tvalue\ttolerance\na\t1\texact\nb\t2\texact\n");
            S.eq("file.comments_and_blank_lines", S.run(sb, "self.file", "", sb.args(), body).exit, 0);
        }
        {
            const Sandbox sb = S.fresh("overlay");
            put(sb.base("self.over"), "a\t1\texact\nb\t2\texact\n");
            put(sb.overlay("arm64", "self.over"), "b\t3\texact\n");
            const auto on = S.run(sb, "self.over", "", sb.args(), [](T::Probe& P) {
                P.num("a", 1, T::Tol::exact());
                P.num("b", 3, T::Tol::exact());
            });
            S.eq("overlay.applies", on.exit, 0);
            const auto base = S.run(sb, "self.over", "", sb.args({}, "x86_64"), [](T::Probe& P) {
                P.num("a", 1, T::Tol::exact());
                P.num("b", 2, T::Tol::exact());
            });
            S.eq("overlay.other_arch_uses_base", base.exit, 0);
            const auto stale = S.run(sb, "self.over", "", sb.args(), body);
            S.eq("overlay.base_value_drifts_on_arm64", stale.exit, 2);
            // wasm32 (v0.12.0) is an architecture like the other two: its own overlay, and nobody else's.
            put(sb.overlay("wasm32", "self.over"), "a\t7\texact\n");
            const auto wasm = S.run(sb, "self.over", "", sb.args({}, "wasm32"), [](T::Probe& P) {
                P.num("a", 7, T::Tol::exact());
                P.num("b", 2, T::Tol::exact());
            });
            S.eq("overlay.wasm32_applies", wasm.exit, 0);
            S.eq("overlay.wasm32_is_not_x86_64s", S.run(sb, "self.over", "", sb.args({}, "x86_64"), body).exit, 0);
        }
        {
            const Sandbox sb = S.fresh("overlay-only-key");
            put(sb.base("self.over"), "a\t1\texact\nb\t2\texact\n");
            put(sb.overlay("arm64", "self.over"), "c\t3\texact\n");
            S.eq("overlay.only_key", S.run(sb, "self.over", "", sb.args(), body).exit, 4);
        }
        {
            const Sandbox sb = S.fresh("overlay-xarch");
            put(sb.base("self.over"), "a\t1\texact\nb\t2\texact\nxarch.h\t00000000000000aa\texact\n");
            put(sb.overlay("arm64", "self.over"), "xarch.h\t00000000000000bb\texact\n");
            S.eq("overlay.xarch", S.run(sb, "self.over", "", sb.args(), body).exit, 4);
        }
        {
            const Sandbox sb = S.fresh("overlay-no-base");
            put(sb.overlay("arm64", "self.over"), "a\t1\texact\n");
            S.eq("overlay.without_base", S.run(sb, "self.over", "", sb.args(), body).exit, 4);
        }
        {
            const Sandbox sb = S.fresh("mode-scope");
            put(sb.base("self.mode", "modes/bus-g"), "a\t1\texact\n");
            const auto o = S.run(sb, "self.mode", "bus-g", sb.args({ "--mode", "bus-g" }), [](T::Probe& P) {
                P.num("a", 1, T::Tol::exact());
            });
            S.eq("scope.mode.pass", o.exit, 0);
            S.yes("scope.mode.results_name", fs::exists(sb.res() / "self.mode.bus-g.json"));
            S.yes("scope.mode.candidate_dir", fs::exists(sb.cand() / "arm64" / "modes" / "bus-g" / "self.mode.txt"));
        }
    }

    // ---- lines sidecars ---------------------------------------------------------------------------------------------
    void sidecars(Suite& S)
    {
        const Sandbox sb = S.fresh("lines");
        put(sb.base("self.lines"), "n\t2\texact\n");
        put(sb.golden() / "base" / "global" / "self.lines.a11y.lines", "Button MODE\nSlider THRESHOLD\n");
        const auto same = S.run(sb, "self.lines", "", sb.args(), [](T::Probe& P) {
            P.num("n", 2, T::Tol::exact());
            const std::string v[] = { "Button MODE", "Slider THRESHOLD" };
            P.lines("a11y", v);
        });
        S.eq("lines.equal", same.exit, 0);
        S.yes("lines.candidate", slurp(sb.cand() / "arm64" / "global" / "self.lines.a11y.lines")
                                     == "Button MODE\nSlider THRESHOLD\n");
        const auto changed = S.run(sb, "self.lines", "", sb.args(), [](T::Probe& P) {
            P.num("n", 2, T::Tol::exact());
            const std::string v[] = { "Button MODE", "Slider RATIO", "Slider THRESHOLD" };
            P.lines("a11y", v);
        });
        S.eq("lines.changed", changed.exit, 2);
        const std::string diff = slurp(sb.cand() / "arm64" / "global" / "self.lines.diff");
        S.yes("lines.diff", has(diff, "@@ lines a11y") && has(diff, "+2\tSlider RATIO"));
        const auto added = S.run(sb, "self.lines", "", sb.args(), [](T::Probe& P) {
            P.num("n", 2, T::Tol::exact());
            const std::string v[] = { "Button MODE", "Slider THRESHOLD" };
            P.lines("a11y", v);
            const std::string w[] = { "Tab 1" };
            P.lines("tabs", w);
        });
        S.yes("lines.new_sidecar_drifts", added.exit == 2 && has(added.json, "\"golden_new\":1"));
    }

    // ---- command line (03 §3.2.2), --only, --quick, positionals -----------------------------------------------------
    void commandLine(Suite& S)
    {
        const Sandbox sb = S.fresh("flags");
        const auto spec = [](T::Probe& P) { P.eq("x", 1, 1); };
        const auto exitOf = [&](std::vector<std::string> args, std::string_view mode = "") {
            std::vector<std::string> a{ "harness_self", "self.flags" };
            a.insert(a.end(), args.begin(), args.end());
            return S.run(sb, "self.flags", mode, a, spec).exit;
        };
        const std::string g = sb.golden().string();
        S.eq("flags.minimal", exitOf({ "--golden-root", g, "--arch", "arm64" }), 0);
        S.eq("flags.unknown", exitOf({ "--golden-root", g, "--arch", "arm64", "--bless" }), 4);
        S.eq("flags.check_is_unknown", exitOf({ "--golden-root", g, "--arch", "arm64", "--check" }), 4);
        S.eq("flags.no_golden_root", exitOf({ "--arch", "arm64" }), 4);
        S.eq("flags.no_arch", exitOf({ "--golden-root", g }), 4);
        S.eq("flags.bad_arch", exitOf({ "--golden-root", g, "--arch", "ppc" }), 4);
        S.eq("flags.arch_wasm32", exitOf({ "--golden-root", g, "--arch", "wasm32" }), 0);
        S.eq("flags.value_missing", exitOf({ "--golden-root", g, "--arch", "arm64", "--results" }), 4);
        S.eq("flags.value_is_flag", exitOf({ "--golden-root", "--arch", "arm64" }), 4);
        S.eq("flags.mode_mismatch", exitOf({ "--golden-root", g, "--arch", "arm64", "--mode", "clean" }, "bus-g"), 4);
        S.eq("flags.mode_match", exitOf({ "--golden-root", g, "--arch", "arm64", "--mode", "clean" }, "clean"), 0);
        S.eq("flags.positionals_ignored", exitOf({ "input.ttf", "--golden-root", g, "--arch", "arm64", "out.png" }), 0);

        put(sb.base("self.only"), "a.x\t1\texact\na.y\t2\texact\nb.z\t3\texact\n");
        const auto only = S.run(sb, "self.only", "", sb.args({ "--only", "a.*" }), [](T::Probe& P) {
            P.num("a.x", 1, T::Tol::exact());
            P.num("a.y", 2, T::Tol::exact());
            P.num("b.z", 99, T::Tol::exact());                  // not selected: neither compared nor written
            P.eq("b.spec", 1, 2);                               // not selected: not judged
        });
        S.eq("only.exit", only.exit, 0);
        S.yes("only.no_candidate", has(only.out, "CANDIDATE not written (partial row set under --only/--quick)"));
        bool globs = false;
        S.run(sb, "self.wants", "", sb.args({ "--only", "fg.*", "--only", "ui.?" }), [&](T::Probe& P) {
            globs = P.wants("fg.font.atlas") && P.wants("ui.a") && !P.wants("ui.ab") && !P.wants("dsp.x");
        });
        S.yes("only.globs", globs);
        const auto quick = S.run(sb, "self.only", "", sb.args({ "--quick" }), [](T::Probe& P) {
            if (!P.quick())
                P.harnessError("quick() is false under --quick");
            P.num("a.x", 1, T::Tol::exact());                   // a.y and b.z not produced: not missing under --quick
        });
        S.eq("quick.missing_rows_not_counted", quick.exit, 0);

        char p0[] = "tool", p1[] = "fg.x", p2[] = "--golden-root", p3[] = "/g", p4[] = "in.ttf", p5[] = "--arch",
             p6[] = "arm64", p7[] = "--quick", p8[] = "--only", p9[] = "a*", p10[] = "out.png", p11[] = "--mode",
             p12[] = "--verbose", p13[] = "last", p14[] = "--results";
        char* pv[] = { p0, p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12, p13, p14, nullptr };
        const auto pos = T::positionals(15, pv);
        // --mode followed by a flag has no value: the Probe reports it; positionals() does not swallow --verbose.
        S.yes("positionals.split", pos == std::vector<std::string>{ "fg.x", "in.ttf", "out.png", "last" });
    }

    // ---- results JSON, RESULT line and note() -----------------------------------------------------------------------
    void results(Suite& S)
    {
        const Sandbox sb = S.fresh("results");
        const auto o = S.run(sb, "self.res", "", sb.args(), [](T::Probe& P) {
            P.eq("x", 1, 1);
            const std::string keys[] = { "clean", "bus-g" };
            P.note("provisional", T::jsonArray(keys));
            P.note("count", "3");
            P.note("nested", R"({"a":[1,-2.5e-3,true,false,null,"q\"\u00e9"],"b":{}})");
        });
        S.eq("results.exit", o.exit, 0);
        const std::string file = slurp(sb.res() / "self.res.json");
        S.yes("results.file_equals_result_line", !o.json.empty() && file == o.json + "\n");
        S.yes("results.standard_members", o.json.starts_with("{\"probe\":\"self.res\",\"mode\":\"\",\"arch\":\"arm64\","
                                                             "\"status\":\"pass\",\"spec_pass\":1,\"spec_fail\":0,"
                                                             "\"golden_rows\":0,\"golden_fail\":0,\"golden_new\":0,"
                                                             "\"golden_missing_rows\":0,\"ms\":"));
        S.yes("results.notes_in_order",
              o.json.ends_with(",\"provisional\":[\"clean\",\"bus-g\"],\"count\":3,"
                               "\"nested\":{\"a\":[1,-2.5e-3,true,false,null,\"q\\\"\\u00e9\"],\"b\":{}}}"));

        const auto noteError = [&](std::string_view key, std::string_view noteKey, std::string_view value) {
            S.eq(key, S.run(sb, "self.res", "", sb.args(), [&](T::Probe& P) { P.note(noteKey, value); }).exit, 4);
        };
        noteError("note.reserved_key", "status", "1");
        noteError("note.bad_key", "Count", "1");
        noteError("note.empty_value", "n", "");
        noteError("note.two_values", "n", "1 2");
        noteError("note.trailing_comma", "n", "[1,]");
        noteError("note.newline", "n", "[1,\n2]");
        noteError("note.bare_word", "n", "clean");
        noteError("note.leading_zero", "n", "01");
        noteError("note.bad_escape", "n", R"("\x")");
        const auto dup = S.run(sb, "self.res", "", sb.args(), [](T::Probe& P) {
            P.note("n", "1");
            P.note("n", "2");
        });
        S.eq("note.duplicate", dup.exit, 4);
        const auto kept = S.run(sb, "self.res", "", sb.args(), [](T::Probe& P) {
            P.note("reason", "\"kept on failure\"");
            P.eq("x", 1, 2);
        });
        S.yes("note.kept_on_spec_fail", kept.exit == 1 && has(kept.json, ",\"reason\":\"kept on failure\"}"));
    }

    // ---- candidates are written atomically (03 §3.2.4) --------------------------------------------------------------
    void candidates(Suite& S)
    {
        const Sandbox sb = S.fresh("atomic");
        const auto o = S.run(sb, "self.atomic", "", sb.args(), [](T::Probe& P) {
            P.num("a", 1, T::Tol::exact());
            const std::string v[] = { "one", "two" };
            P.lines("l", v);
        });
        S.eq("atomic.exit", o.exit, 3);
        int temps = 0, files = 0;
        for (const auto& e : fs::recursive_directory_iterator(sb.cand()))
        {
            if (!e.is_regular_file())
                continue;
            ++files;
            temps += has(e.path().filename().string(), ".tmp.") ? 1 : 0;
        }
        S.eq("atomic.no_temp_files", temps, 0);
        S.eq("atomic.files", files, 2);                         // self.atomic.txt + self.atomic.l.lines
        // A candidate directory that cannot be created is a harness error, and nothing partial is left behind.
        const Sandbox bad = S.fresh("atomic-unwritable");
        put(bad.root / "blocker", "a regular file where a directory is needed\n");
        std::vector<std::string> args = bad.args();
        args[7] = (bad.root / "blocker" / "cand").string();    // --bless-to <file>/cand
        const auto unwritable = S.run(bad, "self.atomic", "", args, [](T::Probe& P) {
            P.num("a", 1, T::Tol::exact());
        });
        S.eq("atomic.unwritable_is_error", unwritable.exit, 4);
    }

    // ---- --bless-to never reaches the golden tree (S0 review R-G1 #3) -----------------------------------------------
    void blessToOutsideRoot(Suite& S)
    {
        const auto body = [](T::Probe& P) { P.num("a", 1, T::Tol::exact()); };
        const auto runWith = [&](const Sandbox& sb, const fs::path& blessTo) {
            std::vector<std::string> args = sb.args();
            args[7] = blessTo.string();                          // args[6] is "--bless-to"
            return S.run(sb, "self.bless", "", args, body);
        };
        {
            // The review's scenario: the first run drifts and, with --bless-to <golden root>, would write the arm64
            // overlay; the second run would then pass.
            const Sandbox sb = S.fresh("bless-to-root");
            put(sb.base("self.bless"), "a\t2\texact\n");
            const auto first = runWith(sb, sb.golden());
            const auto second = runWith(sb, sb.golden());
            S.eq("bless_to.equal_root", first.exit, 4);
            S.yes("bless_to.equal_root.no_overlay", !fs::exists(sb.overlay("arm64", "self.bless")));
            S.eq("bless_to.equal_root.still_refused", second.exit, 4);
            S.yes("bless_to.message", has(first.out, "--bless-to must be outside --golden-root"));
            S.eq("bless_to.relative_spelling", runWith(sb, fs::relative(sb.golden())).exit, 4);
            S.eq("bless_to.dot_dot", runWith(sb, sb.golden() / "arm64" / "..").exit, 4);
            S.eq("bless_to.inside_root", runWith(sb, sb.golden() / "candidates").exit, 4);
            S.eq("bless_to.contains_root", runWith(sb, sb.root).exit, 4);
            std::error_code ec;
            fs::create_directory_symlink(sb.golden(), sb.root / "link", ec);
            S.yes("bless_to.symlink", !ec && runWith(sb, sb.root / "link").exit == 4);
            S.yes("bless_to.symlink_inside", !ec && runWith(sb, sb.root / "link" / "arm64").exit == 4);
            // A case-insensitive volume (the macOS default) spells the golden directory many ways.
            const bool caseInsensitive = fs::exists(sb.root / "GOLDEN", ec);
            S.yes("bless_to.case_variant", !caseInsensitive || runWith(sb, sb.root / "GOLDEN" / "x").exit == 4);
            S.yes("bless_to.golden_untouched", !fs::exists(sb.golden() / "arm64") && !fs::exists(sb.golden() / "x")
                                                   && !fs::exists(sb.golden() / "candidates"));
            // A sibling whose name merely starts with the root's name is outside it.
            const auto sibling = runWith(sb, sb.root / "golden-cand");
            S.yes("bless_to.sibling_prefix_ok",
                  sibling.exit == 2 && fs::exists(sb.root / "golden-cand" / "arm64" / "global" / "self.bless.txt"));
        }
    }

    // ---- a probe's candidate files always come from its latest run (R-G1 #4) ----------------------------------------
    void staleCandidates(Suite& S)
    {
        const Sandbox sb = S.fresh("stale");
        const fs::path dir = sb.cand() / "arm64" / "global";
        const auto at = [&](std::string_view name) { return fs::exists(dir / std::string(name)); };
        const auto full = [](T::Probe& P) {
            P.num("n", 1, T::Tol::exact());
            const std::string a[] = { "Button MODE" }, b[] = { "Tab 1" };
            P.lines("a11y", a);
            P.lines("tabs", b);
        };
        // A longer probe's candidate beside this one (golden.py's longest-owner rule): never touched.
        put(dir / "self.stale.more.txt", "x\t1\texact\n");
        put(dir / "self.stale.more.k.lines", "kept\n");
        const auto longerKept = [&] { return at("self.stale.more.txt") && at("self.stale.more.k.lines"); };

        const auto first = S.run(sb, "self.stale", "", sb.args(), full);
        S.yes("stale.first_run",
              first.exit == 3 && at("self.stale.txt") && at("self.stale.a11y.lines") && at("self.stale.tabs.lines"));
        const auto fewer = S.run(sb, "self.stale", "", sb.args(), [](T::Probe& P) {
            P.num("n", 1, T::Tol::exact());
            const std::string a[] = { "Button MODE" };
            P.lines("a11y", a);
        });
        S.yes("stale.dropped_sidecar_removed",
              fewer.exit == 3 && at("self.stale.a11y.lines") && !at("self.stale.tabs.lines"));
        S.yes("stale.longer_probe_kept", longerKept());
        const auto linesOnly = S.run(sb, "self.stale", "", sb.args(), [](T::Probe& P) {
            const std::string a[] = { "Button MODE" };
            P.lines("a11y", a);
        });
        S.yes("stale.txt_removed_without_rows",
              linesOnly.exit == 3 && !at("self.stale.txt") && at("self.stale.a11y.lines"));

        const auto noneLeft = [&] {
            return !at("self.stale.txt") && !at("self.stale.a11y.lines") && !at("self.stale.tabs.lines")
                && !at("self.stale.diff") && longerKept();
        };
        S.run(sb, "self.stale", "", sb.args(), full);
        const auto partial = S.run(sb, "self.stale", "", sb.args({ "--only", "n" }), full);
        S.yes("stale.partial_run_leaves_none", partial.exit == 3 && noneLeft());
        S.run(sb, "self.stale", "", sb.args(), full);
        const auto failed = S.run(sb, "self.stale", "", sb.args(), [&](T::Probe& P) {
            full(P);
            P.harnessError("forced by the self-test");
        });
        S.yes("stale.failed_run_leaves_none", failed.exit == 4 && noneLeft());
        S.run(sb, "self.stale", "", sb.args(), full);
        const auto specOnly = S.run(sb, "self.stale", "", sb.args(), [](T::Probe& P) { P.eq("x", 1, 1); });
        S.yes("stale.spec_only_run_leaves_none", specOnly.exit == 0 && noneLeft());

        // A probe that no longer produces golden rows drifts against its golden; the .diff is its candidate.
        put(sb.base("self.stale"), "n\t1\texact\n");
        const auto dropped = S.run(sb, "self.stale", "", sb.args(), [](T::Probe& P) { P.eq("x", 1, 1); });
        S.yes("stale.rows_dropped_diff", dropped.exit == 2 && at("self.stale.diff") && !at("self.stale.txt"));
    }

    // ---- num() compares what the golden file holds (R-G1 #6) --------------------------------------------------------
    void printedValues(Suite& S)
    {
        const Sandbox sb = S.fresh("printed");
        const auto body = [](T::Probe& P) {
            P.num("f.le", 0.1f, T::Tol::le(0));
            P.num("f.ge", 0.1f, T::Tol::ge(0));
            P.num("f.abs", 0.1f, T::Tol::abs(0));
            P.num("f.rel", 1.0 / 3.0, T::Tol::rel(0));
            P.num("f.absrel", 2.0 / 3.0, T::Tol::absrel(0, 0));
        };
        const auto first = S.run(sb, "self.printed", "", sb.args(), body);
        S.eq("printed.first_run_missing", first.exit, 3);
        // Bless the candidate exactly as written (what golden.py adopt does), then run the same probe again.
        std::error_code ec;
        fs::create_directories(sb.base("self.printed").parent_path(), ec);
        fs::copy_file(sb.cand() / "arm64" / "global" / "self.printed.txt", sb.base("self.printed"),
                      fs::copy_options::overwrite_existing, ec);
        const auto again = S.run(sb, "self.printed", "", sb.args(), body);
        S.yes("printed.blessed_candidate_passes", !ec && again.exit == 0);
    }

    // ---- non-finite values never become goldens (R-G1 #7) -----------------------------------------------------------
    void nonFinite(Suite& S)
    {
        const Sandbox sb = S.fresh("nonfinite");
        const auto exitOf = [&](auto body) { return S.run(sb, "self.nf", "", sb.args(), body).exit; };
        const double inf = std::numeric_limits<double>::infinity();
        S.eq("nonfinite.num_nan", exitOf([](T::Probe& P) { P.num("a", std::nan(""), T::Tol::exact()); }), 4);
        S.eq("nonfinite.num_inf", exitOf([&](T::Probe& P) { P.num("a", -inf, T::Tol::abs(1)); }), 4);
        S.eq("nonfinite.text_nan", exitOf([](T::Probe& P) { P.text("t", "nan"); }), 4);
        S.eq("nonfinite.text_infinity", exitOf([](T::Probe& P) { P.text("t", "-Infinity"); }), 4);
        S.eq("nonfinite.text_nan_payload", exitOf([](T::Probe& P) { P.text("t", "NaN(0x1)"); }), 4);
        S.eq("nonfinite.lookalikes_ok", exitOf([](T::Probe& P) {
            P.text("t", "nano");
            P.text("u", "info");
            P.hash("h", 0x1e40000000000000ull);                 // "1e40000000000000": a hash, not an overflow
        }), 3);
        const auto goldenWith = [&](std::string_view key, std::string_view content) {
            const Sandbox g = S.fresh(key);
            put(g.base("self.nf"), content);
            S.eq(key, S.run(g, "self.nf", "", g.args(), [](T::Probe& P) { P.text("t", "x"); }).exit, 4);
        };
        goldenWith("nonfinite.golden_nan_exact", "t\tx\texact\na\tnan\texact\n");
        goldenWith("nonfinite.golden_inf_abs", "t\tx\texact\na\tinf\tabs:1\n");
        goldenWith("nonfinite.golden_overflow_abs", "t\tx\texact\na\t1e999\tabs:1\n");
    }

    // ---- setEnv / unsetEnv ------------------------------------------------------------------------------------------
    void environment(Suite& S)
    {
        const char* name = "FUNKGUI_HARNESS_SELF_ENV";
        const bool set = T::setEnv(name, "v1");
        const char* v = std::getenv(name);
        S.yes("env.set", set && v != nullptr && std::strcmp(v, "v1") == 0);
        const bool unset = T::unsetEnv(name);
        S.yes("env.unset", unset && std::getenv(name) == nullptr);
    }
}

int main(int argc, char** argv)
{
    const T::ScopedFtz ftz;
    T::Probe self("fg.harness.self", "", argc, argv);
    Suite suite(self);
    tolerances(suite);
    hashes(suite);
    exitCodes(suite);
    goldenTolerances(suite);
    keys(suite);
    goldenFiles(suite);
    sidecars(suite);
    commandLine(suite);
    results(suite);
    candidates(suite);
    blessToOutsideRoot(suite);
    staleCandidates(suite);
    printedValues(suite);
    nonFinite(suite);
    environment(suite);
    return self.finish();
}

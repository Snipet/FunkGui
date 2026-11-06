// FUNKGUI_TEST name=fg.shader.check timeout=300 gpu=1
//
// fg.shader.check (0.25.0; gpu/ShaderLibrary.h "Checking it off the message thread"): shaders::check(), set()'s checks
// on any thread, and set() not compiling again what check() already compiled. No bgfx, no window: a Metal device only
// (the compile), against fs_custom_test as built at build time (see fg.shader.library), and variants of it whose
// Metal code carries one more comment line, so that each is a code no compile of this process (or of an earlier run,
// were Metal to keep any across processes) has seen. Spec rows only; the times are INFO lines.
//
// - check.*: this platform's test binary passes with why cleared and nothing registered (the registry's generation
//   does not move); each refusal of fg.shader.library's set.* rows is false with set()'s own reason, word for word.
// - record.* (macOS; skipped and noted without a Metal device): a new code costs one Metal compile in check(); set()
//   of it afterwards, set() again and check() again cost none; a one-line change costs one; a refused code is not
//   remembered (set() of it compiles again and refuses it with check()'s reason).
// - bound.* (macOS, likewise): the record's two bounds, exactly. Bytes: a code of 3 MiB alone stays (the newest is
//   always kept) and drops every older one; two codes of exactly 2 MiB together both stay, and one more small code
//   drops the older of them. Count: one code X alone in the record, then 255 new ones (256 in all), then X again
//   and one more new code: X stays (first in, first out would drop it: X was checked first), the second of the 255
//   stays (at least 256 kept) and the first goes (at most 256).
// - thread.*: four worker threads check four new codes and one shared code at once, all true; each new code is
//   compiled once and the shared one at most four times; set() of all five on this thread afterwards compiles none;
//   a worker's refusal carries Metal's reason. On Linux the same rows run without a compile count.

#include <funkgui/gpu/ShaderLibrary.h>
#include <funkgui/shaders/fs_custom_test.mtl.h>
#include <funkgui/shaders/fs_custom_test.spv.h>
#if defined(__APPLE__)
  #include <funkgui/shaders/vs_custom.mtl.h>
#else
  #include <funkgui/shaders/vs_custom.spv.h>
#endif
#include <funkgui/test/Harness.h>

#include "../../src/gpu/ShaderLibraryDetail.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace T = funkgui::test;
namespace S = funkgui::shaders;
namespace D = funkgui::shaders::detail;

namespace
{
    using Bytes = std::vector<uint8_t>;
    using Clock = std::chrono::steady_clock;

    Bytes bytes(const uint8_t* p, std::size_t n) { return Bytes(p, p + n); }

    uint32_t u32(const Bytes& b, std::size_t at)
    {
        return static_cast<uint32_t>(b[at]) | static_cast<uint32_t>(b[at + 1]) << 8
               | static_cast<uint32_t>(b[at + 2]) << 16 | static_cast<uint32_t>(b[at + 3]) << 24;
    }

    // Where a shaderc v11 binary's code starts (after magic, both hashes and the uniform table), and the type byte of
    // the uniform named `name` (0 when absent).
    std::size_t codeAt(const Bytes& b, const std::string& name, std::size_t* typeAt)
    {
        std::size_t at = 12;
        const unsigned count = static_cast<unsigned>(b[at]) | static_cast<unsigned>(b[at + 1]) << 8;
        at += 2;
        for (unsigned i = 0; i < count; ++i)
        {
            const std::size_t n = b[at++];
            if (typeAt != nullptr && std::string_view(reinterpret_cast<const char*>(&b[at]), n) == name)
                *typeAt = at + n;
            at += n + 2 + 2 + 2 + 2 + 2;             // name, type, num, reg index, reg count, tex info, tex format
        }
        return at + 4;                               // the code's size, then the code
    }

    [[maybe_unused]] std::string code(const Bytes& b)
    {
        const std::size_t at = codeAt(b, "", nullptr);
        return std::string(reinterpret_cast<const char*>(&b[at]), u32(b, at - 4));
    }

    // The binary with its code replaced (the size field rewritten; the NUL and what follows the code kept).
    [[maybe_unused]] Bytes withCode(const Bytes& b, const std::string& text)
    {
        const std::size_t at = codeAt(b, "", nullptr);
        const uint32_t was = u32(b, at - 4);
        Bytes out(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(at - 4));
        const auto n = static_cast<uint32_t>(text.size());
        for (int i = 0; i < 4; ++i)
            out.push_back(static_cast<uint8_t>(n >> (8 * i)));
        out.insert(out.end(), text.begin(), text.end());
        out.insert(out.end(), b.begin() + static_cast<std::ptrdiff_t>(at + was), b.end());
        return out;
    }

    // The binary whose code ends in one more comment line naming `tag` and this run. Off macOS the binary itself:
    // nothing compiles its code there, and SPIR-V words take no comment.
    std::string gRun;
    Bytes fresh(const Bytes& b, [[maybe_unused]] const std::string& tag)
    {
#if defined(__APPLE__)
        return withCode(b, code(b) + "\n// fg.shader.check " + gRun + " " + tag + "\n");
#else
        return b;
#endif
    }

    bool patch(Bytes& b, std::string_view from, std::string_view to, std::size_t start)
    {
        for (std::size_t i = start; i + from.size() <= b.size(); ++i)
            if (std::memcmp(&b[i], from.data(), from.size()) == 0)
            {
                std::memcpy(&b[i], to.data(), to.size());
                return true;
            }
        return false;
    }

    bool checks(const Bytes& b, std::string& why)
    {
        return S::check(b.empty() ? nullptr : b.data(), static_cast<uint32_t>(b.size()), why);
    }

    bool sets(uint32_t id, const Bytes& b, std::string& why)
    {
        return S::set(id, b.empty() ? nullptr : b.data(), static_cast<uint32_t>(b.size()), why);
    }

    // check() refuses b with set()'s reason (set() on an id never registered here, so it changes nothing either).
    void refusal(T::Probe& P, const std::string& key, const Bytes& b, std::string_view needle)
    {
        std::string checkWhy = "stale", setWhy;
        const bool ok = checks(b, checkWhy);
        const bool setOk = sets(77, b, setWhy);
        std::printf("INFO     %s: %s\n", key.c_str(), checkWhy.c_str());
        P.eq(key + ".false", ok || setOk, 0);
        P.eq(key + ".why", checkWhy.find(needle) != std::string::npos, 1);
        P.eq(key + ".set_same_why", checkWhy == setWhy, 1);
    }

    double msSince(Clock::time_point t0)
    {
        return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    }

    // How many Metal compiles `f` ran.
    template <typename F> int64_t compiles(F&& f)
    {
        const uint64_t before = D::metalCompileCount();
        f();
        return static_cast<int64_t>(D::metalCompileCount() - before);
    }
}

int main(int argc, char** argv)
{
    T::Probe P("fg.shader.check", "", argc, argv);
    gRun = std::to_string(Clock::now().time_since_epoch().count());

    const Bytes fsMtl = bytes(fs_custom_test_mtl, sizeof fs_custom_test_mtl);
    const Bytes fsSpv = bytes(fs_custom_test_spv, sizeof fs_custom_test_spv);
#if defined(__APPLE__)
    const Bytes& mine = fsMtl;
    const Bytes& other = fsSpv;
    const Bytes vsMine = bytes(vs_custom_mtl, sizeof vs_custom_mtl);
    const char* otherWhy = "SPIR-V";
#else
    const Bytes& mine = fsSpv;
    const Bytes& other = fsMtl;
    const Bytes vsMine = bytes(vs_custom_spv, sizeof vs_custom_spv);
    const char* otherWhy = "no SPIR-V";
#endif

    // ---- check ------------------------------------------------------------------------------------------------------
    std::string why = "stale";
    const uint64_t gen = D::generation();
    const bool first = checks(mine, why);
    std::printf("INFO     check(fs_custom_test): %s%s\n", first ? "true" : "false", first ? "" : (" " + why).c_str());
    const bool noMetal = !first && why.starts_with("no Metal device");
    P.note("metal", noMetal ? "\"absent\"" : "\"present\"");
    if (!noMetal)
    {
        P.eq("check.takes", first, 1);
        P.eq("check.clears_why", why.empty(), 1);
    }
    P.eq("check.registers_nothing", D::generation() == gen && !S::has(77), 1);

    refusal(P, "check.no_bytes", {}, "no shader binary");
    refusal(P, "check.not_a_shader", Bytes{ 'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd' },
            "not a bgfx shader binary");
    refusal(P, "check.cut_short", Bytes(mine.begin(), mine.begin() + 40), "cut short");
    refusal(P, "check.vertex", vsMine, "vertex shader");
    Bytes hashed = mine;
    hashed[4] ^= 0x5a;
    refusal(P, "check.other_varyings", hashed, "$input v_uv, v_px");
    std::size_t typeAt = 0;
    [[maybe_unused]] const std::size_t at = codeAt(mine, "u_params", &typeAt);
    if (P.eq("check.found_u_params", typeAt != 0, 1))
    {
        Bytes sampler = mine;
        sampler[typeAt] = static_cast<uint8_t>(sampler[typeAt] & 0xF0);
        refusal(P, "check.sampler", sampler, "samples a texture");
        Bytes resized = mine;
        resized[typeAt + 1] = 16;
        refusal(P, "check.resized_params", resized, "uniform vec4 u_params[4];");
        Bytes renamed = mine;
        P.eq("check.renamed_patch", patch(renamed, "u_params", "u_paramz", typeAt - 8), 1);
        refusal(P, "check.unknown_uniform", renamed, "never sets");
    }
    refusal(P, "check.other_renderer", other, otherWhy);
#if defined(__APPLE__)
    Bytes broken = fresh(mine, "broken");
    if (!noMetal)
    {
        P.eq("check.broken_patch", patch(broken, "return", "retxrn", at), 1);
        refusal(P, "check.metal_refuses", broken, "Metal did not compile the shader");
        Bytes entry = mine;
        bool all = false;
        while (patch(entry, "xlatMtlMain", "xlatMtlMaix", at))
            all = true;
        P.eq("check.entry_patch", all, 1);
        refusal(P, "check.no_entry_point", entry, "xlatMtlMain");
    }
#endif

    // ---- record -----------------------------------------------------------------------------------------------------
#if defined(__APPLE__)
    if (!noMetal)
    {
        const Bytes a = fresh(mine, "a");
        bool ok = false;
        auto t0 = Clock::now();
        P.eq("record.check_compiles", compiles([&] { ok = checks(a, why); }), 1);
        const double cold = msSince(t0);
        P.eq("record.check_takes", ok, 1);
        t0 = Clock::now();
        P.eq("record.set_after_check", compiles([&] { ok = sets(10, a, why); }), 0);
        const double recalled = msSince(t0);
        P.eq("record.set_registers", ok && S::has(10), 1);
        P.eq("record.set_again", compiles([&] { sets(10, a, why); }), 0);
        P.eq("record.check_again", compiles([&] { checks(a, why); }), 0);
        // What bgfx's own compile of the same text costs at the next frame (Metal's call again, by hand).
        std::string ignored;
        t0 = Clock::now();
        D::metalCompiles(code(a), ignored);
        const double again = msSince(t0);
        std::printf("INFO     time: check() of a new code %.2f ms, set() after it %.3f ms, Metal compiling the same "
                    "text again %.2f ms\n", cold, recalled, again);

        P.eq("record.changed_code", compiles([&] { checks(fresh(mine, "a2"), why); }), 1);

        std::string checkWhy, setWhy;
        P.eq("record.refusal_compiles", compiles([&] { ok = checks(broken, checkWhy); }), 1);
        P.eq("record.refusal_set_compiles", compiles([&] { ok = ok || sets(11, broken, setWhy); }), 1);
        P.eq("record.refusal_false", ok, 0);
        P.eq("record.refusal_same_why", !checkWhy.empty() && checkWhy == setWhy, 1);
        P.eq("record.refusal_unregistered", S::has(11), 0);

        S::remove(10);
    }
    else
        std::printf("INFO     no Metal device: record.* skipped\n");

    // ---- bound ------------------------------------------------------------------------------------------------------
    // The bounds as gpu/ShaderLibrary.h states them, written out here so that a change to either shows. What they
    // count is the Metal code's text: the test shader's code, then one comment line.
    if (!noMetal)
    {
        constexpr std::size_t kBytes = std::size_t{ 2 } << 20;
        constexpr int kCodes = 256;
        const std::string text = code(mine);
        // A code of exactly `total` bytes: the test shader's, its comment line padded with x's.
        auto sized = [&](std::size_t total, const std::string& tag) {
            const std::string head = text + "\n// fg.shader.check " + gRun + " " + tag + " ";
            return withCode(mine, head + std::string(total - head.size() - 1, 'x') + "\n");
        };
        bool ok = false;

        // Bytes. `huge` (3 MiB) is above the bound alone: it stays as the newest and drops every older code (`a`,
        // checked above, among them). `most` and s are exactly 2 MiB together: both stay. One more small code t drops
        // the older of the two (`most`, 2 MiB less one small code): the bound is 2 MiB within one small code.
        const Bytes a = fresh(mine, "a"), s = fresh(mine, "bytes-s"), t = fresh(mine, "bytes-t");
        const Bytes huge = sized(std::size_t{ 3 } << 20, "huge"), most = sized(kBytes - code(s).size(), "most");
        auto t0 = Clock::now();
        P.eq("bound.huge_compiles", compiles([&] { ok = checks(huge, why); }), 1);
        std::printf("INFO     time: check() of a 3 MiB code %.2f ms%s%s\n", msSince(t0), ok ? "" : ": ",
                    why.c_str());
        P.eq("bound.huge_takes", ok, 1);
        P.eq("bound.huge_kept", compiles([&] { checks(huge, why); }), 0);
        P.eq("bound.huge_drops_older", compiles([&] { checks(a, why); }), 1);
        int64_t n = compiles([&] { ok = checks(most, why); });
        n += compiles([&] { ok = checks(s, why) && ok; });
        P.eq("bound.bytes_fill", n == 2 && ok, 1);
        P.eq("bound.bytes_at_bound", compiles([&] { checks(most, why); checks(s, why); }), 0);
        P.eq("bound.bytes_next", compiles([&] { checks(t, why); }), 1);
        P.eq("bound.bytes_next_kept", compiles([&] { checks(s, why); checks(t, why); }), 0);
        P.eq("bound.bytes_over", compiles([&] { checks(most, why); }), 1);

        // Count. `huge` drops every other code, and x then drops `huge`: x alone. x and 255 new codes c0..c254 (256
        // codes, well within the bytes bound), x again, then c255: the code used least lately (c0) goes, not x.
        P.eq("bound.count_fits_bytes", static_cast<std::size_t>(kCodes) * (text.size() + 64) < kBytes, 1);
        const Bytes x = fresh(mine, "count-x");
        auto c = [&](int i) { return fresh(mine, "count" + std::to_string(i)); };
        checks(huge, why);
        P.eq("bound.count_x_new", compiles([&] { checks(x, why); }), 1);
        t0 = Clock::now();
        n = 0;
        for (int i = 0; i < kCodes - 1; ++i)
            n += compiles([&] { checks(c(i), why); });
        std::printf("INFO     time: check() of %d new codes %.1f ms\n", kCodes - 1, msSince(t0));
        P.eq("bound.count_fill", n, kCodes - 1);
        P.eq("bound.count_touch", compiles([&] { checks(x, why); }), 0);
        P.eq("bound.count_next", compiles([&] { checks(c(kCodes - 1), why); }), 1);
        P.eq("bound.count_used_kept", compiles([&] { ok = sets(12, x, why); }), 0);
        P.eq("bound.count_set", ok && S::has(12), 1);
        P.eq("bound.count_second_kept", compiles([&] { checks(c(1), why); }), 0);
        P.eq("bound.count_oldest_dropped", compiles([&] { checks(c(0), why); }), 1);
        S::remove(12);
    }
    else
        std::printf("INFO     no Metal device: bound.* skipped\n");
#endif

    // ---- thread -----------------------------------------------------------------------------------------------------
    if (!noMetal)
    {
        constexpr int kWorkers = 4;
        std::array<Bytes, kWorkers> own;
        for (int i = 0; i < kWorkers; ++i)
            own[static_cast<std::size_t>(i)] = fresh(mine, "thread" + std::to_string(i));
        const Bytes shared = fresh(mine, "shared");
        std::array<int, kWorkers> took{};
        std::array<std::string, kWorkers> whys;
        std::atomic<int> ready{ 0 };
        const uint64_t before = D::metalCompileCount();
        const auto t0 = Clock::now();
        std::vector<std::thread> workers;
        for (int i = 0; i < kWorkers; ++i)
            workers.emplace_back([&, i] {
                const auto k = static_cast<std::size_t>(i);
                ready.fetch_add(1);
                while (ready.load() < kWorkers) {}       // all start together
                std::string w;
                took[k] = (checks(own[k], w) ? 1 : 0) + (checks(shared, whys[k]) ? 1 : 0);
                if (!w.empty())
                    whys[k] = w;
            });
        for (std::thread& w : workers)
            w.join();
        const int64_t n = static_cast<int64_t>(D::metalCompileCount() - before);
        std::printf("INFO     time: %d workers checked %d new codes in %.2f ms, %lld Metal compiles\n", kWorkers,
                    kWorkers + 1, msSince(t0), static_cast<long long>(n));
        bool all = true;
        for (int i = 0; i < kWorkers; ++i)
        {
            all = all && took[static_cast<std::size_t>(i)] == 2;
            if (took[static_cast<std::size_t>(i)] != 2)
                std::printf("INFO     worker %d: %s\n", i, whys[static_cast<std::size_t>(i)].c_str());
        }
        P.eq("thread.all_take", all, 1);
#if defined(__APPLE__)
        P.eq("thread.compiles_at_least", n >= kWorkers + 1, 1);
        P.eq("thread.compiles_at_most", n <= 2 * kWorkers, 1);
#endif
        int64_t later = 0;
        bool registered = true;
        for (int i = 0; i < kWorkers; ++i)
            later += compiles([&] {
                registered = sets(static_cast<uint32_t>(20 + i), own[static_cast<std::size_t>(i)], why) && registered;
            });
        later += compiles([&] { registered = sets(30, shared, why) && registered; });
        P.eq("thread.set_registers", registered, 1);
        P.eq("thread.set_compiles_none", later, 0);
        for (uint32_t id = 20; id <= 30; ++id)
            S::remove(id);

        std::string workerWhy;
        bool workerOk = true;
        std::thread([&] { workerOk = checks(other, workerWhy); }).join();
        P.eq("thread.refusal_false", workerOk, 0);
        P.eq("thread.refusal_why", workerWhy.find(otherWhy) != std::string::npos, 1);
#if defined(__APPLE__)
        std::thread([&] { workerOk = checks(broken, workerWhy); }).join();
        P.eq("thread.metal_refusal_why", !workerOk && workerWhy.find("Metal did not compile the shader") == 0, 1);
#endif
    }
    return P.finish();
}

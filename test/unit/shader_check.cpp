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
//   remembered (set() of it compiles again and refuses it with check()'s reason); the record keeps the 32 codes used
//   last (a code checked again stays, the oldest goes) and at most 2 MiB of text, always keeping the newest.
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

    // ---- check ---------------------------------------------------------------------------------------------------------
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

    // ---- record ----------------------------------------------------------------------------------------------------------
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

        // B, A, 16 new codes, A again, 15 more: 33 codes, so the oldest (B) goes and A, used since, stays.
        const Bytes b = fresh(mine, "b"), lru = fresh(mine, "lru");
        int64_t n = compiles([&] { checks(b, why); checks(lru, why); });
        for (int i = 0; i < 16; ++i)
            n += compiles([&] { checks(fresh(mine, "f" + std::to_string(i)), why); });
        P.eq("record.lru_touch", compiles([&] { checks(lru, why); }), 0);
        for (int i = 16; i < 31; ++i)
            n += compiles([&] { checks(fresh(mine, "f" + std::to_string(i)), why); });
        P.eq("record.lru_fill", n, 33);
        P.eq("record.lru_kept", compiles([&] { sets(12, lru, why); }), 0);
        P.eq("record.oldest_dropped", compiles([&] { checks(b, why); }), 1);

        // A code of 3 MiB (its comment): above the bytes bound alone, kept as the newest, and every other one goes.
        const std::string text = code(mine);
        std::string big = text + "\n//";
        big.append(std::size_t{ 3 } << 20, 'x');
        big += "\n// fg.shader.check " + gRun + " big\n";
        const Bytes huge = withCode(mine, big);
        t0 = Clock::now();
        P.eq("record.big_compiles", compiles([&] { ok = checks(huge, why); }), 1);
        std::printf("INFO     time: check() of a 3 MiB code %.2f ms%s%s\n", msSince(t0), ok ? "" : ": ",
                    why.c_str());
        P.eq("record.big_takes", ok, 1);
        P.eq("record.big_kept", compiles([&] { checks(huge, why); }), 0);
        P.eq("record.big_evicts", compiles([&] { checks(lru, why); }), 1);
        P.eq("record.small_after_big", compiles([&] { checks(lru, why); }), 0);
        S::remove(10);
        S::remove(12);
    }
    else
        std::printf("INFO     no Metal device: record.* skipped\n");
#endif

    // ---- thread ----------------------------------------------------------------------------------------------------------
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

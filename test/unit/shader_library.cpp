// FUNKGUI_TEST name=fg.shader.library timeout=300 gpu=1
//
// fg.shader.library (v0.19.0, gpu/ShaderLibrary.h): the custom-shader registry without a running bgfx (no window, no
// GPU context: what verify can run anywhere), against binaries FunkGuiShaders compiled at build time with the shaderc
// FunkGui uses: vs_custom (FunkGui's vertex half) and fs_custom_test, which is shaders/fs_custom_prelude.sc followed by
// test/shaders/fs_custom_test.main.sc, compiled with shaders/varying_custom.def.sc, exactly as a host compiles its own
// at run time. Spec rows only (vs_custom's hashes are fg.shader.hash's golden rows).
//
// - bin.*: both profiles' binaries have their stage's magic, and the fragment shader's input hash is the vertex
//   shader's output hash (what bgfx's createProgram compares), so the prelude's `$input v_uv, v_px` links.
// - text.*: varyingDef() and fragmentPrelude() are their files byte for byte; the prelude starts with the $input line
//   and declares the three uniforms; bgfxShaderSh() is bgfx's header.
// - target.*: empty strings while bgfx is not running.
// - set.*: this platform's test binary is registered (on macOS after Metal compiled its code), replaced, and removed;
//   each refusal (no bytes, not a shader, cut short, a vertex shader, other varyings, a sampler, an unknown uniform, a
//   resized u_params, the other renderer's code, and on macOS code Metal refuses or without bgfx's entry point) is
//   false with a reason and leaves the id as it was. Without a Metal device (a VM) the rows that need one are skipped
//   and noted.

#include <funkgui/gpu/ShaderLibrary.h>
#include <funkgui/shaders/fs_custom_test.mtl.h>
#include <funkgui/shaders/fs_custom_test.spv.h>
#include <funkgui/shaders/vs_custom.mtl.h>
#include <funkgui/shaders/vs_custom.spv.h>
#include <funkgui/test/Harness.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace T = funkgui::test;
namespace S = funkgui::shaders;

namespace
{
    using Bytes = std::vector<uint8_t>;

    Bytes bytes(const uint8_t* p, std::size_t n) { return Bytes(p, p + n); }

    uint32_t u32(const Bytes& b, std::size_t at)
    {
        return at + 4 <= b.size() ? static_cast<uint32_t>(b[at]) | static_cast<uint32_t>(b[at + 1]) << 8
                                        | static_cast<uint32_t>(b[at + 2]) << 16 | static_cast<uint32_t>(b[at + 3]) << 24
                                  : 0u;
    }

    bool magic(const Bytes& b, char stage) { return b.size() > 4 && b[0] == stage && b[1] == 'S' && b[2] == 'H'; }

    // A binary's uniform table (shaderc v11: name size, name, type, num, reg index, reg count, tex info, tex format)
    // and where the code starts.
    struct Uniform
    {
        std::string name;
        std::size_t typeAt = 0;
        uint8_t     type = 0, num = 0;
    };
    std::vector<Uniform> uniforms(const Bytes& b, std::size_t& codeAt)
    {
        std::vector<Uniform> out;
        std::size_t at = 12;                         // magic, hash in, hash out
        if (b.size() < at + 2)
            return out;
        const unsigned count = static_cast<unsigned>(b[at]) | static_cast<unsigned>(b[at + 1]) << 8;
        at += 2;
        for (unsigned i = 0; i < count && at < b.size(); ++i)
        {
            Uniform u;
            const std::size_t n = b[at++];
            u.name.assign(reinterpret_cast<const char*>(&b[at]), n);
            at += n;
            u.typeAt = at;
            u.type = b[at];
            u.num = b[at + 1];
            at += 2 + 2 + 2 + 2 + 2;                 // type, num, reg index, reg count, tex info, tex format
            out.push_back(u);
        }
        codeAt = at + 4;                             // the code's size, then the code
        return out;
    }

    std::string file(const char* rel)
    {
        const std::filesystem::path root = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
        std::ifstream in(root / rel, std::ios::binary);
        std::ostringstream s;
        s << in.rdbuf();
        return s.str();
    }

    // Replaces the first `from` in the binary's bytes at or after `start` with `to` (same length); false if absent.
    bool patch(Bytes& b, std::string_view from, std::string_view to, std::size_t start = 0)
    {
        if (from.size() != to.size() || b.size() < from.size())
            return false;
        for (std::size_t i = start; i + from.size() <= b.size(); ++i)
            if (std::memcmp(&b[i], from.data(), from.size()) == 0)
            {
                std::memcpy(&b[i], to.data(), to.size());
                return true;
            }
        return false;
    }

    bool refused(T::Probe& P, const std::string& key, uint32_t id, const Bytes& b, std::string_view needle)
    {
        std::string why;
        const bool had = S::has(id);
        const bool ok = S::set(id, b.empty() ? nullptr : b.data(), static_cast<uint32_t>(b.size()), why);
        std::printf("INFO     %s: %s\n", key.c_str(), why.c_str());
        P.eq(key + ".false", ok, 0);
        P.eq(key + ".why", why.find(needle) != std::string::npos, 1);
        return P.eq(key + ".id_unchanged", S::has(id), had ? 1 : 0);
    }
}

int main(int argc, char** argv)
{
    T::Probe P("fg.shader.library", "", argc, argv);

    // ---- bin -----------------------------------------------------------------------------------------------------------
    const Bytes vsMtl = bytes(vs_custom_mtl, sizeof vs_custom_mtl), vsSpv = bytes(vs_custom_spv, sizeof vs_custom_spv);
    const Bytes fsMtl = bytes(fs_custom_test_mtl, sizeof fs_custom_test_mtl);
    const Bytes fsSpv = bytes(fs_custom_test_spv, sizeof fs_custom_test_spv);
    P.eq("bin.vs_magic", magic(vsMtl, 'V') && magic(vsSpv, 'V'), 1);
    P.eq("bin.fs_magic", magic(fsMtl, 'F') && magic(fsSpv, 'F'), 1);
    P.eq("bin.version_11", vsMtl[3] == 11 && fsMtl[3] == 11 && vsSpv[3] == 11 && fsSpv[3] == 11, 1);
    P.eq("bin.mtl_links", u32(vsMtl, 8) == u32(fsMtl, 4), 1);                // vs hash out == fs hash in
    P.eq("bin.spv_links", u32(vsSpv, 8) == u32(fsSpv, 4), 1);
    std::size_t codeAt = 0;
    for (const Uniform& u : uniforms(fsMtl, codeAt))
        std::printf("INFO     fs_custom_test.mtl uniform %s type 0x%02x num %u\n", u.name.c_str(), u.type, u.num);

    // ---- text ----------------------------------------------------------------------------------------------------------
    const std::string prelude = S::fragmentPrelude();
    P.eq("text.varying_def_is_file", S::varyingDef() == file("shaders/varying_custom.def.sc"), 1);
    P.eq("text.prelude_is_file", prelude == file("shaders/fs_custom_prelude.sc"), 1);
    P.eq("text.prelude_input_first", std::string_view(prelude).starts_with("$input v_uv, v_px\n"), 1);
    P.eq("text.prelude_declares", prelude.find("#include <bgfx_shader.sh>") != std::string::npos
                                      && prelude.find("uniform vec4 u_viewSize;") != std::string::npos
                                      && prelude.find("uniform vec4 u_rect;") != std::string::npos
                                      && prelude.find("uniform vec4 u_params[4];") != std::string::npos, 1);
    P.eq("text.varyings", std::string_view(S::varyingDef()).find("vec2 v_uv        : TEXCOORD0;") != std::string::npos
                              && std::string_view(S::varyingDef()).find("vec2 v_px        : TEXCOORD1;")
                                     != std::string::npos, 1);
    const std::string_view sh = S::bgfxShaderSh();
    P.eq("text.bgfx_shader_sh", sh.size() > 10000 && sh.find("BGFX_SHADER_H_HEADER_GUARD") != std::string_view::npos,
         1);

    // ---- target --------------------------------------------------------------------------------------------------------
    const S::Target t = S::target();
    P.eq("target.empty_without_bgfx", t.platform.empty() && t.profile.empty(), 1);

    // ---- set -----------------------------------------------------------------------------------------------------------
#if defined(__APPLE__)
    const Bytes& mine = fsMtl;
    const Bytes& other = fsSpv;
    const Bytes& vsMine = vsMtl;
    const char* otherWhy = "SPIR-V";
#else
    const Bytes& mine = fsSpv;
    const Bytes& other = fsMtl;
    const Bytes& vsMine = vsSpv;
    const char* otherWhy = "no SPIR-V";
#endif
    std::string why = "stale";
    const bool first = S::set(1, mine.data(), static_cast<uint32_t>(mine.size()), why);
    std::printf("INFO     set(1, fs_custom_test): %s%s\n", first ? "true" : "false", first ? "" : (" " + why).c_str());
    const bool noMetal = !first && why.starts_with("no Metal device");
    P.note("metal", noMetal ? "\"absent\"" : "\"present\"");
    if (!noMetal)
    {
        P.eq("set.registers", first, 1);
        P.eq("set.clears_why", why.empty(), 1);
        P.eq("set.has", S::has(1), 1);
    }
    P.eq("set.unknown_has_not", S::has(2), 0);

    refused(P, "set.no_bytes", 2, {}, "no shader binary");
    refused(P, "set.not_a_shader", 2, Bytes{ 'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd' },
            "not a bgfx shader binary");
    refused(P, "set.cut_short", 2, Bytes(mine.begin(), mine.begin() + 40), "cut short");
    refused(P, "set.vertex", 2, vsMine, "vertex shader");
    Bytes hashed = mine;
    hashed[4] ^= 0x5a;                                          // another $input set
    refused(P, "set.other_varyings", 2, hashed, "$input v_uv, v_px");
    std::size_t at = 0;
    const std::vector<Uniform> us = uniforms(mine, at);
    const Uniform* params = nullptr;
    for (const Uniform& u : us)
        if (u.name == "u_params")
            params = &u;
    // shaderc sizes an array uniform to the highest element the code reads: fs_custom_test reads u_params[0] and [1].
    P.eq("set.test_reads_u_params", params != nullptr && params->num == 2, 1);
    if (params != nullptr)
    {
        Bytes sampler = mine;
        sampler[params->typeAt] = static_cast<uint8_t>((sampler[params->typeAt] & 0xF0) | 0);   // Sampler
        refused(P, "set.sampler", 2, sampler, "samples a texture");
        Bytes resized = mine;
        resized[params->typeAt + 1] = 16;                        // u_params[16]
        refused(P, "set.resized_params", 2, resized, "uniform vec4 u_params[4];");
        Bytes renamed = mine;
        const bool done = patch(renamed, "u_params", "u_paramz", params->typeAt - params->name.size());
        P.eq("set.renamed_patch", done, 1);
        refused(P, "set.unknown_uniform", 2, renamed, "never sets");
    }
    refused(P, "set.other_renderer", 2, other, otherWhy);
#if defined(__APPLE__)
    if (!noMetal)
    {
        Bytes broken = mine;
        P.eq("set.broken_patch", patch(broken, "return", "retxrn", at), 1);
        refused(P, "set.metal_refuses", 2, broken, "Metal did not compile the shader");
        Bytes renamed = mine;
        bool all = false;
        while (patch(renamed, "xlatMtlMain", "xlatMtlMaix", at))
            all = true;
        P.eq("set.entry_patch", all, 1);
        refused(P, "set.no_entry_point", 2, renamed, "xlatMtlMain");
    }
#endif
    if (!noMetal)
    {
        refused(P, "set.replace_refused", 1, Bytes{ 'x', 'y', 'z', 'w', 'x', 'y' }, "not a bgfx shader binary");
        P.eq("set.replace", S::set(1, mine.data(), static_cast<uint32_t>(mine.size()), why), 1);
        P.eq("set.second_id", S::set(3, mine.data(), static_cast<uint32_t>(mine.size()), why) && S::has(3), 1);
        S::remove(3);
        P.eq("set.removed", S::has(3), 0);
        P.eq("set.other_kept", S::has(1), 1);
        S::remove(99);                                          // unknown: ignored
        S::remove(1);
        P.eq("set.removed_last", S::has(1), 0);
    }
    return P.finish();
}

// FUNKGUI_TEST name=fg.shader.sampler timeout=300 gpu=1
//
// fg.shader.sampler (0.25.0; gpu/ShaderLibrary.h "Textures"): textures for custom shaders without bgfx, on every host
// (fg.shader.texture draws them, on macOS). The texture registry's rules, each compiler's sampler rules through
// detail::samplers (every rule is compiled on every host), and check() and set() of fs_texture_test (see
// fg.shader.texture: two samplers, s_tex at stage 0 and s_mask at stage 2) for this platform's renderer. Without a
// Metal device (a VM) the rows that need one are skipped and noted. Spec rows only.
//
// - api.*      setTexture() refuses a size outside 1..kMaxTextureSide, no texels or a short stride, changing nothing;
//              takes RGBA8 and R8 (a stride wider than a row too); removeTexture() and unbindTexture() of unknown
//              ones change nothing; bindTexture() before set() is kept and says false; the generation moves with
//              every change and only then.
// - samplers.* the Metal and SPIR-V binaries give s_tex at 0 and s_mask at 2; the SPIR-V table read as Direct3D 11's
//              (stage = register) after its registers are rewritten; each refusal with its reason: another kind of
//              texture, a stage of 4, two samplers at one stage, a sampler named after a uniform FunkGui sets (u_rect,
//              and on Metal and SPIR-V the instanced UI program's u_unorm8), a buffer or image that is no sampler's
//              part; on Metal an argument buffer, a plain uniform shaderc gives
//              no type (a float: "never sets") and an int uniform (typed Sampler, num 1); on SPIR-V a shadow sampler,
//              an integer one, a stage below the shift, a binary of version 10, an int uniform (no sampler bit), and
//              by the code's own image type a multisampled texture, a 3D one the table calls 2D, an image of a fixed
//              format (IMAGE2D_RO) and a binding with no image; on Direct3D 11 an int uniform (no sampler bit).
// - check.*    check() takes fs_texture_test; a texture of another kind is refused before Metal compiles it; u_params
//              declared as a sampler is "declared otherwise"; an int uniform, and on macOS a float one, "never sets";
//              a sampler named u_unorm8 is refused, with nothing compiled;
//              the other renderer's fs_texture_test is said to be the other renderer's, not a sampler's problem.
// - set.*      set() registers it; bindTexture() says whether the shader has the sampler; set() of the same id keeps
//              its bindings and remove(id) drops them (seen through unbindTexture() moving the generation or not).

#include <funkgui/gpu/ShaderLibrary.h>
#include <funkgui/shaders/fs_custom_test.mtl.h>
#include <funkgui/shaders/fs_custom_test.spv.h>
#include <funkgui/shaders/fs_texture_test.mtl.h>
#include <funkgui/shaders/fs_texture_test.spv.h>
#include <funkgui/test/Harness.h>

#include "../../src/gpu/ShaderLibraryDetail.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace T = funkgui::test;
namespace S = funkgui::shaders;
namespace D = funkgui::shaders::detail;

namespace
{
    using Bytes = std::vector<uint8_t>;

    constexpr uint32_t kA = 31, kPlain = 32;                     // shader ids
    constexpr uint32_t kGrid = 131, kRamp = 132, kTile = 133, kMask = 134, kNever = 199;   // texture ids

    Bytes rgba(int w, int h, int stride = 0)
    {
        const int row = stride == 0 ? w * 4 : stride;
        Bytes b(static_cast<std::size_t>(row) * static_cast<std::size_t>(h), 0xEE);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w * 4; ++x)
                b[static_cast<std::size_t>(y * row + x)] = static_cast<uint8_t>(16 * y + x);
        return b;
    }
    constexpr uint8_t kMaskTexels[4] = { 0, 85, 170, 255 };

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

    uint32_t u32(const Bytes& b, std::size_t at)
    {
        return static_cast<uint32_t>(b[at]) | static_cast<uint32_t>(b[at + 1]) << 8
               | static_cast<uint32_t>(b[at + 2]) << 16 | static_cast<uint32_t>(b[at + 3]) << 24;
    }

    void put16(Bytes& b, std::size_t at, uint16_t v)
    {
        b[at] = static_cast<uint8_t>(v & 0xFF);
        b[at + 1] = static_cast<uint8_t>(v >> 8);
    }

    void put32(Bytes& b, std::size_t at, uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            b[at + static_cast<std::size_t>(i)] = static_cast<uint8_t>(v >> (8 * i));
    }

    // A shaderc v11 binary's table (name size, name, type, num, reg index, reg count, tex info, tex format): where the
    // type byte of the uniform `name` is (0 when absent; the num is at +1, the reg index at +2, the tex info at +6),
    // and where the code starts.
    std::size_t typeAt(const Bytes& b, std::string_view name, std::size_t* codeAt = nullptr)
    {
        std::size_t at = 12, found = 0;
        const unsigned count = static_cast<unsigned>(b[at]) | static_cast<unsigned>(b[at + 1]) << 8;
        at += 2;
        for (unsigned i = 0; i < count && at < b.size(); ++i)
        {
            const std::size_t n = b[at++];
            if (found == 0 && std::string_view(reinterpret_cast<const char*>(&b[at]), n) == name)
                found = at + n;
            at += n + 10;
        }
        if (codeAt != nullptr)
            *codeAt = at + 4;                        // the code's size, then the code
        return found;
    }

    // The table's uniform `from` renamed `to`, of any length (the table is read entry by entry; the code is left as it
    // is). False when the table has no `from`.
    bool renameUniform(Bytes& b, std::string_view from, std::string_view to)
    {
        const std::size_t end = typeAt(b, from);
        if (end == 0 || to.empty() || to.size() > 255)
            return false;
        const std::size_t name = end - from.size();
        b[name - 1] = static_cast<uint8_t>(to.size());
        b.erase(b.begin() + static_cast<std::ptrdiff_t>(name), b.begin() + static_cast<std::ptrdiff_t>(end));
        b.insert(b.begin() + static_cast<std::ptrdiff_t>(name), to.begin(), to.end());
        return true;
    }

    // Where the first OpTypeImage (opcode 25) of a SPIR-V binary's code is, in bytes; 0 when absent. Its words: the
    // opcode and count, the result id, the sampled type, Dim, Depth, Arrayed, MS, Sampled, the image format.
    std::size_t imageAt(const Bytes& b)
    {
        std::size_t code = 0;
        typeAt(b, "", &code);
        const std::size_t end = code + u32(b, code - 4);
        for (std::size_t at = code + 20; at + 4 <= end;)
        {
            const uint32_t w = u32(b, at), count = w >> 16;
            if ((w & 0xFFFF) == 25)
                return at;
            if (count == 0)
                return 0;
            at += 4 * static_cast<std::size_t>(count);
        }
        return 0;
    }

    std::string list(const std::vector<D::Sampler>& s)
    {
        std::string out;
        for (const D::Sampler& x : s)
            out += (out.empty() ? "" : " ") + x.name + "@" + std::to_string(x.stage);
        return out;
    }

    bool samplersAre(T::Probe& P, const std::string& key, const Bytes& b, D::Code code, std::string_view want)
    {
        std::vector<D::Sampler> got;
        std::string why;
        const bool ok = D::samplers(b.data(), static_cast<uint32_t>(b.size()), code, got, why);
        std::printf("INFO     %s: %s%s\n", key.c_str(), list(got).c_str(), ok ? "" : (" refused: " + why).c_str());
        return P.eq(key, ok && list(got) == want, 1);
    }

    bool samplersRefuse(T::Probe& P, const std::string& key, const Bytes& b, D::Code code, std::string_view needle)
    {
        std::vector<D::Sampler> got;
        std::string why;
        const bool ok = D::samplers(b.data(), static_cast<uint32_t>(b.size()), code, got, why);
        std::printf("INFO     %s: %s\n", key.c_str(), why.c_str());
        return P.eq(key, !ok && why.find(needle) != std::string::npos, 1);
    }

    bool checkRefuses(T::Probe& P, const std::string& key, const Bytes& b, std::string_view needle)
    {
        std::string why;
        const bool ok = S::check(b.data(), static_cast<uint32_t>(b.size()), why);
        std::printf("INFO     %s: %s\n", key.c_str(), why.c_str());
        return P.eq(key, !ok && why.find(needle) != std::string::npos, 1);
    }

    bool moved(uint64_t& gen)
    {
        const uint64_t now = D::generation();
        const bool m = now != gen;
        gen = now;
        return m;
    }
}

int main(int argc, char** argv)
{
    T::Probe P("fg.shader.sampler", "", argc, argv);
    const Bytes mtl(fs_texture_test_mtl, fs_texture_test_mtl + sizeof fs_texture_test_mtl);
    const Bytes spv(fs_texture_test_spv, fs_texture_test_spv + sizeof fs_texture_test_spv);

    // ---- api: the texture registry --------------------------------------------------------------------------------
    uint64_t gen = D::generation();
    const Bytes grid = rgba(4, 4);
    P.eq("api.zero_width", S::setTexture(kGrid, 0, 4, S::TextureFormat::rgba8, grid.data()), 0);
    P.eq("api.too_tall", S::setTexture(kGrid, 4, S::kMaxTextureSide + 1, S::TextureFormat::rgba8, grid.data()), 0);
    P.eq("api.no_texels", S::setTexture(kGrid, 4, 4, S::TextureFormat::rgba8, nullptr), 0);
    P.eq("api.short_stride", S::setTexture(kGrid, 4, 4, S::TextureFormat::rgba8, grid.data(), 15), 0);
    P.eq("api.r8_short_stride", S::setTexture(kMask, 4, 1, S::TextureFormat::r8, kMaskTexels, 3), 0);
    P.eq("api.refusals_change_nothing", !S::hasTexture(kGrid) && !S::hasTexture(kMask) && !moved(gen), 1);
    P.eq("api.rgba8", S::setTexture(kGrid, 4, 4, S::TextureFormat::rgba8, grid.data(), 0, { true, false })
                          && S::hasTexture(kGrid) && moved(gen), 1);
    P.eq("api.r8", S::setTexture(kMask, 4, 1, S::TextureFormat::r8, kMaskTexels, 4, { true, false })
                       && S::hasTexture(kMask) && moved(gen), 1);
    const Bytes tile = rgba(2, 2, 12);                           // a stride wider than a row (8 bytes)
    P.eq("api.wide_stride", S::setTexture(kTile, 2, 2, S::TextureFormat::rgba8, tile.data(), 12, { true, true }), 1);
    P.eq("api.linear", S::setTexture(kRamp, 2, 1, S::TextureFormat::rgba8, rgba(2, 1).data()), 1);
    moved(gen);
    S::removeTexture(kNever);
    S::unbindTexture(kA, "s_tex");
    P.eq("api.unknown_removals_change_nothing", moved(gen), 0);
    P.eq("api.bind_before_set_false", S::bindTexture(kA, "s_tex", kGrid), 0);
    P.eq("api.bind_moves", moved(gen), 1);
    P.eq("api.same_bind_stays", !S::bindTexture(kA, "s_tex", kGrid) && !moved(gen), 1);
    P.eq("api.rebind_moves", !S::bindTexture(kA, "s_tex", kTile) && moved(gen), 1);
    S::removeTexture(kRamp);
    P.eq("api.remove_moves", !S::hasTexture(kRamp) && moved(gen), 1);
    S::unbindTexture(kA, "s_tex");
    P.eq("api.unbind_moves", moved(gen), 1);

    // ---- samplers: each compiler's rules ----------------------------------------------------------------------------
    samplersAre(P, "samplers.metal", mtl, D::Code::metal, "s_tex@0 s_mask@2");
    samplersAre(P, "samplers.spirv", spv, D::Code::spirv, "s_tex@0 s_mask@2");
    const std::size_t texAt = typeAt(spv, "s_tex"), maskAt = typeAt(spv, "s_mask");
    const std::size_t spvParams = typeAt(spv, "u_params"), imgAt = imageAt(spv);
    if (P.eq("samplers.found_spirv_table", texAt != 0 && maskAt != 0 && spvParams != 0 && imgAt != 0, 1))
    {
        Bytes dx = spv;                                          // Direct3D 11's table: the register is the stage
        put16(dx, texAt + 2, 0);
        put16(dx, maskAt + 2, 2);
        samplersAre(P, "samplers.dxbc", dx, D::Code::dxbc, "s_tex@0 s_mask@2");
        Bytes b = dx;
        b[texAt] = static_cast<uint8_t>(b[texAt] & ~0x20);       // no sampler bit: Direct3D's scalar int uniform
        samplersRefuse(P, "samplers.dxbc_int_uniform", b, D::Code::dxbc, "reads the uniform s_tex, which FunkGui");
        put16(dx, maskAt + 2, 4);
        samplersRefuse(P, "samplers.dxbc_stage_4", dx, D::Code::dxbc, "at stage 4");

        b = spv;
        put16(b, texAt + 6, 0x0600);                             // a 3D texture
        samplersRefuse(P, "samplers.spirv_3d", b, D::Code::spirv, "not a 2D sampler of floats");
        b = spv;
        put16(b, texAt + 6, 0x0201);                             // integer texels
        samplersRefuse(P, "samplers.spirv_int", b, D::Code::spirv, "not a 2D sampler of floats");
        b = spv;
        b[texAt] = static_cast<uint8_t>(b[texAt] | 0x80);        // a comparison (shadow) sampler
        samplersRefuse(P, "samplers.spirv_shadow", b, D::Code::spirv, "not a 2D sampler of floats");
        b = spv;
        b[texAt] = static_cast<uint8_t>(b[texAt] & ~0x20);       // no sampler bit: an int uniform
        samplersRefuse(P, "samplers.spirv_int_uniform", b, D::Code::spirv, "reads the uniform s_tex, which FunkGui");
        b = spv;
        put16(b, maskAt + 2, 6);                                 // stage 4
        samplersRefuse(P, "samplers.spirv_stage_4", b, D::Code::spirv, "at stage 4");
        b = spv;
        put16(b, maskAt + 2, 2);                                 // stage 0, s_tex's
        samplersRefuse(P, "samplers.spirv_shared_stage", b, D::Code::spirv, "both at stage 0");
        b = spv;
        put16(b, texAt + 2, 1);                                  // below the shift
        samplersRefuse(P, "samplers.spirv_below_shift", b, D::Code::spirv, "at stage -1");
        b = spv;
        put16(b, maskAt + 2, 5);                                 // stage 3: no image in the code there
        samplersRefuse(P, "samplers.spirv_no_image", b, D::Code::spirv, "corrupted");
        b = spv;
        b[3] = 10;
        samplersRefuse(P, "samplers.spirv_version_10", b, D::Code::spirv, "version 10");
        b = spv;
        P.eq("samplers.spirv_unorm8_patch", renameUniform(b, "s_mask", "u_unorm8"), 1);
        samplersRefuse(P, "samplers.spirv_funkgui_name_unorm8", b, D::Code::spirv,
                       "has the name of a uniform FunkGui");
        b = spv;
        b[spvParams] = static_cast<uint8_t>((b[spvParams] & 0xF0) | 1);   // End: a storage image or buffer
        samplersRefuse(P, "samplers.spirv_buffer", b, D::Code::spirv, "never binds");
        // The code's image type (the two samplers share it): what the tex info (0x0200, 2D floats) cannot say.
        b = spv;
        put32(b, imgAt + 4 * 6, 1);                              // MS: SAMPLER2DMS
        samplersRefuse(P, "samplers.spirv_multisampled", b, D::Code::spirv, "s_tex is not a 2D sampler of floats");
        b = spv;
        put32(b, imgAt + 4 * 3, 2);                              // Dim 3D, the table still saying 2D
        samplersRefuse(P, "samplers.spirv_code_3d", b, D::Code::spirv, "s_tex is not a 2D sampler of floats");
        b = spv;
        put32(b, imgAt + 4 * 8, 4);                              // the format Rgba8: IMAGE2D_RO
        samplersRefuse(P, "samplers.spirv_fixed_format", b, D::Code::spirv, "s_tex is not a 2D sampler of floats");
    }
    // Metal: the stage and the kind are in the code, beside the uniform table.
    const std::size_t mslAt = typeAt(mtl, "s_mask") + 10;
    const std::size_t mtlParams = typeAt(mtl, "u_params"), mtlMask = typeAt(mtl, "s_mask");
    Bytes m = mtl;
    P.eq("samplers.metal_3d_patch", patch(m, "texture2d<float> s_tex [[", "texture3d<float> s_tex [[", mslAt), 1);
    samplersRefuse(P, "samplers.metal_3d", m, D::Code::metal, "s_tex is not a 2D sampler of floats");
    m = mtl;
    P.eq("samplers.metal_int_patch", patch(m, "texture2d<float> s_mask", "texture2d<int>   s_mask", mslAt), 1);
    samplersRefuse(P, "samplers.metal_int", m, D::Code::metal, "s_mask is not a 2D sampler of floats");
    m = mtl;
    P.eq("samplers.metal_stage_patch", patch(m, "s_mask [[texture(2)]]", "s_mask [[texture(4)]]", mslAt), 1);
    samplersRefuse(P, "samplers.metal_stage_4", m, D::Code::metal, "s_mask is at stage 4");
    m = mtl;
    P.eq("samplers.metal_shared_patch", patch(m, "s_mask [[texture(2)]]", "s_mask [[texture(0)]]", mslAt), 1);
    samplersRefuse(P, "samplers.metal_shared_stage", m, D::Code::metal, "both at stage 0");
    m = mtl;
    P.eq("samplers.metal_part_patch", patch(m, "s_maskSampler", "s_maskSamplex"), 1);   // the table's, first
    samplersRefuse(P, "samplers.metal_stray_part", m, D::Code::metal, "s_maskSamplex, a buffer, image or texture");
    m = mtl;
    P.eq("samplers.metal_name_patch", patch(m, std::string_view("\x06s_mask\x10", 8),
                                            std::string_view("\x06u_rect\x10", 8)), 1);
    samplersRefuse(P, "samplers.funkgui_name", m, D::Code::metal, "has the name of a uniform FunkGui");
    m = mtl;                                                     // v0.26.0: the instanced UI program's table
    P.eq("samplers.metal_unorm8_patch", renameUniform(m, "s_mask", "u_unorm8"), 1);
    samplersRefuse(P, "samplers.funkgui_name_unorm8", m, D::Code::metal, "has the name of a uniform FunkGui");
    m = mtl;
    P.eq("samplers.metal_buffer_patch", patch(m, "_mtl_u [[buffer(0)]]", "_mtl_u [[buffer(2)]]", mslAt), 1);
    samplersRefuse(P, "samplers.metal_buffer", m, D::Code::metal, "uses _mtl_u, a buffer");
    if (P.eq("samplers.found_metal_table", mtlParams != 0 && mtlMask != 0, 1))
    {
        // shaderc's Metal table types a float, vec2, vec3, bool or int vector End, at its byte offset (u_params' 0).
        m = mtl;
        m[mtlParams] = static_cast<uint8_t>((m[mtlParams] & 0xF0) | 1);
        samplersRefuse(P, "samplers.metal_plain_uniform", m, D::Code::metal,
                       "reads the uniform u_params, which FunkGui never sets");
        m = mtl;
        m[mtlMask + 1] = 1;                                      // num 1: an int uniform, not a texture (num 0)
        samplersRefuse(P, "samplers.metal_int_uniform", m, D::Code::metal, "reads the uniform s_mask, which FunkGui");
    }

    // ---- check: this platform's binary ----------------------------------------------------------------------------
#if defined(__APPLE__)
    const Bytes& mine = mtl;
    const Bytes& other = spv;
    const char* otherWhy = "holds SPIR-V, not Metal";
#else
    const Bytes& mine = spv;
    const Bytes& other = mtl;
    const char* otherWhy = "holds no SPIR-V";
#endif
    std::string why = "stale";
    const bool checked = S::check(mine.data(), static_cast<uint32_t>(mine.size()), why);
    std::printf("INFO     check(fs_texture_test): %s%s\n", checked ? "true" : "false",
                checked ? "" : (" " + why).c_str());
    const bool noMetal = !checked && why.starts_with("no Metal device");
    P.note("metal", noMetal ? "\"absent\"" : "\"present\"");
    if (!noMetal)
        P.eq("check.takes", checked && why.empty(), 1);
    // Refused before any compile: a texture of another kind, by this renderer's rule.
    m = mine;
#if defined(__APPLE__)
    P.eq("check.other_kind_patch", patch(m, "texture2d<float> s_tex [[", "texture3d<float> s_tex [[", mslAt), 1);
#else
    if (texAt != 0)
        put16(m, texAt + 6, 0x0600);
#endif
    const uint64_t compiles = D::metalCompileCount();
    checkRefuses(P, "check.other_kind", m, "s_tex is not a 2D sampler");
    P.eq("check.other_kind_not_compiled", static_cast<int64_t>(D::metalCompileCount() - compiles), 0);
    // The other renderer's binary is that, whatever its samplers read as here.
    checkRefuses(P, "check.other_renderer", other, otherWhy);
    const std::size_t params = typeAt(mine, "u_params");
    if (P.eq("check.found_u_params", params != 0, 1))
    {
        m = mine;
        m[params] = static_cast<uint8_t>(m[params] & 0xF0);    // u_params as a sampler
        checkRefuses(P, "check.params_sampler", m, "uniform vec4 u_params[4];");
        m = mine;                                               // an int uniform (no sampler bit, num 1)
        m[params] = static_cast<uint8_t>(m[params] & 0xD0);
        patch(m, "u_params", "u_paramz");
        checkRefuses(P, "check.int_uniform", m, "reads the uniform u_paramz, which FunkGui never sets");
#if defined(__APPLE__)
        m = mine;                                               // a float uniform: no type, at its offset
        m[params] = static_cast<uint8_t>((m[params] & 0xF0) | 1);
        patch(m, "u_params", "u_paramz");
        checkRefuses(P, "check.float_uniform", m, "reads the uniform u_paramz, which FunkGui never sets");
#endif
    }
    // A sampler named u_unorm8 (v0.26.0's table, a Vec4 x 64 in bgfx): refused by check(), so set() never makes it.
    m = mine;
    P.eq("check.unorm8_patch", renameUniform(m, "s_mask", "u_unorm8"), 1);
    checkRefuses(P, "check.unorm8_sampler", m, "has the name of a uniform FunkGui");
    P.eq("check.compiled_nothing", static_cast<int64_t>(D::metalCompileCount() - compiles), 0);

    // ---- set: the registry's side -----------------------------------------------------------------------------------
    if (!noMetal)
    {
        P.eq("set.registers", S::set(kA, mine.data(), static_cast<uint32_t>(mine.size()), why) && S::has(kA), 1);
        P.eq("set.bind_has_sampler", S::bindTexture(kA, "s_tex", kGrid) && S::bindTexture(kA, "s_mask", kMask), 1);
        P.eq("set.bind_unknown_sampler", S::bindTexture(kA, "s_none", kGrid), 0);
        const Bytes plain = [] {
#if defined(__APPLE__)
            return Bytes(fs_custom_test_mtl, fs_custom_test_mtl + sizeof fs_custom_test_mtl);
#else
            return Bytes(fs_custom_test_spv, fs_custom_test_spv + sizeof fs_custom_test_spv);
#endif
        }();
        P.eq("set.plain", S::set(kPlain, plain.data(), static_cast<uint32_t>(plain.size()), why), 1);
        P.eq("set.bind_plain_none", S::bindTexture(kPlain, "s_tex", kGrid), 0);
        P.eq("set.again", S::set(kA, mine.data(), static_cast<uint32_t>(mine.size()), why), 1);
        moved(gen);
        S::unbindTexture(kA, "s_mask");
        P.eq("set.keeps_bindings", moved(gen), 1);
        S::remove(kA);
        moved(gen);
        S::unbindTexture(kA, "s_tex");
        P.eq("set.remove_drops_bindings", moved(gen), 0);
        S::remove(kPlain);
    }

    for (const uint32_t id : { kA, kPlain })
        S::remove(id);
    for (const uint32_t id : { kGrid, kRamp, kTile, kMask })
        S::removeTexture(id);
    return P.finish();
}

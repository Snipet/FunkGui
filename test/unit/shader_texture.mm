// FUNKGUI_TEST name=fg.shader.texture timeout=300 gpu=1
//
// fg.shader.texture (0.25.0; gpu/ShaderLibrary.h "Textures"): textures for custom shaders. The registry's own rules
// with no bgfx, then a shader that samples known textures drawn for real by GPU pictures (renderGpuPicture: bgfx
// headless on Metal, BgfxSink's draws, read back), its pixels checked. fs_texture_test is shaders/
// fs_custom_prelude.sc + test/shaders/fs_texture_test.main.sc, compiled at build time as a host compiles one: two
// samplers, s_tex at stage 0 and s_mask at stage 2. Without a Metal device (a VM) the rows that need one are skipped
// and noted. Spec rows only.
//
// - api.*      setTexture() refuses a size outside 1..kMaxTextureSide, no texels or a short stride, changing nothing;
//              takes RGBA8 and R8 (a stride wider than a row too); removeTexture() and unbindTexture() of unknown
//              ones change nothing; bindTexture() before set() is kept and says false; the generation moves with
//              every change and only then.
// - samplers.* detail::samplers by each compiler's rules: the Metal and SPIR-V binaries give s_tex at 0 and s_mask at
//              2; the SPIR-V table read as Direct3D 11's (stage = register) after its registers are rewritten; each
//              refusal (another kind of texture, a stage of 4, two samplers at one stage, a sampler named after a
//              uniform FunkGui sets, a buffer or image that is no sampler's part; on SPIR-V a shadow sampler, an
//              integer one, a stage below the shift and a binary of version 10) with its reason.
// - set.*      check() and set() take fs_texture_test; a texture of another kind is refused before Metal compiles it;
//              u_params declared as a sampler is "declared otherwise"; bindTexture() says whether the shader has the
//              sampler.
// - plain.*    a picture of fs_custom_test (no samplers) makes no texture.
// - draw.*     one picture (dpi 1): a 4 x 4 RGBA8 nearest texture fills 64 x 64 px, each texel 16 x 16 px exactly,
//              row 0 at the top; an R8 texture at stage 2 reads (r, 0, 0, 1); a 2 x 1 linear one is interpolated
//              between texel centres; a 2 x 2 repeated one wraps where the clamped 4 x 4 one keeps its edge texels;
//              texelFetch reads texel (x, y) at v_px (x, y); an unbound sampler and a shader with no bindings read
//              (0, 0, 0, 0); one draw call per shader draw. The picture is written to shader-texture.png.
// - update.*   texels replaced (same size, then another size and format) draw at the next picture; the GPU texture is
//              made again.
// - unbind.*, remove.*, rebind.*  unbinding, removing the texture (the binding kept, reading zeros until it is set
//              again), set() of the same id keeping its bindings, remove(id) dropping them.
// - restart.*  releaseGpuPictureContext(): bgfx down, no GPU texture left, the texels and bindings kept; the next
//              picture draws the same bytes.

#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/canvas/SoftRaster.h>
#include <funkgui/core/Col.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/gpu/BgfxContext.h>
#include <funkgui/gpu/GpuPicture.h>
#include <funkgui/gpu/ShaderLibrary.h>
#include <funkgui/shaders/fs_custom_test.mtl.h>
#include <funkgui/shaders/fs_texture_test.mtl.h>
#include <funkgui/shaders/fs_texture_test.spv.h>
#include <funkgui/test/Harness.h>
#include <funkgui/text/FontService.h>

#include "../../src/gpu/ShaderLibraryDetail.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace T = funkgui::test;
namespace S = funkgui::shaders;
namespace D = funkgui::shaders::detail;
using funkgui::Col;
using funkgui::FrameInfo;
using funkgui::GpuPicture;
using funkgui::Image;
using funkgui::PrimList;
using FRect = funkgui::Rect;   // macOS has a ::Rect of its own (MacTypes.h)

namespace
{
    using Bytes = std::vector<uint8_t>;

    constexpr int kW = 160, kH = 100;               // logical = device px (dpi 1)
    constexpr Col kClear{ 18, 20, 24, 255 };
    constexpr Col kUnder{ 60, 70, 80, 255 };

    // Shader ids (fs_texture_test under each, bound differently) and texture ids.
    constexpr uint32_t kA = 21, kB = 22, kC = 23, kBare = 24, kPlain = 25;
    constexpr uint32_t kGrid = 101, kRamp = 102, kTile = 103, kMask = 104, kNever = 199;

    // The 4 x 4 grid's texel (x, y): every one its own opaque colour.
    Col gridTexel(int x, int y)
    {
        return { static_cast<uint8_t>(40 + 60 * x), static_cast<uint8_t>(30 + 70 * y),
                 static_cast<uint8_t>(200 - 40 * x - 10 * y), 255 };
    }
    // The 2 x 2 tile's.
    Col tileTexel(int x, int y)
    {
        static constexpr Col k[4] = { { 250, 20, 20, 255 }, { 20, 250, 20, 255 }, { 20, 20, 250, 255 },
                                      { 250, 250, 20, 255 } };
        return k[y * 2 + x];
    }
    constexpr uint8_t kMaskTexels[4] = { 0, 85, 170, 255 };

    template <typename F> Bytes rgba(int w, int h, F texel, int stride = 0)
    {
        const int row = stride == 0 ? w * 4 : stride;
        Bytes b(static_cast<std::size_t>(row) * static_cast<std::size_t>(h), 0xEE);   // padding: never read
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                const Col c = texel(x, y);
                uint8_t* p = &b[static_cast<std::size_t>(y * row + x * 4)];
                p[0] = c.r;
                p[1] = c.g;
                p[2] = c.b;
                p[3] = c.a;
            }
        return b;
    }

    // The draws: where each is, and what it reads.
    const FRect kGridRect{ 8.0f, 8.0f, 64.0f, 64.0f };     // kA, s_tex = the grid, 16 px a texel
    const FRect kMaskRect{ 80.0f, 8.0f, 64.0f, 8.0f };     // kA, s_mask = the R8 mask, 16 px a texel
    const FRect kRampRect{ 80.0f, 20.0f, 64.0f, 8.0f };    // kB, s_tex = the 2 x 1 linear ramp
    const FRect kUnboundRect{ 80.0f, 32.0f, 16.0f, 8.0f }; // kB, s_mask: bound to nothing
    const FRect kBareRect{ 100.0f, 32.0f, 16.0f, 8.0f };   // kBare: no bindings at all
    const FRect kTileRect{ 80.0f, 44.0f, 32.0f, 16.0f };   // kC, s_tex = the 2 x 2 tile, repeated, uv 0..2
    const FRect kClampRect{ 116.0f, 44.0f, 32.0f, 16.0f }; // kA, s_tex = the grid, clamped, uv 0..2
    const FRect kFetchRect{ 80.0f, 64.0f, 4.0f, 4.0f };    // kA, texelFetch of the grid

    FrameInfo frame()
    {
        FrameInfo f;
        f.logicalW = kW;
        f.logicalH = kH;
        f.dpi = 1.0f;
        f.clear = kClear;
        f.textGamma = 0.714285731f;
        f.fixedClock = true;
        return f;
    }

    PrimList record()
    {
        funkgui::Canvas c(funkgui::FontService::get().atlas());
        c.begin(frame());
        c.rrect(76.0f, 30.0f, 44.0f, 12.0f, 0.0f, kUnder);   // under the unbound draws: they show it
        const float tex[4] = { 0.0f, 1.0f, 0.0f, 0.0f }, mask[4] = { 1.0f, 1.0f, 0.0f, 0.0f };
        const float twice[4] = { 0.0f, 2.0f, 0.0f, 0.0f }, fetch[4] = { 2.0f, 1.0f, 0.0f, 0.0f };
        c.shader(kA, kGridRect, tex, 4);
        c.shader(kA, kMaskRect, mask, 4);
        c.shader(kB, kRampRect, tex, 4);
        c.shader(kB, kUnboundRect, mask, 4);
        c.shader(kBare, kBareRect, tex, 4);
        c.shader(kC, kTileRect, twice, 4);
        c.shader(kA, kClampRect, twice, 4);
        c.shader(kA, kFetchRect, fetch, 4);
        return c.end();
    }

    const uint8_t* at(const Image& img, int x, int y)
    {
        return &img.rgba[(static_cast<std::size_t>(y) * static_cast<std::size_t>(img.w) + static_cast<std::size_t>(x))
                         * 4u];
    }

    bool is(const Image& img, int x, int y, Col c, int tol = 0)
    {
        const uint8_t* p = at(img, x, y);
        const bool ok = std::abs(p[0] - c.r) <= tol && std::abs(p[1] - c.g) <= tol && std::abs(p[2] - c.b) <= tol;
        if (!ok)
            std::printf("INFO     pixel (%d, %d) = %u %u %u, want %u %u %u\n", x, y, p[0], p[1], p[2], c.r, c.g, c.b);
        return ok;
    }

    // The nearest texel of a w x h texture at uv, clamped or wrapped.
    void nearest(double u, double v, int w, int h, bool repeat, int& x, int& y)
    {
        auto one = [repeat](double t, int n) {
            int i = static_cast<int>(std::floor(t * n));
            if (repeat)
                i = ((i % n) + n) % n;
            return i < 0 ? 0 : (i >= n ? n - 1 : i);
        };
        x = one(u, w);
        y = one(v, h);
    }

    // The uv of a device pixel's centre in a rect, scaled.
    void uvOf(const FRect& r, int px, int py, double scale, double& u, double& v)
    {
        u = (px + 0.5 - r.x) / r.w * scale;
        v = (py + 0.5 - r.y) / r.h * scale;
    }

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

    // Where a uniform's type byte is in a binary's table (shaderc v11: name size, name, type, num, reg index, reg
    // count, tex info, tex format); 0 when absent.
    std::size_t typeAt(const Bytes& b, std::string_view name)
    {
        std::size_t at = 12;
        const unsigned count = static_cast<unsigned>(b[at]) | static_cast<unsigned>(b[at + 1]) << 8;
        at += 2;
        for (unsigned i = 0; i < count && at < b.size(); ++i)
        {
            const std::size_t n = b[at++];
            const std::string_view here(reinterpret_cast<const char*>(&b[at]), n);
            at += n;
            if (here == name)
                return at;
            at += 10;
        }
        return 0;
    }

    void put16(Bytes& b, std::size_t at, uint16_t v)
    {
        b[at] = static_cast<uint8_t>(v & 0xFF);
        b[at + 1] = static_cast<uint8_t>(v >> 8);
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
    T::Probe P("fg.shader.texture", "", argc, argv);
    const Bytes mtl(fs_texture_test_mtl, fs_texture_test_mtl + sizeof fs_texture_test_mtl);
    const Bytes spv(fs_texture_test_spv, fs_texture_test_spv + sizeof fs_texture_test_spv);
    {
        const juce::ScopedJuceInitialiser_GUI juceInit;      // the message thread (this one) and the font's bake

        // ---- api: the texture registry --------------------------------------------------------------------------
        uint64_t gen = D::generation();
        const Bytes grid = rgba(4, 4, gridTexel);
        P.eq("api.zero_width", S::setTexture(kGrid, 0, 4, S::TextureFormat::rgba8, grid.data()), 0);
        P.eq("api.too_tall", S::setTexture(kGrid, 4, S::kMaxTextureSide + 1, S::TextureFormat::rgba8, grid.data()),
             0);
        P.eq("api.no_texels", S::setTexture(kGrid, 4, 4, S::TextureFormat::rgba8, nullptr), 0);
        P.eq("api.short_stride", S::setTexture(kGrid, 4, 4, S::TextureFormat::rgba8, grid.data(), 15), 0);
        P.eq("api.r8_short_stride", S::setTexture(kMask, 4, 1, S::TextureFormat::r8, kMaskTexels, 3), 0);
        P.eq("api.refusals_change_nothing", !S::hasTexture(kGrid) && !S::hasTexture(kMask) && !moved(gen), 1);
        P.eq("api.rgba8", S::setTexture(kGrid, 4, 4, S::TextureFormat::rgba8, grid.data(), 0, { true, false })
                              && S::hasTexture(kGrid) && moved(gen), 1);
        P.eq("api.r8", S::setTexture(kMask, 4, 1, S::TextureFormat::r8, kMaskTexels, 4, { true, false })
                           && S::hasTexture(kMask) && moved(gen), 1);
        const Bytes ramp = rgba(2, 1, [](int x, int) {
            return x == 0 ? Col{ 0, 0, 0, 255 } : Col{ 255, 255, 255, 255 };
        });
        const Bytes tile = rgba(2, 2, tileTexel, 12);                 // a stride wider than a row (8 bytes)
        P.eq("api.wide_stride", S::setTexture(kTile, 2, 2, S::TextureFormat::rgba8, tile.data(), 12, { true, true }),
             1);
        P.eq("api.linear", S::setTexture(kRamp, 2, 1, S::TextureFormat::rgba8, ramp.data()), 1);
        moved(gen);
        S::removeTexture(kNever);
        S::unbindTexture(kA, "s_tex");
        P.eq("api.unknown_removals_change_nothing", moved(gen), 0);
        P.eq("api.bind_before_set_false", S::bindTexture(kA, "s_tex", kGrid), 0);
        P.eq("api.bind_moves", moved(gen), 1);
        P.eq("api.same_bind_stays", !S::bindTexture(kA, "s_tex", kGrid) && !moved(gen), 1);

        // ---- samplers: each compiler's rules --------------------------------------------------------------------
        samplersAre(P, "samplers.metal", mtl, D::Code::metal, "s_tex@0 s_mask@2");
        samplersAre(P, "samplers.spirv", spv, D::Code::spirv, "s_tex@0 s_mask@2");
        Bytes dx = spv;                                              // Direct3D 11's table: the register is the stage
        const std::size_t texAt = typeAt(spv, "s_tex"), maskAt = typeAt(spv, "s_mask");
        P.eq("samplers.found_spirv_table", texAt != 0 && maskAt != 0, 1);
        if (texAt != 0 && maskAt != 0)
        {
            put16(dx, texAt + 2, 0);
            put16(dx, maskAt + 2, 2);
            samplersAre(P, "samplers.dxbc", dx, D::Code::dxbc, "s_tex@0 s_mask@2");
            put16(dx, maskAt + 2, 4);
            samplersRefuse(P, "samplers.dxbc_stage_4", dx, D::Code::dxbc, "at stage 4");

            Bytes b = spv;
            put16(b, texAt + 6, 0x0600);                             // a 3D texture
            samplersRefuse(P, "samplers.spirv_3d", b, D::Code::spirv, "not a 2D sampler of floats");
            b = spv;
            put16(b, texAt + 6, 0x0201);                             // integer texels
            samplersRefuse(P, "samplers.spirv_int", b, D::Code::spirv, "not a 2D sampler of floats");
            b = spv;
            b[texAt] = static_cast<uint8_t>(b[texAt] | 0x80);        // a comparison (shadow) sampler
            samplersRefuse(P, "samplers.spirv_shadow", b, D::Code::spirv, "not a 2D sampler of floats");
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
            b[3] = 10;
            samplersRefuse(P, "samplers.spirv_version_10", b, D::Code::spirv, "version 10");
            b = spv;
            const std::size_t paramsAt = typeAt(spv, "u_params");
            if (P.eq("samplers.found_u_params", paramsAt != 0, 1))
            {
                b[paramsAt] = static_cast<uint8_t>((b[paramsAt] & 0xF0) | 1);   // End: a buffer or image
                samplersRefuse(P, "samplers.spirv_buffer", b, D::Code::spirv, "never binds");
            }
        }
        // Metal: the stage and the kind are in the code, beside the uniform table.
        const std::size_t mslAt = typeAt(mtl, "s_mask") + 10;
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
        samplersRefuse(P, "samplers.metal_stray_part", m, D::Code::metal, "s_maskSamplex");
        m = mtl;
        P.eq("samplers.metal_name_patch", patch(m, std::string_view("\x06s_mask\x10", 8),
                                                std::string_view("\x06u_rect\x10", 8)), 1);
        samplersRefuse(P, "samplers.funkgui_name", m, D::Code::metal, "has the name of a uniform FunkGui");

        // ---- set: through check() and set() ---------------------------------------------------------------------
        std::string why = "stale";
        const bool checked = S::check(mtl.data(), static_cast<uint32_t>(mtl.size()), why);
        std::printf("INFO     check(fs_texture_test): %s%s\n", checked ? "true" : "false",
                    checked ? "" : (" " + why).c_str());
        const bool noMetal = !checked && why.starts_with("no Metal device");
        const bool checkedClean = checked && why.empty();
        P.note("metal", noMetal ? "\"absent\"" : "\"present\"");
        m = mtl;
        patch(m, "texture2d<float> s_tex [[", "texture3d<float> s_tex [[", mslAt);
        const uint64_t compiles = D::metalCompileCount();
        P.eq("set.other_kind_refused", S::check(m.data(), static_cast<uint32_t>(m.size()), why)
                                           || why.find("not a 2D sampler") == std::string::npos, 0);
        P.eq("set.other_kind_not_compiled", static_cast<int64_t>(D::metalCompileCount() - compiles), 0);
        m = mtl;
        const std::size_t mtlParams = typeAt(mtl, "u_params");
        if (P.eq("set.found_u_params", mtlParams != 0, 1))
        {
            m[mtlParams] = static_cast<uint8_t>(m[mtlParams] & 0xF0);   // u_params as a sampler
            P.eq("set.params_sampler", S::check(m.data(), static_cast<uint32_t>(m.size()), why)
                                           || why.find("uniform vec4 u_params[4];") == std::string::npos, 0);
        }
        if (noMetal)
        {
            std::printf("INFO     no Metal device: nothing to draw on, the drawing rows skipped\n");
            S::removeTexture(kGrid);
            S::removeTexture(kMask);
            S::removeTexture(kTile);
            S::removeTexture(kRamp);
            S::unbindTexture(kA, "s_tex");
            return P.finish();
        }
        P.eq("set.checked", checkedClean, 1);
        for (const uint32_t id : { kA, kB, kC, kBare })
            P.eq("set.registers_" + std::to_string(id), S::set(id, mtl.data(), static_cast<uint32_t>(mtl.size()), why),
                 1);
        P.eq("set.plain", S::set(kPlain, fs_custom_test_mtl, sizeof fs_custom_test_mtl, why), 1);
        P.eq("set.bind_has_sampler", S::bindTexture(kA, "s_tex", kGrid) && S::bindTexture(kA, "s_mask", kMask)
                                         && S::bindTexture(kB, "s_tex", kRamp) && S::bindTexture(kC, "s_tex", kTile),
             1);
        P.eq("set.bind_unknown_sampler", S::bindTexture(kB, "s_none", kGrid), 0);
        P.eq("set.bind_plain_none", S::bindTexture(kPlain, "s_tex", kGrid), 0);

        // ---- plain: a shader without samplers makes no texture --------------------------------------------------
        {
            funkgui::Canvas c(funkgui::FontService::get().atlas());
            c.begin(frame());
            const float params[4] = { 0.0f, 0.0f, 0.5f, 1.0f };
            c.shader(kPlain, { 10.0f, 10.0f, 20.0f, 20.0f }, params, 4);
            const GpuPicture pic = funkgui::renderGpuPicture(c.end());
            P.eq("plain.ok", pic.ok() && pic.drawCalls == 1, 1);
            P.eq("plain.no_texture", D::textureCount(), 0);
        }

        // ---- draw -------------------------------------------------------------------------------------------------
        const PrimList scene = record();
        const GpuPicture first = funkgui::renderGpuPicture(scene);
        std::printf("INFO     picture: %s, %d x %d, %u draw calls\n", funkgui::toString(first.status), first.image.w,
                    first.image.h, first.drawCalls);
        if (!P.eq("draw.ok", first.ok() && first.image.w == kW && first.image.h == kH, 1))
            return P.finish();
        if (funkgui::writePng(first.image, "shader-texture.png"))   // in the test's sandbox (the working directory)
            std::printf("INFO     wrote shader-texture.png\n");
        P.eq("draw.calls", first.drawCalls, 1 + 8);                 // the rrect's run, then the eight shader draws
        P.eq("draw.textures", D::textureCount(), 5);                // the grid, mask, ramp and tile, and the zeros
        const Image& img = first.image;
        int wrong = 0;
        for (int ty = 0; ty < 4; ++ty)
            for (int tx = 0; tx < 4; ++tx)
            {
                // Every pixel of each 16 x 16 block is its texel: the corners and the centre.
                const int x0 = static_cast<int>(kGridRect.x) + tx * 16, y0 = static_cast<int>(kGridRect.y) + ty * 16;
                for (const auto& [ox, oy] : std::array<std::pair<int, int>, 5>{ { { 0, 0 }, { 15, 0 }, { 0, 15 },
                                                                                  { 15, 15 }, { 8, 8 } } })
                    wrong += !is(img, x0 + ox, y0 + oy, gridTexel(tx, ty));
            }
        P.eq("draw.grid_texels_exact", wrong, 0);
        P.eq("draw.row_0_top", is(img, 8, 8, gridTexel(0, 0)) && is(img, 8, 71, gridTexel(0, 3)), 1);
        wrong = 0;
        for (int i = 0; i < 4; ++i)
            wrong += !is(img, 80 + i * 16 + 8, 12, Col{ kMaskTexels[i], 0, 0, 255 });
        P.eq("draw.r8_reads_r001", wrong, 0);
        // The 2 x 1 ramp, linear: black at u <= 0.25, white at u >= 0.75, straight between.
        wrong = 0;
        for (const int px : { 80, 92, 96, 104, 112, 120, 128, 143 })
        {
            double u = 0.0, v = 0.0;
            uvOf(kRampRect, px, 24, 1.0, u, v);
            const double t = std::fmin(1.0, std::fmax(0.0, (u - 0.25) / 0.5));
            const auto g = static_cast<uint8_t>(std::lround(t * 255.0));
            wrong += !is(img, px, 24, Col{ g, g, g, 255 }, 2);
        }
        P.eq("draw.linear", wrong, 0);
        P.eq("draw.unbound_reads_zeros", is(img, 88, 36, kUnder), 1);
        P.eq("draw.no_bindings_reads_zeros", is(img, 108, 36, kUnder), 1);
        // Repeated: the 2 x 2 tile twice across and down; clamped: the grid's last row and column beyond 1.
        int wrongTile = 0, wrongClamp = 0;
        for (int py = 44; py < 60; py += 3)
            for (int px = 0; px < 32; px += 3)
            {
                double u = 0.0, v = 0.0;
                int x = 0, y = 0;
                uvOf(kTileRect, 80 + px, py, 2.0, u, v);
                nearest(u, v, 2, 2, true, x, y);
                wrongTile += !is(img, 80 + px, py, tileTexel(x, y));
                uvOf(kClampRect, 116 + px, py, 2.0, u, v);
                nearest(u, v, 4, 4, false, x, y);
                // The grid is nearest-sampled too: its texel exactly, clamped to the edge.
                wrongClamp += !is(img, 116 + px, py, gridTexel(x, y));
            }
        P.eq("draw.repeat_wraps", wrongTile, 0);
        P.eq("draw.clamp_keeps_edge", wrongClamp, 0);
        wrong = 0;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                wrong += !is(img, 80 + x, 64 + y, gridTexel(x, y));
        P.eq("draw.texel_fetch", wrong, 0);
        P.eq("draw.clear_elsewhere", is(img, 150, 90, kClear), 1);

        // ---- update: texels replaced --------------------------------------------------------------------------
        const Bytes grid2 = rgba(4, 4, [](int x, int y) { return Col{ static_cast<uint8_t>(250 - 50 * x),
                                                                      static_cast<uint8_t>(10 + 50 * y), 128, 255 }; });
        uint64_t g0 = D::generation();
        P.eq("update.same_size", S::setTexture(kGrid, 4, 4, S::TextureFormat::rgba8, grid2.data(), 0, { true, false })
                                     && moved(g0), 1);
        P.eq("update.texture_dropped", D::textureCount(), 4);
        GpuPicture pic = funkgui::renderGpuPicture(scene);
        P.eq("update.draws_new", pic.ok() && is(pic.image, 8 + 3 * 16 + 8, 8 + 2 * 16 + 8, Col{ 100, 110, 128, 255 }),
             1);
        P.eq("update.made_again", D::textureCount(), 5);
        // Another size and format: the R8 mask, 2 x 1, under the grid's id.
        const uint8_t two[2] = { 200, 40 };
        P.eq("update.other_size", S::setTexture(kGrid, 2, 1, S::TextureFormat::r8, two, 0, { true, false }), 1);
        pic = funkgui::renderGpuPicture(scene);
        P.eq("update.draws_other_size", pic.ok() && is(pic.image, 8 + 8, 8 + 40, Col{ 200, 0, 0, 255 })
                                            && is(pic.image, 8 + 56, 8 + 8, Col{ 40, 0, 0, 255 }), 1);
        S::setTexture(kGrid, 4, 4, S::TextureFormat::rgba8, grid.data(), 0, { true, false });

        // ---- unbind, remove, rebind -----------------------------------------------------------------------------
        S::unbindTexture(kA, "s_tex");
        pic = funkgui::renderGpuPicture(scene);
        P.eq("unbind.reads_zeros", pic.ok() && is(pic.image, 16, 16, kClear), 1);
        P.eq("unbind.others_kept", pic.ok() && is(pic.image, 104, 12, Col{ 85, 0, 0, 255 }), 1);
        P.eq("unbind.bind_again", S::bindTexture(kA, "s_tex", kGrid), 1);
        S::removeTexture(kGrid);
        P.eq("remove.gone", !S::hasTexture(kGrid) && D::textureCount() == 4, 1);
        pic = funkgui::renderGpuPicture(scene);
        P.eq("remove.reads_zeros", pic.ok() && is(pic.image, 16, 16, kClear), 1);
        S::setTexture(kGrid, 4, 4, S::TextureFormat::rgba8, grid.data(), 0, { true, false });
        pic = funkgui::renderGpuPicture(scene);
        P.eq("remove.binding_kept", pic.ok() && pic.image.rgba == first.image.rgba, 1);
        P.eq("rebind.set_keeps_bindings", S::set(kA, mtl.data(), static_cast<uint32_t>(mtl.size()), why), 1);
        pic = funkgui::renderGpuPicture(scene);
        P.eq("rebind.same_bytes", pic.ok() && pic.image.rgba == first.image.rgba, 1);
        S::remove(kC);
        P.eq("rebind.removed", S::set(kC, mtl.data(), static_cast<uint32_t>(mtl.size()), why), 1);
        pic = funkgui::renderGpuPicture(scene);
        P.eq("rebind.remove_drops_bindings", pic.ok() && is(pic.image, 84, 48, kClear), 1);
        P.eq("rebind.bind_again", S::bindTexture(kC, "s_tex", kTile), 1);

        // ---- restart ----------------------------------------------------------------------------------------------
        P.eq("restart.released", funkgui::releaseGpuPictureContext(), 1);
        P.eq("restart.down", funkgui::BgfxContext::get().valid() ? 1 : 0, 0);
        P.eq("restart.no_gpu_texture", D::textureCount(), 0);
        P.eq("restart.kept", S::hasTexture(kGrid) && S::hasTexture(kMask) && S::hasTexture(kRamp)
                                 && S::hasTexture(kTile), 1);
        pic = funkgui::renderGpuPicture(scene);
        P.eq("restart.same_bytes", pic.ok() && pic.startedContext && pic.image.rgba == first.image.rgba, 1);
        P.eq("restart.made_again", D::textureCount(), 5);

        for (const uint32_t id : { kA, kB, kC, kBare, kPlain })
            S::remove(id);
        for (const uint32_t id : { kGrid, kRamp, kTile, kMask })
            S::removeTexture(id);
        funkgui::releaseGpuPictureContext();
    }
    return P.finish();
}

#pragma once

// The custom-shader registry's side for FunkGui's own GPU code (v0.19.0; the public side is
// include/funkgui/gpu/ShaderLibrary.h). Private to src/gpu/: BgfxSink draws through it and BgfxContext hands its bgfx
// objects back before bgfx shuts down. Message thread only, except metalCompiles(), metalCompileCount() and samplers()
// (any thread, as shaders::check()).

#include <bgfx/bgfx.h>

#include <cstdint>
#include <string>
#include <vector>

namespace funkgui::shaders::detail
{
    // The program to draw id's shader draws with, built from the kept binary on first use after bgfx started (and
    // never retried after bgfx refused it, until bgfx starts again). Invalid when no program is registered under id,
    // bgfx is not running or it refused the program.
    bgfx::ProgramHandle program(uint32_t id);

    // u_rect and u_params (vec4[4]), created with the first program of a bgfx run. Invalid before.
    bgfx::UniformHandle uRect();
    bgfx::UniformHandle uParams();

    // Binds the textures of id's samplers for the next bgfx::submit (0.25.0; gpu/ShaderLibrary.h "Textures"): at each
    // sampler's stage the texture bound to its name, uploaded at its first draw, or the 1 x 1 texture of zeros. No
    // bgfx call for a shader without samplers, so a frame without textures is submitted as before. Call it after
    // program(id) returned a valid program.
    void bindTextures(uint32_t id);

    // The GPU textures alive in the registry (0.25.0): the uploaded texture ids' and the texture of zeros (tests).
    uint32_t textureCount();

    // A counter that changes with every set() that registered a program and every remove() that dropped one (0.25.0),
    // and with every texture or binding change (setTexture(), and the removeTexture(), bindTexture() and
    // unbindTexture() that changed something): EditorHost draws again when it moved, since a recording that names an
    // id draws whatever is registered and bound under it now (gpu/EditorHost.h "Unchanged frames"). Never 0.
    uint64_t generation();

    // F1 (gpu/GpuInfo.h): the programs registered (set() and not removed), whether or not bgfx has built them.
    uint32_t programCount();

    // Destroys every bgfx object the registry holds (the programs, their fragment shaders and sampler uniforms, the
    // vertex shader, the two uniforms and the textures) and keeps the binaries, the texels and the bindings. Called by
    // BgfxContext::destroyResources() while bgfx is still up.
    void releaseGpu();

    // Apple only (src/gpu/ShaderCheckMetal.mm): whether Metal's compiler takes this MSL text (bgfx's call: the text as
    // ASCII, no options) and it has bgfx's entry point, xlatMtlMain; false with Metal's log in why. Any thread: it
    // shares nothing (Metal's device is thread-safe).
    bool metalCompiles(const std::string& msl, std::string& why);

    // How many times check() and set() have run Metal's compiler in this process, refusals included (0.25.0; always 0
    // off macOS): a code they remember costs none (gpu/ShaderLibrary.h "Checking it off the message thread").
    uint64_t metalCompileCount();

    // ---- Samplers (0.25.0), for the tests ---------------------------------------------------------------------------
    // The code a binary carries, which says where a sampler's stage is: the Metal text's [[texture(n)]], the SPIR-V
    // binding less bgfx's shift (kSpirvBindShift, 2) or the Direct3D 11 register.
    enum class Code : uint8_t { metal, spirv, dxbc };

    struct Sampler
    {
        std::string name;                        // as in SAMPLER2D(name, stage)
        uint8_t     stage = 0;
    };

    // The samplers of a fragment-shader binary by the rules of `code` (whatever the platform: every rule builds and is
    // tested on every host), with set()'s reasons: false when the binary does not parse, or a sampler is not a 2D
    // sampler of floats at a stage 0 to 3 of its own (on SPIR-V, by the code's image type too: not multisampled, not
    // a storage image), is named after a uniform FunkGui or bgfx sets, or a buffer or image other than a sampler's own
    // parts is declared; and when the table holds a uniform FunkGui never sets that shaderc types as a sampler (an
    // int) or as nothing (Metal's float, vec2, vec3, bool and int vectors). The code's format is not checked here
    // (checkBinary does that first). Any thread.
    bool samplers(const void* binary, uint32_t size, Code code, std::vector<Sampler>& out, std::string& why);
}

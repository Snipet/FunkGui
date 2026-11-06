#pragma once

// The custom-shader registry's side for FunkGui's own GPU code (v0.19.0; the public side is
// include/funkgui/gpu/ShaderLibrary.h). Private to src/gpu/: BgfxSink draws through it and BgfxContext hands its bgfx
// objects back before bgfx shuts down. Message thread only, except metalCompiles() and metalCompileCount() (any
// thread, as shaders::check()).

#include <bgfx/bgfx.h>

#include <cstdint>
#include <string>

namespace funkgui::shaders::detail
{
    // The program to draw id's shader draws with, built from the kept binary on first use after bgfx started (and
    // never retried after bgfx refused it, until bgfx starts again). Invalid when no program is registered under id,
    // bgfx is not running or it refused the program.
    bgfx::ProgramHandle program(uint32_t id);

    // u_rect and u_params (vec4[4]), created with the first program of a bgfx run. Invalid before.
    bgfx::UniformHandle uRect();
    bgfx::UniformHandle uParams();

    // A counter that changes with every set() that registered a program and every remove() that dropped one (0.25.0):
    // EditorHost draws again when it moved, since a recording that names an id draws whatever is registered under it
    // now (gpu/EditorHost.h "Unchanged frames"). Never 0.
    uint64_t generation();

    // Destroys every bgfx object the registry holds (the programs, their fragment shaders, the vertex shader and the
    // two uniforms) and keeps the binaries. Called by BgfxContext::destroyResources() while bgfx is still up.
    void releaseGpu();

    // Apple only (src/gpu/ShaderCheckMetal.mm): whether Metal's compiler takes this MSL text (bgfx's call: the text as
    // ASCII, no options) and it has bgfx's entry point, xlatMtlMain; false with Metal's log in why. Any thread: it
    // shares nothing (Metal's device is thread-safe).
    bool metalCompiles(const std::string& msl, std::string& why);

    // How many times check() and set() have run Metal's compiler in this process, refusals included (0.25.0; always 0
    // off macOS): a code they remember costs none (gpu/ShaderLibrary.h "Checking it off the message thread").
    uint64_t metalCompileCount();
}

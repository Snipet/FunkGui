#pragma once

// The image program and the picture textures for BgfxSink (v0.20.0; canvas/Images.h is the registry, canvas/PrimList.h
// ImageDraw the rule). Private to src/gpu/: BgfxSink draws through it and BgfxContext hands its bgfx objects back
// before bgfx shuts down. Process-wide (every editor shares one texture per picture) and on bgfx's thread only, the
// message thread in FunkGui's single-threaded bgfx; the registry it reads is safe from any thread.

#include <funkgui/canvas/Images.h>

#include <bgfx/bgfx.h>

#include <cstdint>
#include <memory>

namespace funkgui::images::gpu
{
    // The image program: FunkGui's custom vertex shader (shaders/vs_custom.sc) with shaders/fs_image.sc, built on first
    // use after bgfx started (and not retried after bgfx refused it, until bgfx starts again). Invalid while bgfx is
    // not running on FunkGui's renderer, or when it refused the program.
    bgfx::ProgramHandle program();

    // Its sampler and uniforms (fs_image.sc), created with the program. Invalid before.
    struct Uniforms
    {
        bgfx::UniformHandle sImage = BGFX_INVALID_HANDLE;
        bgfx::UniformHandle rect = BGFX_INVALID_HANDLE;       // u_imageRect
        bgfx::UniformHandle uv = BGFX_INVALID_HANDLE;         // u_imageUv
        bgfx::UniformHandle clamp = BGFX_INVALID_HANDLE;      // u_imageClamp
        bgfx::UniformHandle mask = BGFX_INVALID_HANDLE;       // u_imageMask
    };
    const Uniforms& uniforms();

    // The texture of id's picture as the registry holds it now, with pic set to that picture: RGBA8, the whole level
    // chain, clamped, linear. Uploaded on the first draw of each revision (the texture of an older revision of id is
    // destroyed then); invalid, with pic null, when id is not registered, bgfx is not running, or bgfx refused it.
    bgfx::TextureHandle texture(uint32_t id, std::shared_ptr<const Picture>& pic);

    // Destroys the textures whose id was removed or replaced since the last sweep. Cheap while images::generation() is
    // unchanged. BgfxSink calls it at the start of every submit().
    void sweep();

    // The textures alive (BgfxSink::imageTextureCount).
    uint32_t textureCount() noexcept;

    // Destroys every bgfx object held here: the textures, the program, its shaders and uniforms. Called by
    // BgfxContext::destroyResources() while bgfx is still up; the pictures stay in the registry and are uploaded again
    // at their first draw after bgfx starts again.
    void releaseGpu();
}

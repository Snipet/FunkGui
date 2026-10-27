#include "ImageGpu.h"

#include <funkgui/gpu/BgfxContext.h>

#include <bgfx/bgfx.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// The GPU half of Canvas::image (v0.20.0; ImageGpu.h). One texture per picture id for the whole process, keyed by the
// registry's revision: a replaced picture is uploaded again at its next draw, and sweep() destroys what the registry
// no longer holds. bgfx frees a destroyed texture after the frame that may still sample it, so destroying one here
// never pulls it from under a submitted draw. Never destroyed (as ShaderLibrary.cpp's registry): a plug-in is
// unloaded while statics are torn down in an order nothing here controls, after bgfx is gone.

#if defined(__APPLE__)
  #include <funkgui/shaders/fs_image.mtl.h>
  #include <funkgui/shaders/vs_custom.mtl.h>
#else
  #include <funkgui/shaders/fs_image.spv.h>
  #include <funkgui/shaders/vs_custom.spv.h>
#endif

namespace funkgui::images::gpu
{
    namespace
    {
#if defined(__APPLE__)
        constexpr bgfx::RendererType::Enum kRenderer = bgfx::RendererType::Metal;
        constexpr const uint8_t* kVs = vs_custom_mtl;
        constexpr std::size_t    kVsBytes = sizeof vs_custom_mtl;
        constexpr const uint8_t* kFs = fs_image_mtl;
        constexpr std::size_t    kFsBytes = sizeof fs_image_mtl;
#else
        constexpr bgfx::RendererType::Enum kRenderer = bgfx::RendererType::Vulkan;
        constexpr const uint8_t* kVs = vs_custom_spv;
        constexpr std::size_t    kVsBytes = sizeof vs_custom_spv;
        constexpr const uint8_t* kFs = fs_image_spv;
        constexpr std::size_t    kFsBytes = sizeof fs_image_spv;
#endif

        struct Texture
        {
            uint32_t            id = 0;
            uint64_t            revision = 0;
            bgfx::TextureHandle handle = BGFX_INVALID_HANDLE;
        };

        struct State
        {
            bgfx::ShaderHandle  vs = BGFX_INVALID_HANDLE;
            bgfx::ShaderHandle  fs = BGFX_INVALID_HANDLE;
            bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
            bool                refused = false;     // bgfx refused it in this run: not retried before the next
            Uniforms            uniforms;
            std::vector<Texture> textures;
            uint64_t            swept = 0;           // images::generation() at the last sweep (0: never)
        };

        State& state()
        {
            static State* const s = new State();     // never destroyed (the file comment)
            return *s;
        }

        bool running()
        {
            return BgfxContext::get().valid() && bgfx::getRendererType() == kRenderer;
        }

        template <typename H> void drop(H& h)
        {
            if (bgfx::isValid(h))
                bgfx::destroy(h);
            h = BGFX_INVALID_HANDLE;
        }
    }

    bgfx::ProgramHandle program()
    {
        State& st = state();
        if (!running())
            return BGFX_INVALID_HANDLE;
        if (bgfx::isValid(st.program) || st.refused)
            return st.program;
        st.vs = bgfx::createShader(bgfx::copy(kVs, static_cast<uint32_t>(kVsBytes)));
        st.fs = bgfx::createShader(bgfx::copy(kFs, static_cast<uint32_t>(kFsBytes)));
        if (bgfx::isValid(st.vs) && bgfx::isValid(st.fs))
            st.program = bgfx::createProgram(st.vs, st.fs, false);
        if (!bgfx::isValid(st.program))
        {
            drop(st.fs);
            drop(st.vs);
            st.refused = true;
            return st.program;
        }
        Uniforms& u = st.uniforms;
        u.sImage = bgfx::createUniform("s_image", bgfx::UniformType::Sampler);
        u.rect = bgfx::createUniform("u_imageRect", bgfx::UniformType::Vec4);
        u.uv = bgfx::createUniform("u_imageUv", bgfx::UniformType::Vec4);
        u.clamp = bgfx::createUniform("u_imageClamp", bgfx::UniformType::Vec4);
        u.mask = bgfx::createUniform("u_imageMask", bgfx::UniformType::Vec4);
        return st.program;
    }

    const Uniforms& uniforms() { return state().uniforms; }

    bgfx::TextureHandle texture(uint32_t id, std::shared_ptr<const Picture>& pic)
    {
        pic = nullptr;
        if (!running())
            return BGFX_INVALID_HANDLE;
        std::shared_ptr<const Picture> now = images::picture(id);
        if (now == nullptr)
            return BGFX_INVALID_HANDLE;
        State& st = state();
        auto it = std::find_if(st.textures.begin(), st.textures.end(), [id](const Texture& t) { return t.id == id; });
        if (it != st.textures.end() && it->revision == now->revision)
        {
            pic = std::move(now);
            return it->handle;
        }
        if (it != st.textures.end())
        {
            drop(it->handle);                        // an older revision: bgfx frees it after the frames using it
            st.textures.erase(it);
        }
        const bgfx::Caps* caps = bgfx::getCaps();
        if (caps != nullptr && (static_cast<uint32_t>(now->w) > caps->limits.maxTextureSize
                                || static_cast<uint32_t>(now->h) > caps->limits.maxTextureSize))
            return BGFX_INVALID_HANDLE;
        // Every level in one block, as bgfx lays a mip chain out (canvas/Images.h builds it so).
        const bgfx::TextureHandle handle = bgfx::createTexture2D(
            static_cast<uint16_t>(now->w), static_cast<uint16_t>(now->h), now->levels.size() > 1, 1,
            bgfx::TextureFormat::RGBA8, BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
            bgfx::copy(now->pixels.data(), static_cast<uint32_t>(now->pixels.size())));
        if (!bgfx::isValid(handle))
            return handle;
        st.textures.push_back({ id, now->revision, handle });
        pic = std::move(now);
        return handle;
    }

    void sweep()
    {
        State& st = state();
        const uint64_t generation = images::generation();   // first: a change during the scan sweeps again next time
        if (generation == st.swept || !running())
            return;
        for (auto it = st.textures.begin(); it != st.textures.end();)
        {
            const std::shared_ptr<const Picture> now = images::picture(it->id);
            if (now != nullptr && now->revision == it->revision)
            {
                ++it;
                continue;
            }
            drop(it->handle);
            it = st.textures.erase(it);
        }
        st.swept = generation;
    }

    uint32_t textureCount() noexcept { return static_cast<uint32_t>(state().textures.size()); }

    void releaseGpu()
    {
        State& st = state();
        for (Texture& t : st.textures)
            drop(t.handle);
        st.textures.clear();
        drop(st.program);
        drop(st.fs);
        drop(st.vs);
        drop(st.uniforms.sImage);
        drop(st.uniforms.rect);
        drop(st.uniforms.uv);
        drop(st.uniforms.clamp);
        drop(st.uniforms.mask);
        st.refused = false;
        st.swept = 0;
    }
}

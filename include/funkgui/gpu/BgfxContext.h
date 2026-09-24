#pragma once

// Process-wide bgfx state (02 §3.1, §3.4, §4.5; A §6.4.7). bgfx is a global singleton, but a host can open several
// instances of the plugin: the first surface's window backs the default swapchain, later surfaces get their own
// framebuffers via bgfx::createFrameBuffer(nwh). Single-threaded mode (renderFrame before init) so everything happens
// on the message thread.
//
// G7: the font texture is FontService's CPU atlas uploaded once (the same bake every Canvas and HeadlessHost reads, so
// a live frame and a headless frame share glyph metrics and UVs), and the transient vertex buffer is sized from
// configure() (default FUNKGUI_TRANSIENT_VB_BYTES = 32 MiB, 02 §4.5) instead of bgfx's compiled 6 MiB.

#include <bgfx/bgfx.h>
#include <funkgui/text/FontAtlasSdf.h>

#include <cstdint>

#ifndef FUNKGUI_TRANSIENT_VB_BYTES                   // FunkGui::gpu defines it from FUNKGUI_TRANSIENT_VB_MIB (02 §1.7)
  #define FUNKGUI_TRANSIENT_VB_BYTES (32 << 20)
#endif

namespace funkgui
{
    class BgfxContext
    {
    public:
        static constexpr int kMaxWindows = 16;       // editors per process (A §2.4); the 17th waits for a slot

        // Budgets (A §6.4.7). transientVbBytes is bgfx::Init::limits.maxTransientVbSize: shared by every editor in
        // the process for one bgfx::frame(), 384 B per primitive (02 §4.5). maxWindows is clamped to 1..kMaxWindows.
        struct Config
        {
            uint32_t transientVbBytes = static_cast<uint32_t>(FUNKGUI_TRANSIENT_VB_BYTES);
            int      maxWindows = kMaxWindows;
        };

        static BgfxContext& get();

        // Sets the budgets. maxWindows applies at once (to later acquire() calls); transientVbBytes applies when bgfx
        // is next initialised, i.e. now when no window is registered, else after the last one is released. Returns
        // whether the whole configuration is in effect now (false while bgfx already runs with another buffer size).
        // Message thread; call it before the first editor opens.
        bool configure(const Config&);
        const Config& config() const noexcept { return config_; }

        // The transient vertex buffer bgfx was initialised with (0 while it is not initialised). Diagnostics.
        uint32_t transientVbBytes() const noexcept { return initialised_ ? activeTransientVbBytes_ : 0u; }

        // Register a window. Returns false if bgfx cannot initialise (the
        // editor then falls back to a plain JUCE-painted background).
        bool acquire(void* nwh, int physW, int physH);
        void release(void* nwh);

        // Per-window framebuffer. Invalid handle for the primary window
        // (which owns the default backbuffer).
        bgfx::FrameBufferHandle framebufferFor(void* nwh);
        void resizeWindow(void* nwh, int physW, int physH);

        bgfx::ViewId viewIdFor(void* nwh);

        bool valid() const { return initialised_; }

        // Whether acquire() can take another window at all. Distinct from a
        // failure: an editor that finds every slot taken waits for one.
        bool hasFreeSlot() const { return windowCount_ < maxWindows(); }

        // Whether this window is currently registered. False after a primary
        // rebuild that failed: see release().
        bool owns(void* nwh) const
        {
            for (int i = 0; i < windowCount_; ++i)
                if (windows_[i].nwh == nwh) return true;
            return false;
        }

        // Shared draw resources. One program draws every primitive.
        bgfx::ProgramHandle progUi()    const { return progUi_; }
        bgfx::UniformHandle uViewSize() const { return uViewSize_; }
        bgfx::UniformHandle sTexColor() const { return sTexColor_; }
        bgfx::TextureHandle fontTex()   const { return fontTex_; }
        const bgfx::VertexLayout& layout() const { return layout_; }

        // FontService's atlas (text/FontService.h): the CPU bake the recorder lays text out against.
        const FontAtlasSdf& font() const;

        // False when the atlas could not be baked. The GPU path still runs —
        // shapes draw, text is skipped — rather than taking the whole editor
        // down over a font problem.
        bool hasFont() const { return fontBaked_ && bgfx::isValid(fontTex_); }

        // False means the bundled face could not be loaded and the panel is
        // running on a system font — layout will differ from machine to
        // machine. Should be impossible; worth being able to ask.
        bool usingBundledFont() const;

    private:
        BgfxContext() = default;
        bool initBackend(void* nwh, int physW, int physH);
        bool createResources();
        void destroyResources();
        int  maxWindows() const noexcept;

        struct Window
        {
            void* nwh = nullptr;
            bgfx::FrameBufferHandle fb = BGFX_INVALID_HANDLE;
            bgfx::ViewId view = 0;
            int w = 0, h = 0;
            bool primary = false;
        };

        Window windows_[kMaxWindows];
        int windowCount_ = 0;
        Config config_{};
        uint32_t activeTransientVbBytes_ = 0;        // what the running bgfx was initialised with

        // View ids are allocated from a free list rather than derived from
        // windowCount_. Deriving them collided: release() compacts the array
        // without renumbering, so closing the middle of three editors and
        // opening a new one handed the newcomer an id still in use, and both
        // windows then re-pointed the same view's framebuffer every frame.
        bool viewUsed_[kMaxWindows + 1] = {};
        bgfx::ViewId allocViewId();
        void freeViewId(bgfx::ViewId v);

        bool initialised_ = false;
        bool resourcesReady_ = false;
        int  primaryW_ = 0, primaryH_ = 0;

        bgfx::VertexLayout  layout_{};
        bgfx::ProgramHandle progUi_    = BGFX_INVALID_HANDLE;
        bgfx::UniformHandle uViewSize_ = BGFX_INVALID_HANDLE;
        bgfx::UniformHandle sTexColor_ = BGFX_INVALID_HANDLE;
        bgfx::TextureHandle fontTex_   = BGFX_INVALID_HANDLE;

        // Held explicitly. The program is created with destroyShaders = false,
        // so the shaders are ours to destroy; creating them inline leaked a
        // handle per editor open/close cycle, and bgfx's 512-handle ceiling
        // then killed the GPU path for the rest of the host session.
        bgfx::ShaderHandle vsUi_ = BGFX_INVALID_HANDLE;
        bgfx::ShaderHandle fsUi_ = BGFX_INVALID_HANDLE;

        bool fontBaked_ = false;                     // FontService's atlas was baked when the texture was made
    };
}

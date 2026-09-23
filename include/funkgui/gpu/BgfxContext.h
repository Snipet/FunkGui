#pragma once

#include <bgfx/bgfx.h>
#include "FontAtlasSdf.h"

namespace funkgui
{
    // Process-wide bgfx state. bgfx is a global singleton, but a host can
    // open several instances of the plugin: the first surface's window backs
    // the default swapchain, later surfaces get their own framebuffers via
    // bgfx::createFrameBuffer(nwh). Single-threaded mode (renderFrame before
    // init) so everything happens on the message thread.
    class BgfxContext
    {
    public:
        static BgfxContext& get();

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
        bool hasFreeSlot() const { return windowCount_ < kMaxWindows; }

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
        const FontAtlasSdf& font() const { return font_; }

        // False when the atlas could not be baked. The GPU path still runs —
        // shapes draw, text is skipped — rather than taking the whole editor
        // down over a font problem.
        bool hasFont() const { return fontBaked_ && bgfx::isValid(fontTex_); }

        // False means the bundled face could not be loaded and the panel is
        // running on a system font — layout will differ from machine to
        // machine. Should be impossible; worth being able to ask.
        bool usingBundledFont() const { return font_.usedEmbeddedFace(); }

    private:
        BgfxContext() = default;
        bool initBackend(void* nwh, int physW, int physH);
        bool createResources();
        void destroyResources();

        struct Window
        {
            void* nwh = nullptr;
            bgfx::FrameBufferHandle fb = BGFX_INVALID_HANDLE;
            bgfx::ViewId view = 0;
            int w = 0, h = 0;
            bool primary = false;
        };

        static constexpr int kMaxWindows = 16;
        Window windows_[kMaxWindows];
        int windowCount_ = 0;

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

        FontAtlasSdf font_{};
        bool fontBaked_ = false;
    };
}

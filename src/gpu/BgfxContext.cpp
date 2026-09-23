#include "BgfxContext.h"

#include "BundledFont.h"

#include <cassert>
#include <cstring>

// Metal binaries produced by shaderc at build time.
#include <shaders/vs_ui.mtl.h>
#include <shaders/fs_ui.mtl.h>

namespace hrvbgui
{
    BgfxContext& BgfxContext::get()
    {
        static BgfxContext ctx;
        return ctx;
    }

    bool BgfxContext::initBackend(void* nwh, int physW, int physH)
    {
        // Single-threaded mode: calling renderFrame before init keeps all
        // rendering on the caller's (message) thread — no render thread in a
        // plugin process.
        bgfx::renderFrame();

        bgfx::Init init;
        init.type = bgfx::RendererType::Metal;   // project is Apple-only
        init.platformData.nwh   = nwh;
        init.resolution.width   = static_cast<uint32_t>(physW);
        init.resolution.height  = static_cast<uint32_t>(physH);
        init.resolution.reset   = BGFX_RESET_NONE;   // no vsync: frame() must
                                                     // never block the message
                                                     // thread; a timer paces us
        if (!bgfx::init(init))
            return false;

        primaryW_ = physW;
        primaryH_ = physH;
        initialised_ = true;
        return true;
    }

    bool BgfxContext::createResources()
    {
        if (resourcesReady_) return true;

        layout_.begin()
            .add(bgfx::Attrib::Position,  2, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Color0,    4, bgfx::AttribType::Uint8, true)
            .add(bgfx::Attrib::Color1,    4, bgfx::AttribType::Uint8, true)
            .add(bgfx::Attrib::TexCoord0, 4, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord1, 4, bgfx::AttribType::Float)
            .add(bgfx::Attrib::TexCoord2, 4, bgfx::AttribType::Float)
            .end();

        auto shader = [](const uint8_t* bin, size_t len)
        {
            return bgfx::createShader(bgfx::copy(bin, static_cast<uint32_t>(len)));
        };
        vsUi_ = shader(vs_ui_mtl, sizeof(vs_ui_mtl));
        fsUi_ = shader(fs_ui_mtl, sizeof(fs_ui_mtl));
        progUi_ = bgfx::createProgram(vsUi_, fsUi_, false);

        uViewSize_ = bgfx::createUniform("u_viewSize", bgfx::UniformType::Vec4);
        sTexColor_ = bgfx::createUniform("s_texColor", bgfx::UniformType::Sampler);

        if (!fontBaked_)
        {
            // Baked from the bundled face, not the system one: the system
            // font's metrics, shapes and glyph coverage vary by macOS version
            // and user settings, so the panel would otherwise be laid out
            // against type that differs machine to machine.
            fontBaked_ = font_.bake(BundledFont::data(), BundledFont::size());

            if (!fontBaked_)
            {
                // Unreachable in a correctly linked build — the bytes are
                // fixed at link time — so if it ever fires, something is very
                // wrong. Falling back keeps the plugin usable, but the panel
                // is then laid out against whatever the host machine happens
                // to provide, which is the exact failure bundling prevents.
                // It is deliberate and detectable here rather than silent one
                // layer down.
                assert(false && "bundled typeface failed to load");
                fontBaked_ = font_.bake();
            }
        }
        if (fontBaked_)
        {
            const auto& px = font_.pixels();
            fontTex_ = bgfx::createTexture2D(
                FontAtlasSdf::kAtlasW, FontAtlasSdf::kAtlasH, false, 1,
                bgfx::TextureFormat::R8,
                BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
                bgfx::copy(px.data(), static_cast<uint32_t>(px.size())));
        }
        else
        {
            // A 1x1 texture so the sampler always has something valid bound.
            // The canvas skips text entirely when hasFont() is false; this
            // only keeps the draw call well formed.
            static const uint8_t blank = 0;
            fontTex_ = bgfx::createTexture2D(1, 1, false, 1,
                bgfx::TextureFormat::R8,
                BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP,
                bgfx::copy(&blank, 1));
        }

        // Deliberately not gated on the font. A failed bake used to make
        // acquire() shut bgfx down and latch the editor into its no-GPU
        // fallback forever — a font problem took down the entire renderer.
        resourcesReady_ = bgfx::isValid(progUi_);
        return resourcesReady_;
    }

    void BgfxContext::destroyResources()
    {
        // Not gated on resourcesReady_: a partially built resource set (the
        // font bake failed, say) still has handles to give back, and the
        // caller reaches here precisely on that path before shutting bgfx
        // down. destroy() on an invalid handle is a no-op in bgfx.
        auto drop = [](auto& h) { if (bgfx::isValid(h)) bgfx::destroy(h); h = BGFX_INVALID_HANDLE; };
        drop(progUi_);
        drop(fsUi_);
        drop(vsUi_);
        drop(uViewSize_);
        drop(sTexColor_);
        drop(fontTex_);
        resourcesReady_ = false;
    }

    bgfx::ViewId BgfxContext::allocViewId()
    {
        // View 0 is the default backbuffer's, always taken by the primary.
        for (int i = 1; i <= kMaxWindows; ++i)
            if (!viewUsed_[i]) { viewUsed_[i] = true; return static_cast<bgfx::ViewId>(i); }
        return 0;
    }

    void BgfxContext::freeViewId(bgfx::ViewId v)
    {
        if (v > 0 && v <= kMaxWindows) viewUsed_[v] = false;
    }

    bool BgfxContext::acquire(void* nwh, int physW, int physH)
    {
        if (windowCount_ >= kMaxWindows) return false;

        if (!initialised_)
        {
            if (!initBackend(nwh, physW, physH)) return false;
            if (!createResources())
            {
                destroyResources();   // createResources may have built some
                bgfx::shutdown();
                initialised_ = false;
                return false;
            }
            Window& w = windows_[windowCount_++];
            w = {};
            w.nwh = nwh; w.primary = true; w.w = physW; w.h = physH;
            w.view = 0;
            return true;
        }

        const bgfx::ViewId view = allocViewId();
        const auto fb = bgfx::createFrameBuffer(nwh,
                                                static_cast<uint16_t>(physW),
                                                static_cast<uint16_t>(physH));
        if (!bgfx::isValid(fb))
        {
            // Do not leave a half-registered window behind. The old code
            // incremented windowCount_ first, so a failed framebuffer left a
            // phantom entry holding an nwh the editor was about to destroy —
            // and because the count could then never fall back to zero,
            // bgfx::shutdown() was never reached again either.
            freeViewId(view);
            return false;
        }

        Window& w = windows_[windowCount_++];
        w = {};
        w.nwh  = nwh;
        w.w    = physW;
        w.h    = physH;
        w.view = view;
        w.fb   = fb;
        return true;
    }

    void BgfxContext::release(void* nwh)
    {
        int idx = -1;
        for (int i = 0; i < windowCount_; ++i)
            if (windows_[i].nwh == nwh) { idx = i; break; }
        if (idx < 0) return;

        const bool wasPrimary = windows_[idx].primary;
        if (bgfx::isValid(windows_[idx].fb))
            bgfx::destroy(windows_[idx].fb);
        freeViewId(windows_[idx].view);
        for (int i = idx; i < windowCount_ - 1; ++i)
            windows_[i] = windows_[i + 1];
        --windowCount_;

        if (windowCount_ == 0)
        {
            destroyResources();
            bgfx::shutdown();
            initialised_ = false;
            return;
        }

        if (wasPrimary)
        {
            // The default swapchain's window is gone but other editors are
            // still open. Rebuild bgfx on the next remaining window and
            // recreate everything (framebuffers, programs, the atlas).
            for (int i = 0; i < windowCount_; ++i)
                if (bgfx::isValid(windows_[i].fb))
                { bgfx::destroy(windows_[i].fb); windows_[i].fb = BGFX_INVALID_HANDLE; }
            destroyResources();
            bgfx::shutdown();
            initialised_ = false;

            Window survivors[kMaxWindows];
            const int n = windowCount_;
            for (int i = 0; i < n; ++i) survivors[i] = windows_[i];
            windowCount_ = 0;
            for (int i = 0; i <= kMaxWindows; ++i) viewUsed_[i] = false;

            if (initBackend(survivors[0].nwh, survivors[0].w, survivors[0].h)
                && createResources())
            {
                // Register survivor 0 as the new primary directly. Routing it
                // back through acquire() took the already-initialised branch
                // and built a second swapchain on the very NSView that now
                // backs the default one — and left primary unset on every
                // window, so the next primary close could not rebuild again.
                Window& p = windows_[windowCount_++];
                p = {};
                p.nwh = survivors[0].nwh; p.primary = true;
                p.w = survivors[0].w; p.h = survivors[0].h;
                p.view = 0;
                for (int i = 1; i < n; ++i)
                    acquire(survivors[i].nwh, survivors[i].w, survivors[i].h);
            }
            // Otherwise: no window is registered and initialised_ is false.
            // That is deliberately a recoverable state rather than a silent
            // one — each surviving editor notices on its next frame that
            // owns() is false, detaches, and re-attaches through acquire(),
            // whose first caller re-initialises the backend on its own view.
        }
    }

    bgfx::FrameBufferHandle BgfxContext::framebufferFor(void* nwh)
    {
        for (int i = 0; i < windowCount_; ++i)
            if (windows_[i].nwh == nwh) return windows_[i].fb;
        return BGFX_INVALID_HANDLE;
    }

    bgfx::ViewId BgfxContext::viewIdFor(void* nwh)
    {
        for (int i = 0; i < windowCount_; ++i)
            if (windows_[i].nwh == nwh) return windows_[i].view;
        return 0;
    }

    void BgfxContext::resizeWindow(void* nwh, int physW, int physH)
    {
        for (int i = 0; i < windowCount_; ++i)
        {
            Window& w = windows_[i];
            if (w.nwh != nwh) continue;
            if (w.w == physW && w.h == physH) return;
            w.w = physW; w.h = physH;
            if (w.primary)
            {
                primaryW_ = physW; primaryH_ = physH;
                bgfx::reset(static_cast<uint32_t>(physW),
                            static_cast<uint32_t>(physH), BGFX_RESET_NONE);
            }
            else
            {
                if (bgfx::isValid(w.fb)) bgfx::destroy(w.fb);
                w.fb = bgfx::createFrameBuffer(nwh,
                                               static_cast<uint16_t>(physW),
                                               static_cast<uint16_t>(physH));
            }
            return;
        }
    }
}

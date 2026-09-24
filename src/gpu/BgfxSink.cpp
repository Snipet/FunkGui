#include <funkgui/gpu/BgfxSink.h>

#include <funkgui/gpu/BgfxContext.h>

#include <cstring>

// BgfxSink::submit (02 §3.4, §4.5; G7): the snapshot's SdfCanvas::begin()/end() (gpu/SdfCanvas.cpp, HR
// Source/gui/SdfCanvas.cpp) split at the recorder: the view setup and the single draw call are HR's call for call,
// and the vertex stream is funkgui::expand's, which fg.canvas.expansion proves equal to HR's vertices bit for bit.

namespace funkgui
{
    namespace
    {
        // The view clear as bgfx wants it (0xRRGGBBAA), from the theme's ground: never a duplicated literal, so a
        // theme cannot desync the two (HR).
        uint32_t clearRgba(Col c) noexcept
        {
            return (static_cast<uint32_t>(c.r) << 24) | (static_cast<uint32_t>(c.g) << 16)
                 | (static_cast<uint32_t>(c.b) << 8) | 0xffu;
        }

        uint16_t px16(int v) noexcept
        {
            return static_cast<uint16_t>(v < 0 ? 0 : (v > 0xffff ? 0xffff : v));
        }
    }

    BgfxSink::Result BgfxSink::submit(const PrimList& list, bgfx::ViewId view, bgfx::FrameBufferHandle fb, int physW,
                                      int physH)
    {
        const BgfxContext& ctx = BgfxContext::get();
        const FrameInfo& info = list.info;

        bgfx::setViewFrameBuffer(view, fb);
        bgfx::setViewRect(view, 0, 0, px16(physW), px16(physH));
        bgfx::setViewClear(view, BGFX_CLEAR_COLOR, clearRgba(info.clear));
        bgfx::setViewMode(view, bgfx::ViewMode::Sequential);
        bgfx::touch(view);                           // the clear happens even when nothing is drawn

        lastVertices_ = 0;
        if (list.prims.empty())
            return Result::empty;

        expand(list, scratch_);
        const auto n = static_cast<uint32_t>(scratch_.size());
        if (bgfx::getAvailTransientVertexBuffer(n, ctx.layout()) < n)
        {
            // The whole frame is one batch, so an exhausted buffer drops everything rather than one class of
            // widget. Counted, never silent (02 §4.5).
            ++overflows_;
            return Result::droppedOverflow;
        }

        bgfx::TransientVertexBuffer tvb;
        bgfx::allocTransientVertexBuffer(&tvb, n, ctx.layout());
        std::memcpy(tvb.data, scratch_.data(), static_cast<size_t>(n) * sizeof(Vtx));

        const float dpi = info.dpi > 0.05f ? info.dpi : 1.0f;     // HR's guard on the backing scale
        const float viewSize[4] = { static_cast<float>(info.logicalW), static_cast<float>(info.logicalH), 1.0f / dpi,
                                    info.seconds };
        bgfx::setUniform(ctx.uViewSize(), viewSize);
        bgfx::setTexture(0, ctx.sTexColor(), ctx.fontTex());
        bgfx::setVertexBuffer(0, &tvb, 0, n);
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_BLEND_ALPHA);
        bgfx::submit(view, ctx.progUi());
        lastVertices_ = n;
        return Result::submitted;
    }
}

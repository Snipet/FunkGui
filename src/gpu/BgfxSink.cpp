#include <funkgui/gpu/BgfxSink.h>

#include "ShaderLibraryDetail.h"

#include <funkgui/gpu/BgfxContext.h>

#include <cstring>

// BgfxSink::submit (02 §3.4, §4.5; G7): the snapshot's SdfCanvas::begin()/end() (gpu/SdfCanvas.cpp, HR
// Source/gui/SdfCanvas.cpp) split at the recorder: the view setup and the single draw call are HR's call for call,
// and the vertex stream is funkgui::expand's, which fg.canvas.expansion proves equal to HR's vertices bit for bit.
// v0.19.0: a frame with shader draws is the same calls, split into the runs funkgui::drawSteps gives, with each
// shader draw submitted between them (gpu/BgfxSink.h); without shader draws there is one run, so one draw call.

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

        constexpr uint64_t kState = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_BLEND_ALPHA;
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
        lastDraws_ = 0;
        if (list.prims.empty() && list.shaders.empty())
            return Result::empty;

        expand(list, scratch_);
        const auto primVertices = static_cast<uint32_t>(scratch_.size());
        if (!list.shaders.empty())
        {
            // v0.19.0: each shader draw's quad after the primitives' vertices, in list order.
            scratch_.resize(scratch_.size() + list.shaders.size() * static_cast<size_t>(kVerticesPerPrim));
            Vtx* dst = scratch_.data() + primVertices;
            for (const ShaderDraw& s : list.shaders)
            {
                expandShader(s, dst);
                dst += kVerticesPerPrim;
            }
        }
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
        drawSteps(list, steps_);
        for (const DrawStep& step : steps_)
        {
            if (step.kind == DrawStep::Kind::prims)
            {
                // HR's call for call; without shader draws this is the frame's one draw call over every vertex.
                bgfx::setUniform(ctx.uViewSize(), viewSize);
                bgfx::setTexture(0, ctx.sTexColor(), ctx.fontTex());
                bgfx::setVertexBuffer(0, &tvb, step.first * static_cast<uint32_t>(kVerticesPerPrim),
                                      step.count * static_cast<uint32_t>(kVerticesPerPrim));
                bgfx::setState(kState);
                bgfx::submit(view, ctx.progUi());
                ++lastDraws_;
                continue;
            }
            const ShaderDraw& s = list.shaders[step.first];
            const bgfx::ProgramHandle program = shaders::detail::program(s.id);
            const PixelRect px = shaderPixels(s, info, physW, physH);
            if (!bgfx::isValid(program) || px.w <= 0 || px.h <= 0)
                continue;                            // no program, or nothing of it on screen: no draw call
            const float rect[4] = { s.rect.x, s.rect.y, s.rect.w, s.rect.h };
            bgfx::setUniform(ctx.uViewSize(), viewSize);
            bgfx::setUniform(shaders::detail::uRect(), rect);
            bgfx::setUniform(shaders::detail::uParams(), s.params, 4);
            bgfx::setScissor(px16(px.x), px16(px.y), px16(px.w), px16(px.h));
            bgfx::setVertexBuffer(0, &tvb, primVertices + step.first * static_cast<uint32_t>(kVerticesPerPrim),
                                  static_cast<uint32_t>(kVerticesPerPrim));
            bgfx::setState(kState);
            bgfx::submit(view, program);
            ++lastDraws_;
        }
        lastVertices_ = n;
        return Result::submitted;
    }
}

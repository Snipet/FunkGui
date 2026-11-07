#include <funkgui/gpu/BgfxSink.h>

#include "ImageGpu.h"
#include "ShaderLibraryDetail.h"

#include <funkgui/gpu/BgfxContext.h>

#include <cstring>
#include <memory>

// BgfxSink::submit (02 §3.4, §4.5; G7): the snapshot's SdfCanvas::begin()/end() (gpu/SdfCanvas.cpp, HR
// Source/gui/SdfCanvas.cpp) split at the recorder: the view setup and the single draw call are HR's call for call,
// and the vertex stream is funkgui::expand's, which fg.canvas.expansion proves equal to HR's vertices bit for bit.
// v0.19.0: a frame with shader draws is the same calls, split into the runs funkgui::drawSteps gives, with each
// shader draw submitted between them (gpu/BgfxSink.h); without shader draws there is one run, so one draw call.
// v0.20.0: image draws are split out the same way, each one draw call with the image program (src/gpu/ImageGpu.h).
// v0.26.0: a run of primitives is one instanced draw call over the context's quad (gpu/BgfxSink.h), the same draw calls
// in the same order; a frame drawn as vertices is the code below as it was.

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

        // v0.26.0: u_unorm8 holds canvas/Expand.h unorm8Values' 256 floats, four to a vec4 (BgfxContext's uniform).
        constexpr uint16_t kUnorm8Vec4s = 256 / 4;

        // v0.26.0: whether the transient buffer has `bytes` left, asked in 16-byte units. Every allocation FunkGui
        // makes there starts and ends on 16 bytes (64-byte vertices, 80-byte records), so bgfx rounds nothing here.
        // Asked with a stride the buffer's offset is not a multiple of, bgfx first rounds the offset up, and past the
        // buffer's end its count wraps: a 32 MiB buffer filled to its last byte by vertices, asked for 80-byte records,
        // says that millions fit.
        bool roomFor(uint64_t bytes) noexcept
        {
            constexpr uint64_t kMostUnits = uint64_t{ 1 } << 26;  // 1 GiB: more than any buffer, and no wrap in bgfx
            const uint64_t units = (bytes + 15) / 16;
            if (units == 0)
                return true;
            return units <= kMostUnits && bgfx::getAvailInstanceDataBuffer(static_cast<uint32_t>(units), 16) >= units;
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
        lastDraws_ = 0;
        lastBytes_ = 0;
        images::gpu::sweep();                        // v0.20.0: textures of removed or replaced pictures go
        if (list.prims.empty() && list.shaders.empty() && list.images.empty())
            return Result::empty;

        // v0.26.0: the primitives as instances when the context can and every one of them is instanceable; else as
        // expand()'s vertices, at the head of the vertex buffer as before.
        const bool asInstances = ctx.config().instancing && ctx.instancing() && instances(list, instances_);
        const auto nInstances = asInstances ? static_cast<uint32_t>(instances_.size()) : 0u;
        if (asInstances)
            scratch_.clear();
        else
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
        const auto imageVertices = static_cast<uint32_t>(scratch_.size());
        if (!list.images.empty())
        {
            // v0.20.0: each image draw's quad after those, in list order.
            scratch_.resize(scratch_.size() + list.images.size() * static_cast<size_t>(kVerticesPerPrim));
            Vtx* dst = scratch_.data() + imageVertices;
            for (const ImageDraw& m : list.images)
            {
                expandImage(m, dst);
                dst += kVerticesPerPrim;
            }
        }
        const auto n = static_cast<uint32_t>(scratch_.size());
        // v0.26.0: the instance records and the vertices come out of the same transient buffer, records first, each
        // allocation starting at the next multiple of its stride (less than one stride on). Room for both, that
        // padding included, is checked before either is allocated, so a frame that does not fit takes nothing from
        // the buffer and a later editor's frame in the same bgfx::frame() may still fit, as before v0.26.0. Without
        // records the check is the one before v0.26.0 (bgfx's count, exact for vertices alone), roomFor's first so
        // that bgfx's count cannot wrap after another editor's records.
        bool fits = true;
        if (nInstances > 0)
            fits = roomFor(uint64_t{ nInstances } * kInstanceBytes + (kInstanceBytes - 1)
                           + (n > 0 ? uint64_t{ n } * sizeof(Vtx) + (sizeof(Vtx) - 1) : 0u));
        else if (n > 0)
            fits = roomFor(uint64_t{ n } * sizeof(Vtx)) && bgfx::getAvailTransientVertexBuffer(n, ctx.layout()) >= n;
        if (!fits)
        {
            // The whole frame is one batch, so an exhausted buffer drops everything rather than one class of
            // widget. Counted, never silent (02 §4.5).
            ++overflows_;
            return Result::droppedOverflow;
        }

        bgfx::InstanceDataBuffer idb{};
        if (nInstances > 0)
        {
            bgfx::allocInstanceDataBuffer(&idb, nInstances, static_cast<uint16_t>(kInstanceBytes));
            std::memcpy(idb.data, instances_.data(), static_cast<size_t>(nInstances) * sizeof(Inst));
        }
        bgfx::TransientVertexBuffer tvb{};
        if (n > 0)
        {
            bgfx::allocTransientVertexBuffer(&tvb, n, ctx.layout());
            std::memcpy(tvb.data, scratch_.data(), static_cast<size_t>(n) * sizeof(Vtx));
        }
        const float* unorm8 = unorm8Values();

        const float dpi = info.dpi > 0.05f ? info.dpi : 1.0f;     // HR's guard on the backing scale
        const float viewSize[4] = { static_cast<float>(info.logicalW), static_cast<float>(info.logicalH), 1.0f / dpi,
                                    info.seconds };
        drawSteps(list, steps_);
        for (const DrawStep& step : steps_)
        {
            if (step.kind == DrawStep::Kind::prims && asInstances)
            {
                // v0.26.0: the same draw call with one instance per primitive of the run.
                bgfx::setUniform(ctx.uViewSize(), viewSize);
                bgfx::setUniform(ctx.uUnorm8(), unorm8, kUnorm8Vec4s);
                bgfx::setTexture(0, ctx.sTexColor(), ctx.fontTex());
                bgfx::setVertexBuffer(0, ctx.quad());
                bgfx::setIndexBuffer(ctx.quadIndices());
                bgfx::setInstanceDataBuffer(&idb, step.first, step.count);
                bgfx::setState(kState);
                bgfx::submit(view, ctx.progUiInstanced());
                ++lastDraws_;
                continue;
            }
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
            if (step.kind == DrawStep::Kind::image)
            {
                // v0.20.0: one picture with the image program (shaders/fs_image.sc; canvas/PrimList.h ImageDraw).
                const ImageDraw& m = list.images[step.first];
                std::shared_ptr<const images::Picture> pic;
                const bgfx::TextureHandle texture = images::gpu::texture(m.id, pic);
                const bgfx::ProgramHandle program = images::gpu::program();
                const PixelRect px = imagePixels(m, info, physW, physH);
                if (!bgfx::isValid(texture) || !bgfx::isValid(program) || pic == nullptr || px.w <= 0 || px.h <= 0)
                    continue;                        // no picture, no program, or nothing of it on screen
                const int level = imageLevel(m, dpi, pic->w, pic->h);
                const ImageSampling smp = imageSampling(m, level, pic->w, pic->h);
                const images::gpu::Uniforms& u = images::gpu::uniforms();
                const float rect[4] = { m.dst.x, m.dst.y, m.dst.w, m.dst.h };
                const float uv[4] = { smp.u0, smp.v0, smp.du, smp.dv };
                const float clamp[4] = { smp.loU, smp.loV, smp.hiU, smp.hiV };
                const float mask[4] = { m.radius, m.opacity, static_cast<float>(level), 0.0f };
                bgfx::setUniform(ctx.uViewSize(), viewSize);
                bgfx::setUniform(u.rect, rect);
                bgfx::setUniform(u.uv, uv);
                bgfx::setUniform(u.clamp, clamp);
                bgfx::setUniform(u.mask, mask);
                bgfx::setTexture(0, u.sImage, texture);
                bgfx::setScissor(px16(px.x), px16(px.y), px16(px.w), px16(px.h));
                bgfx::setVertexBuffer(0, &tvb, imageVertices + step.first * static_cast<uint32_t>(kVerticesPerPrim),
                                      static_cast<uint32_t>(kVerticesPerPrim));
                bgfx::setState(kState);
                bgfx::submit(view, program);
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
            shaders::detail::bindTextures(s.id);     // 0.25.0: its samplers' textures; none without samplers
            bgfx::setScissor(px16(px.x), px16(px.y), px16(px.w), px16(px.h));
            bgfx::setVertexBuffer(0, &tvb, primVertices + step.first * static_cast<uint32_t>(kVerticesPerPrim),
                                  static_cast<uint32_t>(kVerticesPerPrim));
            bgfx::setState(kState);
            bgfx::submit(view, program);
            ++lastDraws_;
        }
        lastVertices_ = n + nInstances * static_cast<uint32_t>(kVerticesPerPrim);
        lastBytes_ = n * static_cast<uint32_t>(sizeof(Vtx)) + nInstances * static_cast<uint32_t>(kInstanceBytes);
        lastGeometry_ = asInstances ? Geometry::instances : Geometry::vertices;
        return Result::submitted;
    }

    uint32_t BgfxSink::imageTextureCount() noexcept { return images::gpu::textureCount(); }
}

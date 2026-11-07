#pragma once

// A recorded frame onto the GPU (02 §3.4, §4.5): the PrimList a Canvas recorded is expanded into HR's six 64-byte
// vertices per primitive by the bgfx-free funkgui::expand (canvas/Expand.h, bit-identical to what HR's SdfCanvas
// pushed for the same calls) and submitted as ONE draw call into one transient vertex buffer, with BgfxContext's
// program, uniform and font texture. Replaces the snapshot's SdfCanvas::end() (retired by G7).
//
// v0.19.0, shader draws (Canvas::shader; canvas/PrimList.h ShaderDraw): a frame that has any is split at each of them
// (funkgui::drawSteps, canvas/Expand.h): the run of primitives before it with the UI program, then the shader draw
// with the program registered under its id (gpu/ShaderLibrary.h), then the next run, all in the one view in
// sequential mode, so the drawing order is the recording order. Every draw of the frame reads the one transient
// vertex buffer: the primitives' vertices, then each shader draw's six (funkgui::expandShader), in list order. A
// shader draw's state is the UI's (BGFX_STATE_WRITE_RGB | WRITE_A | BLEND_ALPHA), its scissor funkgui::shaderPixels
// (rect ∩ clip in device px), its uniforms u_viewSize (the UI's value), u_rect and u_params, and it binds no texture
// unless its shader declares samplers (0.25.0: then each sampler's bound texture at its stage, or a 1 x 1 texture of
// zeros; gpu/ShaderLibrary.h "Textures"). One whose program is missing or was refused, or whose scissor is empty,
// makes no draw call. A frame without shader draws is submitted exactly as before v0.19.0: one draw call, the same
// calls in the same order.
//
// v0.20.0, image draws (Canvas::image; canvas/PrimList.h ImageDraw): split out of the runs the same way (drawSteps,
// in recording order with the shader draws), each one draw call of FunkGui's image program (shaders/vs_custom.sc with
// shaders/fs_image.sc) over its six vertices (funkgui::expandImage, after the shader draws' in the one buffer), with
// the UI's state, its scissor funkgui::imagePixels, the uniforms u_viewSize, u_imageRect, u_imageUv, u_imageClamp
// (canvas/Expand.h imageSampling) and u_imageMask (radius, opacity, imageLevel), and its picture's texture on stage 0.
// The textures are process-wide, one per picture id: RGBA8 holding the registry's premultiplied level chain
// (canvas/Images.h), clamped, linear, made at the first draw of each revision (a replaced picture is uploaded again),
// destroyed at the start of the first submit() after images::remove() or a replacement, and all of them when bgfx
// shuts down (they come back at their next draw after it starts again). A draw whose picture is not registered, or
// whose texture or program bgfx refused, or whose scissor is empty, makes no draw call.
//
// v0.26.0, primitives as instances: while BgfxContext::instancing() and its Config::instancing are true, a run of
// primitives is one instanced draw call of BgfxContext::progUiInstanced() (shaders/vs_ui_inst.sc with fs_ui.sc) over
// the context's unit quad and its indices, one 80-byte record per primitive (canvas/Expand.h Inst,
// funkgui::instances) in one transient instance data buffer, with u_unorm8 set to canvas/Expand.h unorm8Values; the
// shader and image draws keep their six vertices each in the transient vertex buffer. The draw calls, their order,
// state, textures and scissors are the ones above, and the vertex shader makes the very vertices expand() writes, so
// the pixels are the same (fg.gpu.instancing). A frame is drawn otherwise, as vertices exactly as before v0.26.0, when
// instancing is off or unavailable, or when one of its primitives is not instanceable (canvas/Expand.h: a kind that
// is not an integer 0..255, which no Canvas records). lastGeometry() says which, lastUploadBytes() what it copied.
//
// The transient buffer is shared by every editor in the process for one bgfx::frame(). When this frame does not fit,
// nothing is drawn and submit() returns droppedOverflow (HR dropped the frame silently, SdfCanvas.cpp:322-329); the
// caller makes it visible (EditorHost: Diagnostics::overflows, a GPU_LOG line, a Debug assertion and the next dump's
// "overflow N"). Message thread; bgfx must be initialised (BgfxContext::valid()).

#include <funkgui/canvas/Expand.h>
#include <funkgui/canvas/PrimList.h>

#include <bgfx/bgfx.h>

#include <cstdint>
#include <vector>

namespace funkgui
{
    class BgfxSink
    {
    public:
        enum class Result : uint8_t { submitted, empty, droppedOverflow };

        // v0.26.0: how a frame's primitives reached the GPU (the header): one instance each, or six vertices each.
        enum class Geometry : uint8_t { instances, vertices };

        // Points `view` at `fb` (invalid = the default backbuffer) over physW x physH device px, clears it to
        // list.info.clear, sequential mode, touch; then, unless the list has neither a primitive nor a shader or image
        // draw, expands it, allocates one transient vertex buffer and submits its draw calls (one without shader and
        // image draws).
        // u_viewSize = (logicalW, logicalH, 1 / dpi, seconds) from list.info. Does not call bgfx::frame() (the
        // FramePump does, once for every editor).
        Result submit(const PrimList& list, bgfx::ViewId view, bgfx::FrameBufferHandle fb, int physW, int physH);

        uint32_t overflowCount() const noexcept { return overflows_; }   // lifetime drops, shown in diagnostics
        // The vertices the last submit() drew (0 unless it returned submitted): six per primitive, shader and image
        // draw, whether its primitives were instances (v0.26.0) or vertices.
        uint32_t lastVertexCount() const noexcept { return lastVertices_; }
        // v0.19.0: the bgfx::submit() calls the last submit() made (0 unless it returned submitted): 1 for a frame
        // without shader draws; with them, its runs of primitives plus the shader draws that drew (v0.20.0: and the
        // image draws that drew).
        uint32_t lastDrawCalls() const noexcept { return lastDraws_; }
        // v0.20.0: the picture textures alive in the process, over every sink (diagnostics, tests).
        static uint32_t imageTextureCount() noexcept;
        // v0.26.0: how the last submit() that returned submitted drew its primitives (a frame without any: how it
        // would have), and the bytes it copied into bgfx's transient buffers (0 unless it returned submitted): 80 per
        // primitive as an instance (canvas/Expand.h kInstanceBytes), 384 as vertices, 384 per shader and image draw.
        Geometry lastGeometry() const noexcept { return lastGeometry_; }
        uint32_t lastUploadBytes() const noexcept { return lastBytes_; }

    private:
        std::vector<Vtx> scratch_;                   // 6 * (prims as vertices + shader + image draws), capacity kept
        std::vector<Inst> instances_;                // v0.26.0: one per primitive as an instance, capacity kept
        std::vector<DrawStep> steps_;                // v0.19.0: the frame's draw calls, capacity kept
        uint32_t overflows_ = 0;
        uint32_t lastVertices_ = 0;
        uint32_t lastDraws_ = 0;
        uint32_t lastBytes_ = 0;
        Geometry lastGeometry_ = Geometry::instances;
    };
}

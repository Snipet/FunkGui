#pragma once

// A recorded frame onto the GPU (02 §3.4, §4.5): the PrimList a Canvas recorded is expanded into HR's six 64-byte
// vertices per primitive by the bgfx-free funkgui::expand (canvas/Expand.h, bit-identical to what HR's SdfCanvas
// pushed for the same calls) and submitted as ONE draw call into one transient vertex buffer, with BgfxContext's
// program, uniform and font texture. Replaces the snapshot's SdfCanvas::end() (retired by G7).
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

        // Points `view` at `fb` (invalid = the default backbuffer) over physW x physH device px, clears it to
        // list.info.clear, sequential mode, touch; then, unless the list is empty, expands it, allocates one transient
        // vertex buffer and submits one draw call. u_viewSize = (logicalW, logicalH, 1 / dpi, seconds) from list.info.
        // Does not call bgfx::frame() (the FramePump does, once for every editor).
        Result submit(const PrimList& list, bgfx::ViewId view, bgfx::FrameBufferHandle fb, int physW, int physH);

        uint32_t overflowCount() const noexcept { return overflows_; }   // lifetime drops, shown in diagnostics
        uint32_t lastVertexCount() const noexcept { return lastVertices_; }

    private:
        std::vector<Vtx> scratch_;                   // 6 * prims, capacity kept across frames
        uint32_t overflows_ = 0;
        uint32_t lastVertices_ = 0;
    };
}

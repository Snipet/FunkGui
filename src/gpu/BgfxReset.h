#pragma once

// bgfx's reset flags (v0.25.1): the ones BgfxContext gives bgfx::init and every bgfx::reset. bgfx keeps the flags of
// its last init or reset only, so a reset without one of them turns it off; there is no other call that sets them.
// fg.gpu.flip checks them, and what they do.
// - No vsync (no BGFX_RESET_VSYNC): bgfx::frame() never waits for the display. The FramePump's clock (a display link,
//   or a timer) paces the frames.
// - BGFX_RESET_FLIP_AFTER_RENDER: bgfx runs single-threaded here (bgfx::renderFrame() before bgfx::init, BgfxContext),
//   so each bgfx::frame() renders the frame just submitted. With the flag it then presents it ("flips") before it
//   returns. Without it, bgfx presents a frame at the start of the next bgfx::frame(), before rendering that one. Since
//   0.25.0 the FramePump runs no bgfx::frame() while nothing changes, so the last frame drawn stayed rendered but not
//   shown until something drew again: a hover that showed the pointer's previous position, a view that appeared only
//   at the next pointer move or readout change. The flag moves only bgfx's own call to the renderer's flip()
//   (Context::renderFrame), on every renderer: Metal commits a command buffer that presents the frame's drawables,
//   Vulkan queues its swapchains' presents, Direct3D 11 presents its swap chains. None of them waits for the display
//   without vsync, and a headless bgfx (a GPU picture) has nothing to present either way.

#include <bgfx/defines.h>

#include <cstdint>

namespace funkgui::detail
{
    inline constexpr uint32_t kBgfxResetFlags = BGFX_RESET_FLIP_AFTER_RENDER;
}

#pragma once

// GPU pictures (v0.23.0): a recorded frame drawn offscreen by bgfx and read back as RGBA pixels, so that a product that
// shows a picture of its UI (to a person, or to a model through a tool) shows it as the screen does: custom shaders
// (Canvas::shader) and pictures (Canvas::image) drawn for real, where SoftRaster (canvas/SoftRaster.h) draws each
// shader draw as its striped placeholder.
//
// ---- What is drawn --------------------------------------------------------------------------------------------------
// The PrimList (what Canvas::end() returns: an editor's frame, HeadlessHost's frame) is submitted by BgfxSink::submit
// itself, with BgfxContext's program, font texture, custom programs (gpu/ShaderLibrary.h) and picture textures, the
// same state and the same draw calls in the same order as an editor's frame, into an offscreen BGRA8 render target (the
// format of an editor's drawable) cleared to info.clear, which is then copied to a read-back texture and read.
// - Size and scale are the list's own: round(info.logicalW * dpi) x round(info.logicalH * dpi) device px, SoftRaster's
//   size (a dpi <= 0.05 or not finite draws at 1, as Canvas::begin). Record the frame at the scale wanted
//   (FrameInfo::dpi, HeadlessHost's dpi). A shader's clock (u_viewSize.w) is info.seconds.
// - The pixels are an Image (canvas/SoftRaster.h): RGBA8, straight alpha, row-major from the top-left, w x h as above,
//   and opaque (every alpha 255): the frame over its clear colour, as an editor's window shows it. (The render target's
//   own alpha channel is not the frame's: the UI's blend applies to it too.)
// - Against SoftRaster's image of the same list at one sample per pixel (rasterise(list, font, 1)), away from shader
//   draws: within fg.image.draw's bounds for Metal (no channel of any pixel off by more than 4, at most 1 % of the
//   pixels off by more than 2); fg.gpu.picture holds a frame of every primitive kind, text, a picture and a shader to
//   them (measured on an Apple M5 at dpi 2 and 1.25: largest difference 1, no pixel over 2). A HeadlessHost's frame is
//   renderGpuPicture(host.draw()); its writePng() supersamples (2 by default) where the GPU and the screen do not.
//
// ---- Threads --------------------------------------------------------------------------------------------------------
// bgfx runs in single-threaded mode on JUCE's message thread (gpu/BgfxContext.h): that is the one thread that draws.
// - Called on the message thread, renderGpuPicture() draws at once and returns, never waiting for another thread, so
//   it cannot deadlock on its own thread: also from inside a frame of the FramePump (a Panel's tick or draw, an
//   editor's callback). There, the editors that already submitted that frame are presented by the picture's own
//   bgfx::frame() calls and the others by the pump's, as usual.
// - Called on any other thread, it posts the work to the message thread (juce::MessageManager::callAsync) and waits at
//   most options.timeoutMs. timedOut means the message thread did not run it in time (it is blocked, busy, or not
//   dispatching messages, as the main thread of a console program that never runs its loop); a job that timed out
//   before it started is dropped and never draws (GpuPictureCounters::abandoned). A thread that holds a
//   juce::MessageManagerLock gets timedOut at once (the message thread is waiting for that lock).
// - noMessageThread: there is no juce::MessageManager in the process (or it is shutting down).
//
// ---- The context ----------------------------------------------------------------------------------------------------
// - Editors open (BgfxContext::valid()): the picture draws on their bgfx, into its own target and on two views of its
//   own (bgfx's last two, caps->limits.maxViews - 2 and - 1; an editor's are 0..BgfxContext::kMaxWindows), between
//   their frames or inside one. Nothing of theirs changes: both views draw into the picture's target, never an editor's
//   drawable.
// - No window in the process: the first renderGpuPicture() starts bgfx headless (BgfxContext::acquire(nullptr, 0, 0):
//   Metal with no swapchain and no window; GpuPicture::startedContext) and holds it, so later pictures are fast. An
//   editor that opens meanwhile takes bgfx over: BgfxContext starts it again on the editor's window (a window never
//   draws behind a headless bgfx, which has no swapchain to present it with: gpu/BgfxContext.h), the hold stays
//   registered behind it, and bgfx goes back to headless when the last editor closes while the hold lasts. Each such
//   restart builds custom programs and picture textures again at their next draw, as after any restart.
//   releaseGpuPictureContext() lets the hold go (bgfx shuts down unless an editor holds it; with an editor open nothing
//   restarts), and so does JUCE's shutdown (the hold is a juce::DeletedAtShutdown, deleted by the last
//   ScopedJuceInitialiser_GUI, HeadlessGuiScope's included). The hold takes none of BgfxContext's window slots
//   (BgfxContext::Config::maxWindows counts windows only; v0.23.1: in v0.23.0 it took one while it lasted).
//
// ---- Where there is no GPU picture: SoftRaster (the stated fallback) -----------------------------------------------
// - Linux: unsupported (gpuPicturesSupported() is false). FunkGui::gpu draws there on Vulkan, but no read-back is
//   implemented or tested; nothing is drawn and bgfx is not touched.
// - initFailed: bgfx could not start (no Metal device, as in a VM).
// - Without FunkGui::gpu (FUNKGUI_WITH_BGFX=OFF, FUNKGUI_WITH_JUCE=OFF, the web) this header is not available; a
//   product built both ways tests FUNKGUI_HAS_BGFX, which FunkGui::gpu defines to 1.
// In each case, and on any other status, the product draws the frame with SoftRaster:
//
//   funkgui::Image img;
//   if (funkgui::GpuPicture pic = funkgui::renderGpuPicture(list); pic.ok())
//       img = std::move(pic.image);
//   else
//       img = funkgui::rasterise(list, funkgui::FontService::get().atlas());   // shader draws as placeholders

#include <funkgui/canvas/PrimList.h>
#include <funkgui/canvas/SoftRaster.h>

#include <cstdint>

namespace funkgui
{
    struct GpuPicture
    {
        enum class Status : uint8_t
        {
            ok,                  // image holds the frame
            unsupported,         // no GPU picture on this platform (Linux): SoftRaster
            noMessageThread,     // no juce::MessageManager (or it is shutting down): nothing drew
            timedOut,            // the message thread did not run it within timeoutMs: nothing drew for this call
            initFailed,          // bgfx could not start headless (no Metal device): SoftRaster
            badSize,             // the frame's device size is under 1 px or over the GPU's texture limit (or 16384)
            overflow,            // BgfxSink dropped the frame: bgfx's transient vertex buffer is full this frame
            failed               // bgfx refused the render target, or the read never completed
        };

        Status   status = Status::unsupported;
        Image    image;                  // empty unless ok
        bool     startedContext = false; // this call started bgfx headless (no window in the process; see above)
        uint32_t drawCalls = 0;          // BgfxSink::lastDrawCalls() of the frame (ok only)

        bool ok() const noexcept { return status == Status::ok; }
    };

    struct GpuPictureOptions
    {
        // How long a call from a thread other than the message thread waits for it, in ms (clamped to 1..600000).
        // The message thread itself never waits.
        int timeoutMs = 2000;
    };

    // Draws `list` offscreen with bgfx and reads it back (see above). Any thread; copies the list when it posts.
    GpuPicture renderGpuPicture(const PrimList& list, const GpuPictureOptions& options = {});

    // Whether this platform has GPU pictures at all: true on macOS (Metal), false on Linux. A true answer does not
    // promise a GPU (renderGpuPicture can still say initFailed).
    bool gpuPicturesSupported() noexcept;

    // Lets the headless context go (see "The context"); nothing to do while the picture holds none. Any thread: from
    // another one it is posted to the message thread and waits at most timeoutMs (clamped as above). True when no hold
    // remains afterwards.
    bool releaseGpuPictureContext(int timeoutMs = 2000);

    // Whether the picture holds a headless context now. Any thread.
    bool gpuPictureContextHeld() noexcept;

    // Diagnostics and tests, since the process started: pictures drawn (status ok), and jobs posted from another thread
    // that the message thread reached after their caller had timed out (dropped there, never drawn).
    struct GpuPictureCounters
    {
        uint32_t drawn = 0;
        uint32_t abandoned = 0;
    };
    GpuPictureCounters gpuPictureCounters() noexcept;

    // The status as a word for logs: "ok", "unsupported", "noMessageThread", ...
    const char* toString(GpuPicture::Status) noexcept;
}

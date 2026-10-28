#include <funkgui/gpu/GpuPicture.h>

#include <funkgui/gpu/BgfxContext.h>
#include <funkgui/gpu/BgfxSink.h>

#include <bgfx/bgfx.h>
#include <juce_events/juce_events.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

// GPU pictures (v0.23.0; gpu/GpuPicture.h is the contract). The frame is BgfxSink::submit's, into an offscreen BGRA8
// render target on the picture's own view, copied by a blit on the next view into a read-back texture and read with
// bgfx::readTexture, as fg.shader.draw and fg.image.draw read their frames (bgfx in single-threaded mode renders a
// frame inside bgfx::frame(), and the read lands within two of them). Everything that touches bgfx runs on the message
// thread: inline when called there, else posted to it and waited for with a timeout. Choices the header leaves open:
// - The render target and the read-back texture are made and destroyed per picture (a picture is a tool's request, not
//   a frame of an animation); their destruction runs at bgfx's next frame, as every bgfx destroy does.
// - Both views draw into the picture's target, the blit view too: a view touched while it names the default backbuffer
//   opens a render pass on the primary editor's swapchain, which takes that window's next drawable (loaded, not
//   cleared) and presents it at the frame's end, a stale frame on screen until the editor's next one.
// - The picture's views are set back (no framebuffer, no clear) only after its frames ran: a view pointed back at the
//   default backbuffer before then would clear the primary editor's drawable with the picture's clear colour.
// - The headless hold stays registered while editors open and close (BgfxContext starts bgfx again on a window acquired
//   while it runs headless, and goes back to headless when the last window closes): the picture follows whatever bgfx
//   runs on.
// - BgfxContext::afterFrame() follows each of the picture's bgfx::frame() calls, as it follows the pump's: a view an
//   editor released is let go two frames after its release, whoever ran them.
// - The headless hold is BgfxContext's registration of the null window (BgfxContext::acquire(nullptr, 0, 0)), the one
//   FunkGui's GPU tests make too; a hold that something else released is noticed (owns(nullptr)) and forgotten.
// - Linux: gpuPicturesSupported() is false and renderGpuPicture() returns unsupported before touching bgfx or JUCE; the
//   code below compiles there all the same (it is plain bgfx and JUCE), so it cannot rot unseen.

namespace funkgui
{
    namespace
    {
        using Status = GpuPicture::Status;

        constexpr double kMaxSide = 16384.0;         // SoftRaster's limit: a larger frame is not a panel
        constexpr int    kMaxFrames = 8;             // bgfx::frame() calls the read may take (it lands after two)
        constexpr int    kMinTimeoutMs = 1, kMaxTimeoutMs = 600000;

        std::atomic<bool>     held{ false };         // the headless hold; written on the message thread only
        std::atomic<uint32_t> drawn{ 0 }, abandoned{ 0 };

        class HoldGuard;
        HoldGuard* guard = nullptr;                  // message thread only

        void dropHold();

        // The hold's end at JUCE's shutdown: DeletedAtShutdown::deleteAll(), from the last ScopedJuceInitialiser_GUI.
        class HoldGuard final : public juce::DeletedAtShutdown
        {
        public:
            HoldGuard() = default;
            HoldGuard(const HoldGuard&) = delete;
            HoldGuard& operator=(const HoldGuard&) = delete;

            ~HoldGuard() override
            {
                if (guard != this)
                    return;                          // dropHold() is deleting it
                guard = nullptr;
                dropHold();
            }
        };

        // Message thread.
        void dropHold()
        {
            if (held.exchange(false))
            {
                BgfxContext& ctx = BgfxContext::get();
                if (ctx.owns(nullptr))
                    ctx.release(nullptr);            // bgfx shuts down unless an editor holds it
            }
            if (guard != nullptr)
            {
                HoldGuard* g = guard;
                guard = nullptr;
                delete g;
            }
        }

        bool sizeOf(const PrimList& list, int& w, int& h)
        {
            const float dpiIn = list.info.dpi;
            const float dpi = std::isfinite(dpiIn) && dpiIn > 0.05f ? dpiIn : 1.0f;
            const double pw = std::round(static_cast<double>(list.info.logicalW) * static_cast<double>(dpi));
            const double ph = std::round(static_cast<double>(list.info.logicalH) * static_cast<double>(dpi));
            if (!(pw >= 1.0 && ph >= 1.0 && pw <= kMaxSide && ph <= kMaxSide))
                return false;
            w = static_cast<int>(pw);
            h = static_cast<int>(ph);
            return true;
        }

        // The picture, on the message thread.
        GpuPicture renderHere(const PrimList& list)
        {
            GpuPicture out;
            int w = 0, h = 0;
            if (!sizeOf(list, w, h))
            {
                out.status = Status::badSize;
                return out;
            }

            BgfxContext& ctx = BgfxContext::get();
            if (held.load() && !ctx.owns(nullptr))
                held.store(false);                   // let go by something else: nothing of ours to release
            if (!ctx.valid())
            {
                // No window in the process (or none that could start bgfx): bgfx headless, held for later pictures.
                if (!ctx.acquire(nullptr, 0, 0))
                {
                    out.status = Status::initFailed;
                    return out;
                }
                held.store(true);
                if (guard == nullptr)
                    guard = new HoldGuard();
                out.startedContext = true;
            }

            const bgfx::Caps* caps = bgfx::getCaps();
            constexpr uint64_t kNeeds = BGFX_CAPS_TEXTURE_BLIT | BGFX_CAPS_TEXTURE_READ_BACK;
            if (caps == nullptr || (caps->supported & kNeeds) != kNeeds || caps->limits.maxViews < 2u)
            {
                out.status = Status::failed;
                return out;
            }
            const auto maxSide = static_cast<int>(std::min<uint32_t>(caps->limits.maxTextureSize, 0xffffu));
            if (w > maxSide || h > maxSide)
            {
                out.status = Status::badSize;
                return out;
            }
            const auto drawView = static_cast<bgfx::ViewId>(caps->limits.maxViews - 2u);
            const auto blitView = static_cast<bgfx::ViewId>(caps->limits.maxViews - 1u);
            const auto tw = static_cast<uint16_t>(w), th = static_cast<uint16_t>(h);

            bgfx::TextureHandle colour = bgfx::createTexture2D(tw, th, false, 1, bgfx::TextureFormat::BGRA8,
                                                               BGFX_TEXTURE_RT);
            bgfx::TextureHandle readback = bgfx::createTexture2D(tw, th, false, 1, bgfx::TextureFormat::BGRA8,
                                                                 BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
            bgfx::FrameBufferHandle fb = BGFX_INVALID_HANDLE;
            if (bgfx::isValid(colour))
                fb = bgfx::createFrameBuffer(1, &colour, false);
            const auto destroyTargets = [&]
            {
                if (bgfx::isValid(fb)) bgfx::destroy(fb);
                if (bgfx::isValid(readback)) bgfx::destroy(readback);
                if (bgfx::isValid(colour)) bgfx::destroy(colour);
            };
            if (!bgfx::isValid(colour) || !bgfx::isValid(readback) || !bgfx::isValid(fb))
            {
                destroyTargets();                    // nothing was submitted on the views: nothing to set back
                out.status = Status::failed;
                return out;
            }

            // The frame, exactly as an editor submits it (its clear, its draw calls), then the copy and the read. An
            // overflow still touched the view (its clear), so the frames run whatever submit() said.
            BgfxSink sink;
            const BgfxSink::Result submitted = sink.submit(list, drawView, fb, w, h);
            bgfx::setViewFrameBuffer(blitView, fb);  // never the default backbuffer: the primary editor's drawable
            bgfx::touch(blitView);
            bgfx::blit(blitView, readback, 0, 0, colour);
            std::vector<uint8_t> bgra(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
            const uint32_t ready = bgfx::readTexture(readback, bgra.data());
            uint32_t now = 0;
            for (int i = 0; i < kMaxFrames && now < ready; ++i)
            {
                now = bgfx::frame();
                ctx.afterFrame();
            }

            bgfx::setViewFrameBuffer(drawView, BGFX_INVALID_HANDLE);
            bgfx::setViewFrameBuffer(blitView, BGFX_INVALID_HANDLE);
            bgfx::setViewClear(drawView, BGFX_CLEAR_NONE);
            destroyTargets();

            if (now < ready)
            {
                // Never seen: bgfx still owes the read, and would write into a freed buffer. The buffer is left to it.
                static_cast<void>(new std::vector<uint8_t>(std::move(bgra)));
                out.status = Status::failed;
                return out;
            }
            if (submitted == BgfxSink::Result::droppedOverflow)
            {
                out.status = Status::overflow;
                return out;
            }

            out.image.w = w;
            out.image.h = h;
            out.image.rgba.resize(bgra.size());
            for (size_t i = 0; i < bgra.size(); i += 4u)
            {
                out.image.rgba[i] = bgra[i + 2u];
                out.image.rgba[i + 1u] = bgra[i + 1u];
                out.image.rgba[i + 2u] = bgra[i];
                out.image.rgba[i + 3u] = 255u;       // opaque: the frame over its clear (gpu/GpuPicture.h)
            }
            out.drawCalls = sink.lastDrawCalls();
            out.status = Status::ok;
            drawn.fetch_add(1u);
            return out;
        }

        // ---- The message thread -------------------------------------------------------------------------------------

        enum class Ran : uint8_t { done, timedOut, noMessageThread };

        struct Call
        {
            std::function<void()> work;              // runs on the message thread at most once
            std::atomic<int> state{ 0 };             // 0 posted, 1 started, 2 abandoned by its caller
            juce::WaitableEvent done;
        };

        // Whether the calling thread is the message thread; `none` when the process has no message manager at all.
        bool onMessageThread(bool& none)
        {
            none = false;
            const juce::MessageManager* mm = juce::MessageManager::getInstanceWithoutCreating();
            if (mm == nullptr)
            {
                none = true;
                return false;
            }
            return mm->isThisTheMessageThread();
        }

        // From a thread that is not the message thread: posts work and waits at most timeoutMs.
        Ran post(std::function<void()> work, int timeoutMs)
        {
            juce::MessageManager* mm = juce::MessageManager::getInstanceWithoutCreating();
            if (mm == nullptr)
                return Ran::noMessageThread;
            if (mm->currentThreadHasLockedMessageManager())
                return Ran::timedOut;                // the message thread is waiting for this thread's lock
            auto call = std::make_shared<Call>();
            call->work = std::move(work);
            const bool posted = juce::MessageManager::callAsync([call]
            {
                int expected = 0;
                if (!call->state.compare_exchange_strong(expected, 1))
                {
                    abandoned.fetch_add(1u);         // its caller timed out first: dropped, never drawn
                    return;
                }
                call->work();
                call->done.signal();
            });
            if (!posted)
                return Ran::noMessageThread;         // the message manager is shutting down
            if (call->done.wait(static_cast<double>(std::clamp(timeoutMs, kMinTimeoutMs, kMaxTimeoutMs))))
                return Ran::done;
            int expected = 0;
            // Not started: it never will. Started: it finishes on its own, into the shared result nobody reads.
            static_cast<void>(call->state.compare_exchange_strong(expected, 2));
            return Ran::timedOut;
        }

        GpuPicture withStatus(Status s)
        {
            GpuPicture out;
            out.status = s;
            return out;
        }
    }

    bool gpuPicturesSupported() noexcept
    {
#if defined(__APPLE__)
        return true;
#else
        return false;                                // the stated fallback (gpu/GpuPicture.h): SoftRaster
#endif
    }

    GpuPicture renderGpuPicture(const PrimList& list, const GpuPictureOptions& options)
    {
        if (!gpuPicturesSupported())
            return withStatus(Status::unsupported);
        bool none = false;
        if (onMessageThread(none))
            return renderHere(list);                 // inline: never waits on its own thread
        if (none)
            return withStatus(Status::noMessageThread);

        auto copy = std::make_shared<PrimList>(list);    // the caller's list may be gone when the job runs
        auto result = std::make_shared<GpuPicture>();
        switch (post([copy, result] { *result = renderHere(*copy); }, options.timeoutMs))
        {
            case Ran::done:            return std::move(*result);
            case Ran::timedOut:        return withStatus(Status::timedOut);
            case Ran::noMessageThread: return withStatus(Status::noMessageThread);
        }
        return withStatus(Status::failed);
    }

    bool releaseGpuPictureContext(int timeoutMs)
    {
        if (!held.load())
            return true;
        bool none = false;
        if (onMessageThread(none))
            dropHold();
        else if (!none)
            static_cast<void>(post([] { dropHold(); }, timeoutMs));
        return !held.load();
    }

    bool gpuPictureContextHeld() noexcept { return held.load(); }

    GpuPictureCounters gpuPictureCounters() noexcept
    {
        GpuPictureCounters c;
        c.drawn = drawn.load();
        c.abandoned = abandoned.load();
        return c;
    }

    const char* toString(GpuPicture::Status s) noexcept
    {
        switch (s)
        {
            case Status::ok:              return "ok";
            case Status::unsupported:     return "unsupported";
            case Status::noMessageThread: return "noMessageThread";
            case Status::timedOut:        return "timedOut";
            case Status::initFailed:      return "initFailed";
            case Status::badSize:         return "badSize";
            case Status::overflow:        return "overflow";
            case Status::failed:          return "failed";
        }
        return "?";
    }
}

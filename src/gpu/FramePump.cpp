#include <funkgui/gpu/FramePump.h>
#include <funkgui/gpu/BgfxContext.h>
#include <funkgui/gpu/DisplayLink.h>

#include "ClientCadence.h"

#include <algorithm>
#include <bgfx/bgfx.h>

namespace funkgui
{
    namespace
    {
        void displayLinkStep(void* user, double timestampSec)
        {
            static_cast<FramePump*>(user)->tick(timestampSec);
        }
    }

    FramePump& FramePump::get()
    {
        // Deliberately never destroyed. A plugin binary is unloaded from the
        // host while other statics — including JUCE's MessageManager — are
        // being torn down in an order nothing here controls, and running a
        // juce::Timer destructor into that is a shutdown crash waiting to
        // happen. The pump releases its display link and stops its timer as
        // soon as the last editor leaves, so the only thing outliving the
        // process is the empty object itself.
        static FramePump* pump = new FramePump();
        return *pump;
    }

    FramePump::FramePump() = default;
    FramePump::~FramePump() = default;

    detail::ClientCadence* FramePump::cadenceOf(const FrameClient* c) const
    {
        for (const auto& s : clients_)
            if (s.client == c)
                return s.cadence.get();
        return nullptr;
    }

    void FramePump::add(FrameClient* c)
    {
        if (c == nullptr || cadenceOf(c) != nullptr) return;
        clients_.push_back({ c, std::make_unique<detail::ClientCadence>() });   // runs at the next tick
        updateClock();
    }

    void FramePump::remove(FrameClient* c)
    {
        clients_.erase(std::remove_if(clients_.begin(), clients_.end(), [c](const Slot& s) { return s.client == c; }),
                       clients_.end());

        // The link is bound to one client's view. If that client is going
        // away, the link has to be rebuilt on a surviving one or it dies with
        // a dangling view.
        if (linkOwner_ == c)
        {
            destroyDisplayLink(link_);
            link_ = nullptr;
            linkOwner_ = nullptr;
            ratePref_ = -1;
        }
        updateClock();
    }

    // A link on the first client whose view is on screen (or, when none is, on any view in a window: it starts firing
    // once that window shows, and the watchdog moves it if another shows first).
    bool FramePump::bindLink(bool onScreenOnly)
    {
        for (const auto& s : clients_)
        {
            void* v = s.client->frameClockView();
            if (v == nullptr || (onScreenOnly && !viewOnScreen(v)))
                continue;
            link_ = createDisplayLink(v, displayLinkStep, this);
            if (link_ != nullptr)
            {
                linkOwner_ = s.client;
                return true;
            }
        }
        return false;
    }

    void FramePump::updateClock()
    {
        if (clients_.empty())
        {
            destroyDisplayLink(link_);
            link_ = nullptr;
            linkOwner_ = nullptr;
            ratePref_ = -1;
            haveStamp_ = false;
            stopTimer();
            watchdog_.stopTimer();
            return;
        }

        const bool hadLink = (link_ != nullptr);
        if (link_ == nullptr && !forceTimer_)
            if (!bindLink(true))
                bindLink(false);
        if (!watchdog_.isTimerRunning())
        {
            lastTickWall_ = juce::Time::getMillisecondCounterHiRes() * 0.001;
            watchdog_.startTimerHz(4);
        }
        // The fallback timer and the display link both report uptime seconds,
        // but the link reports a *target presentation* time slightly ahead of
        // now. Dropping the stamp on a source change keeps that step out of
        // the first frame's delta.
        if (hadLink != (link_ != nullptr)) haveStamp_ = false;

        // The fallback runs only while there is no link — during the window
        // between the editor being constructed and its view reaching a screen,
        // or permanently if the GPU path never comes up. It honours the same
        // idle/full cadence the link does: an editor stuck on the no-GPU
        // screen used to be ticked sixty times a second forever. A capture run
        // (forceTimer_) keeps the full rate so dumps land promptly.
        if (link_ != nullptr) stopTimer();
        else if (!isTimerRunning())
            startTimerHz(static_cast<int>((forceTimer_ || ratePref_ == 1) ? kFullHz : kIdleHz));
    }

    void FramePump::requestRate(bool fullRate)
    {
        const int want = fullRate ? 1 : 0;
        if (want == ratePref_) return;
        ratePref_ = want;
        // A panel with nothing moving still needs to poll hover and follow
        // host automation, but it does not need 120 redraws a second of an
        // identical frame.
        if (link_ != nullptr)
        {
            if (fullRate) setDisplayLinkRate(link_, 30.0f, 120.0f, kFullHz);
            else          setDisplayLinkRate(link_, 8.0f,  kIdleHz, kIdleHz);
        }
        else if (isTimerRunning() && !forceTimer_)
        {
            startTimerHz(static_cast<int>(fullRate ? kFullHz : kIdleHz));
        }
    }

    void FramePump::forceFallbackClock()
    {
        forceTimer_ = true;
        destroyDisplayLink(link_);
        link_ = nullptr;
        linkOwner_ = nullptr;
        ratePref_ = -1;
        updateClock();
    }

    void FramePump::nudgeFullRate()
    {
        for (auto& s : clients_)
            s.cadence->nudge();
        requestRate(true);
    }

    void FramePump::nudgeFullRate(const FrameClient* c)
    {
        if (auto* cadence = cadenceOf(c))
            cadence->nudge();
        requestRate(true);
    }

    float FramePump::measuredFps(const FrameClient* c) const
    {
        const auto* cadence = cadenceOf(c);
        return cadence != nullptr ? cadence->fps() : 0.0f;
    }

    bool FramePump::wantedFullRate(const FrameClient* c) const
    {
        const auto* cadence = cadenceOf(c);
        return cadence != nullptr && cadence->fullRate();
    }

    void FramePump::releaseClockFor(const FrameClient* c)
    {
        if (linkOwner_ != c) return;
        destroyDisplayLink(link_);
        link_ = nullptr;
        linkOwner_ = nullptr;
        ratePref_ = -1;
        updateClock();   // rebind to another client, or fall back to the timer
    }

    void FramePump::timerCallback()
    {
        tick(juce::Time::getMillisecondCounterHiRes() * 0.001);
    }

    void FramePump::watch()
    {
        if (link_ == nullptr || clients_.empty() || inTick_)
            return;                          // the timer drives the frames, or nothing is open
        const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
        if (now - lastTickWall_ < 0.25)
            return;                          // firing
        // Quiet for a quarter of a second: its view is off screen (or the whole host is). Move it to a client whose
        // view is on screen, if one is; with none, nothing needs drawing and the link stays where it is.
        const FrameClient* owner = linkOwner_;
        bool another = false;
        for (const auto& s : clients_)
            if (s.client != owner && s.client->frameClockView() != nullptr && viewOnScreen(s.client->frameClockView()))
                another = true;
        if (!another)
            return;
        destroyDisplayLink(link_);
        link_ = nullptr;
        linkOwner_ = nullptr;
        ratePref_ = -1;
        haveStamp_ = false;
        ++rebinds_;
        updateClock();
        lastTickWall_ = now;                 // give the new link its quarter second
    }

    void FramePump::tick(double timestampSec)
    {
        if (inTick_) return;                 // re-entrancy guard
        if (clients_.empty()) { updateClock(); return; }

        float dt = 1.0f / 60.0f;
        if (haveStamp_)
            dt = static_cast<float>(timestampSec - lastStampSec_);
        lastStampSec_ = timestampSec;
        haveStamp_ = true;
        // A hidden window, a paused link or a debugger breakpoint can leave a
        // huge gap; clamping keeps every easing from jumping to its target.
        dt = juce::jlimit(1.0f / 1000.0f, 0.1f, dt);

        ++fpsCount_;
        if (timestampSec - fpsWindowStart_ >= 1.0)
        {
            fps_ = static_cast<float>(fpsCount_ / (timestampSec - fpsWindowStart_));
            fpsCount_ = 0;
            fpsWindowStart_ = timestampSec;
        }

        lastTickWall_ = juce::Time::getMillisecondCounterHiRes() * 0.001;
        inTick_ = true;
        bool submitted = false;

        // Each client its own cadence (0.25.0, ClientCadence.h): while the
        // pump is at full rate a calm client runs only at its own idle tick,
        // with the time since its last frame as its dt. While the pump idles
        // (its ticks are the idle cadence) or runs on its pinned clock (a
        // capture), every client runs at every tick, as before.
        static_assert(!(detail::ClientCadence::kIdleHz < static_cast<double>(kIdleHz))
                      && !(static_cast<double>(kIdleHz) < detail::ClientCadence::kIdleHz)); // equal, without -Wfloat-equal
        const bool everyTick = ratePref_ != 1 || forceTimer_;

        // Iterate a snapshot: a client destroyed from inside submitFrame would
        // otherwise invalidate the iterator. A client added during this tick
        // runs at the next.
        tickClients_.clear();
        for (const auto& s : clients_)
            tickClients_.push_back(s.client);
        for (std::size_t i = 0; i < tickClients_.size(); ++i)
        {
            FrameClient* c = tickClients_[i];
            auto* cadence = cadenceOf(c);
            if (cadence == nullptr)
                continue;                    // removed during this tick
            if (!cadence->tick(static_cast<double>(dt), everyTick))
                continue;                    // calm, between its own frames
            const auto r = c->submitFrame(cadence->begin(timestampSec));
            submitted |= r.submitted;
            if (auto* still = cadenceOf(c))  // not removed by its own frame
                still->end(r.wantsFullRate);
        }
        inTick_ = false;

        bool fullRate = false;
        for (const auto& s : clients_)
            fullRate = fullRate || s.cadence->fullRate();

        // Exactly one present per frame for every editor in the process.
        // Skipped entirely when nothing drew, so a minimised host window, or
        // every editor leaving its last picture up (0.25.0), costs nothing.
        // A frame is presented before bgfx::frame() returns (v0.25.1,
        // BgfxReset.h), so the last one drawn is on screen however long the
        // next waits; before, bgfx presented it at the start of the next.
        // bgfx presents only the windows whose views drew this frame (Metal
        // takes a drawable, Direct3D 11 and Vulkan flag a present, only for a
        // framebuffer a submitted view renders into), so an editor that
        // skipped keeps its picture while another draws (on macOS and
        // Windows: Linux draws every frame, NativeSurface.h
        // renderViewKeepsFrame), and so does one that did not run at this
        // tick (a calm editor between its own frames, 0.25.0; on Linux too,
        // which redraws it at its next frame as when it idles alone). A frame
        // also runs when bgfx has work waiting for one
        // (BgfxContext::framePending): with nothing submitted it draws and
        // presents nothing, and lets go of a closed editor's view.
        auto& ctx = BgfxContext::get();
        if ((submitted || ctx.framePending()) && ctx.valid())
        {
            bgfx::frame();
            ++framesRun_;
            ctx.afterFrame();                // views closed two frames ago are let go (BgfxContext.h, v0.21.0)
        }

        if (link_ == nullptr) updateClock();   // a view may have appeared
        requestRate(fullRate);
    }
}

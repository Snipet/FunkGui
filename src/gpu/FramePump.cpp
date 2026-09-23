#include "FramePump.h"
#include "BgfxContext.h"
#include "DisplayLink.h"

#include <algorithm>
#include <bgfx/bgfx.h>

namespace hrvbgui
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

    void FramePump::add(FrameClient* c)
    {
        if (c == nullptr) return;
        if (std::find(clients_.begin(), clients_.end(), c) != clients_.end()) return;
        clients_.push_back(c);
        updateClock();
    }

    void FramePump::remove(FrameClient* c)
    {
        clients_.erase(std::remove(clients_.begin(), clients_.end(), c), clients_.end());

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
            return;
        }

        const bool hadLink = (link_ != nullptr);
        if (link_ == nullptr && !forceTimer_)
        {
            for (auto* c : clients_)
            {
                if (void* v = c->frameClockView())
                {
                    link_ = createDisplayLink(v, displayLinkStep, this);
                    if (link_ != nullptr) { linkOwner_ = c; break; }
                }
            }
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
        requestRate(true);
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

        inTick_ = true;
        bool submitted = false, fullRate = false;

        // Iterate a snapshot: a client destroyed from inside submitFrame would
        // otherwise invalidate the iterator.
        const auto snapshot = clients_;
        for (auto* c : snapshot)
        {
            if (std::find(clients_.begin(), clients_.end(), c) == clients_.end())
                continue;                    // removed during this tick
            const auto r = c->submitFrame(dt);
            submitted |= r.submitted;
            fullRate  |= r.wantsFullRate;
        }
        inTick_ = false;

        // Exactly one present per frame for every editor in the process.
        // Skipped entirely when nothing drew, so a minimised host window costs
        // nothing; bgfx defers its destroy queue until the next frame, which
        // is harmless.
        if (submitted && BgfxContext::get().valid())
            bgfx::frame();

        if (link_ == nullptr) updateClock();   // a view may have appeared
        requestRate(fullRate);
    }
}

#pragma once

// Which of several views is shown, with a crossfade and a dwell (02 §5.7; HR's view/target/shown/amount/dwell,
// BgfxEditor.cpp:983-1019, made generic). A tab click pins a view; a drag holds the dragged control's view for the
// drag; a wheel, key or a11y write touches a view, which then stays for `dwell` seconds; hover never switches views.
// The crossfade amount eases with tau through funkgui::ease and snaps within 1e-3, so it settles exactly with a fixed
// dt. Draw outgoing() at 1 - amount() and incoming() at amount().
//
// Declared in G2 (v0.2.0, frozen at FZ1). A template, so the implementing card (G6) adds the member bodies to this
// header.
//
// G6's choices where 02 is silent (fg.dwell pins them):
// - The target is the held view, else the touched view while its dwell lasts, else the pinned view. incoming() is the
//   target from the moment a call changes it (input goes to the target at once, 02 §7.1); tick() only moves time.
// - Releasing a hold (hold(nullopt)) touches the released view, so it stays for the dwell after the drag (HR's
//   touchView on mouseUp, :1854). A hold cancels a pending touch; pin() cancels a pending touch too (a tab click is an
//   explicit choice) but not a hold. The dwell counts down only while nothing is held (HR: not while dragging), by dt
//   per tick, and ends when it reaches 0.
// - A new target starts a crossfade from the view that currently dominates (amount 0 → 1). Going back to the view
//   that is fading out reverses the fade from where it is (amount → 1 - amount), so the picture never jumps.
//   pin(v, true) cuts: incoming = outgoing = target, amount 1.
// - settled() is amount() == 1 and no dwell pending (a held view at amount 1 is settled: nothing moves by itself).
// - dt <= 0 or NaN moves nothing. V needs copy and ==; the members are defined below, and DwellSelector<int>
//   (ScreenFader) is instantiated once in src/widgets/DwellSelector.cpp.

#include <funkgui/core/Ease.h>

#include <optional>
#include <utility>

namespace funkgui
{
    template <class V>
    class DwellSelector
    {
    public:
        explicit DwellSelector(V pinned, float tau = 0.18f, float dwell = 0.9f);

        void  pin(V, bool instant = false);      // tab click
        void  hold(std::optional<V>);            // while dragging (nullopt releases)
        void  touch(V);                          // wheel/key/a11y write: hold V for `dwell` s. Hover never calls this.
        void  tick(float dt);                    // amount eases with tau, snaps within 1e-3
        V     incoming() const;
        V     outgoing() const;
        float amount() const;                    // draw outgoing at 1-amt, incoming at amt
        bool  settled() const;                   // amount() == 1 and no dwell pending (G2 addition, for wantsFullRate)

        // ---- G6 additions (additive API) -------------------------------------------------------------------------
        V     pinned() const { return pinned_; }
        std::optional<V> held() const { return held_; }
        float dwellLeft() const noexcept { return touched_ ? touchLeft_ : 0.0f; }   // seconds; 0 when none pending

    private:
        // Private state: completed by the implementing card (G6); not part of the frozen API.
        V     target() const;
        void  retarget(bool instant);

        V                pinned_;
        V                incoming_;
        V                outgoing_;
        std::optional<V> held_;
        std::optional<V> touched_;
        float            touchLeft_ = 0.0f;
        float            amount_ = 1.0f;
        float            tau_;
        float            dwell_;
    };

    using ScreenFader = DwellSelector<int>;      // pin() only; tau 0.12 s (§7.1)

    inline constexpr float kScreenFadeTau = 0.12f;   // ScreenFader(screen, kScreenFadeTau) (02 §7.1; G6 addition)

    // ---- members ----------------------------------------------------------------------------------------------------

    template <class V>
    DwellSelector<V>::DwellSelector(V pinned, float tau, float dwell)
        : pinned_(pinned), incoming_(pinned), outgoing_(pinned), tau_(tau), dwell_(dwell)
    {
    }

    template <class V>
    V DwellSelector<V>::target() const
    {
        if (held_)
            return *held_;
        if (touched_)
            return *touched_;
        return pinned_;
    }

    template <class V>
    void DwellSelector<V>::retarget(bool instant)
    {
        const V t = target();
        if (instant)
        {
            incoming_ = t;
            outgoing_ = t;
            amount_ = 1.0f;
            return;
        }
        if (t == incoming_)
            return;
        if (t == outgoing_)
        {
            std::swap(incoming_, outgoing_);             // back to the view fading out: reverse from where it is
            amount_ = 1.0f - amount_;
            return;
        }
        if (amount_ >= 0.5f)
            outgoing_ = incoming_;                       // else the outgoing view still dominates and stays
        incoming_ = t;
        amount_ = 0.0f;
    }

    template <class V>
    void DwellSelector<V>::pin(V v, bool instant)
    {
        pinned_ = v;
        touched_.reset();
        touchLeft_ = 0.0f;
        retarget(instant);
    }

    template <class V>
    void DwellSelector<V>::hold(std::optional<V> v)
    {
        if (!v && held_)
        {
            touched_ = held_;                            // released: the view stays for a dwell (HR :1854)
            touchLeft_ = dwell_;
        }
        else if (v)
        {
            touched_.reset();
            touchLeft_ = 0.0f;
        }
        held_ = v;
        retarget(false);
    }

    template <class V>
    void DwellSelector<V>::touch(V v)
    {
        touched_ = v;
        touchLeft_ = dwell_;
        retarget(false);
    }

    template <class V>
    void DwellSelector<V>::tick(float dt)
    {
        if (touched_ && !held_ && dt > 0.0f)
        {
            touchLeft_ -= dt;
            if (!(touchLeft_ > 0.0f))
            {
                touched_.reset();
                touchLeft_ = 0.0f;
            }
        }
        retarget(false);
        amount_ = ease::toward(amount_, 1.0f, dt, tau_);
    }

    template <class V>
    V DwellSelector<V>::incoming() const
    {
        return incoming_;
    }

    template <class V>
    V DwellSelector<V>::outgoing() const
    {
        return outgoing_;
    }

    template <class V>
    float DwellSelector<V>::amount() const
    {
        return amount_;
    }

    template <class V>
    bool DwellSelector<V>::settled() const
    {
        return ease::sameBits(amount_, 1.0f) && !touched_;
    }

    extern template class DwellSelector<int>;    // ScreenFader: src/widgets/DwellSelector.cpp
}

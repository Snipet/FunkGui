#pragma once

// Which of several views is shown, with a crossfade and a dwell (02 §5.7; HR's view/target/shown/amount/dwell,
// BgfxEditor.cpp:983-1019, made generic). A tab click pins a view; a drag holds the dragged control's view for the
// drag; a wheel, key or a11y write touches a view, which then stays for `dwell` seconds; hover never switches views.
// The crossfade amount eases with tau through funkgui::ease and snaps within 1e-3, so it settles exactly with a fixed
// dt. Draw outgoing() at 1 - amount() and incoming() at amount().
//
// Declared in G2 (v0.2.0, frozen at FZ1). A template, so the implementing card (G6) adds the member bodies to this
// header.

#include <optional>

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

    private:
        // Private state: completed by the implementing card (G6); not part of the frozen API.
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
}

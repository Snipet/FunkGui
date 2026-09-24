#pragma once

// The first-run hint, then the persistent spec line (02 §5.8; HR BgfxEditor.cpp:1556-1586). There are no tooltips
// anywhere: the footer line says what is under the hand. The hint shows for `seconds`, and the first pointer move cuts
// what is left to 0.4 s (HR :1668). A mouse-down with no mouse-move ever seen (a touch screen or a tablet) switches to
// always-chrome, where every slot shows its hover chrome (HR :1488). Probes start without the hint (02 §3.7 rule 6).
//
// Declared in G2 (v0.2.0, frozen at FZ1); implemented by G6 (src/widgets/HintLine.cpp).
//
// G6's choices where 02 is silent (fg.dwell's hint.* rows and fg.gallery.hint pin them):
// - tick(dt) counts the hint down by dt (dt <= 0 or NaN: nothing), whether or not the pointer ever arrives (HR: gating
//   it on a move held full rate forever on an untouched panel). The hint is active() while time is left, and
//   wantsFullRate() is active(): the hint's fade is the panel's only reason to redraw.
// - pointerMoved() is a mouse move as HR handles one: it notes the move (noteMove()) and cuts the hint to 0.4 s when
//   more is left. noteMove() alone only notes it (a Panel that sees moves it does not want to cut the hint with).
//   alwaysChrome() is "a down was noted and no move ever was" (HR's everSawMouseDown && !everSawMouseMove), so a move
//   after the down turns it off again.
// - draw(): while active, the hint in kLabel ink32 faded by min(1, left / 0.4) (colour only, so the geometry does not
//   move while it fades); else `spec` (nothing when null or empty) in kLabel ink32. Both are left-aligned at (x, y) (y
//   is the text top) and fitted to maxW with text::fitEllipsis. Tagged HINT; never live.

namespace funkgui
{
    class Canvas;
    struct Theme;

    class HintLine
    {
    public:
        explicit HintLine(const char* firstRun, float seconds = 6.0f);

        void tick(float dt);
        void pointerMoved();                     // cuts the remaining hint to 0.4 s (HR :1668)
        void noteDown();                         // mouseDown with no mouseMove ever -> always-chrome (HR :1488)
        void noteMove();
        void skip();                             // no hint at all (probes and captures, 02 §3.7 rule 6)
        bool active() const;
        bool wantsFullRate() const;
        bool alwaysChrome() const;
        void draw(Canvas&, const Theme&, float x, float y, float maxW, const char* spec) const;   // kLabel ink32

        // ---- G6 additions (additive API) -------------------------------------------------------------------------

        float remaining() const noexcept { return left_; }   // seconds of hint left (0 once expired or skipped)
        float alpha() const noexcept;                         // the hint's fade, min(1, remaining / 0.4)

        static constexpr float kCut = 0.4f;      // what the first move leaves of the hint, and its fade-out time (HR)

    private:
        // Private state: completed by the implementing card (G6); not part of the frozen API.
        const char* firstRun_;
        float       left_;
        bool        moved_ = false;              // a mouse move was ever noted
        bool        downSeen_ = false;           // a mouse down was ever noted
    };
}

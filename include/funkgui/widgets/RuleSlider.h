#pragma once

// HR's slot (a label, a value, a 1 px rule with a caret), plus detents and the five states (02 §5.4, §8.1). It renders
// a ValueModel and writes only through a GestureController.
//
// Input rules (02 §5.4): continuous drag in track space, 240 px per full track (Shift 1200, Cmd 6000), right or up
// increases, re-anchored when a modifier toggles; stepped drag p = clamp(240/(n-1), 24, 64) px per detent,
// committing at 0.5·p + 6 px of travel, one gesture for the whole drag, with a ghost caret; a detent-label click arms
// on down and commits through writeDetent on up unless the pointer moved > 3 px; wheel in track space (step 0.025,
// Shift 0.005, non-smooth ×4) or one detent per notch; hybrid end cells entered 18 px past a range edge; locked,
// derived and n/a refuse every write (the right-click host menu still works). Keys: 02 §8.9.
//
// Declared in G2 (v0.2.0, frozen at FZ1); implemented by G5 (src/widgets/RuleSlider.cpp).
//
// G5's choices where 02 is silent (fg.ruleslider.input pins them):
// - The view is re-read (ValueModel::key()) in tick() and at the start of every input call; const calls (draw, a11y,
//   specLine, hit tests) use the view of the last re-read. The constructor reads it once, so a new slider draws at once.
// - Hybrid end cells are laid out in array order, left to right: endLo[0] is the leftmost (outermost) low cell and
//   endHi[nEndHi-1] the rightmost; activeEnd -k is endLo[k-1] and +k is endHi[k-1]. A primary hybrid's sub text starts
//   after the low cells, beside the continuous part.
// - Continuous and hybrid a11y is in track space {0, 1}, step 0.01 (the ValueView has no display range; a product
//   may rewrite lo/hi/v and translate setValue); stepped is index space {0, n-1}, step 1 (F §8.2).
// - The caret eases in logical px: HR's shown() (tau 90 ms, jumps over 0.15 of the track snap) for continuous slots,
//   toward() with tau 60 ms for detents and end cells, and toward() with tau 90 ms and no snap after flashLabel()
//   (the Mode-switch landing, 02 §8.7). `focused` counts as under the hand (hover chrome), like hovered and dragging.
// - flashLabel(s) marks a Mode-switch landing (the next caret move eases); s > 0 also flashes the label ink100 for s
//   seconds, so a Panel calls flashLabel(0.6f) on slots whose state, label or tag changed and flashLabel(0) on the rest.
// - Extension is the tag "+" (02 §8.1); a locked view with `clamped` set has its fixed value outside the track, so
//   no notch is drawn; a clamped view's text.sub ("CLAMPED FROM 5 µS") joins the spec line. ValueView::live marks the
//   sub text (and a derived slot's value and hollow caret) live, so geometry fingerprints leave telemetry out.
// - Marks hang below the rule (default and soft notches 1×3, detent and end-cell ticks and the locked notch 1×5); the
//   caret stands on it. Refused and display-only slots return false from wheel() (the host may scroll); keys aimed at
//   a refused slot return true and write nothing.

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/panel/Input.h>
#include <funkgui/widgets/ValueModel.h>

#include <cstddef>
#include <cstdint>

namespace funkgui
{
    class AttachedWord;
    class Canvas;
    class FontAtlasSdf;
    class GestureController;
    class HostServices;
    struct Theme;

    enum class SlotSize : uint8_t { primary, secondary };

    struct SlotGeom
    {
        float    x = 0.0f, top = 0.0f, w = 0.0f;
        SlotSize size = SlotSize::primary;

        constexpr bool isPrimary() const noexcept { return size == SlotSize::primary; }

        // {x-6, top-6, w+12, primary ? 80 : 66}
        constexpr Rect hit() const noexcept { return { x - 6.0f, top - 6.0f, w + 12.0f, isPrimary() ? 80.0f : 66.0f }; }
        // top+18 (kValueP 24) | top+14 (kValueS 18)
        constexpr float valueTop() const noexcept { return top + (isPrimary() ? 18.0f : 14.0f); }
        // top+46 | top+34 (kMicro: sub-readout | detent labels; clears the tallest caret, 02 §6.1)
        constexpr float subTop() const noexcept { return top + (isPrimary() ? 46.0f : 34.0f); }
        // top+64 | top+54
        constexpr float trackY() const noexcept { return top + (isPrimary() ? 64.0f : 54.0f); }
    };

    class RuleSlider
    {
    public:
        RuleSlider(ValueModel&, SlotGeom, uint32_t a11yId);

        void   setWord(AttachedWord*);                  // optional latch word, carved out of hit() (checked first)
        void   tick(float dt, bool hovered, bool focused, bool alwaysChrome);  // hover 90/160 ms, caret ease, flashes
        void   draw(Canvas&, const Theme&, bool focusRing) const;              // state table §8.1
        bool   contains(Point) const;
        int    detentLabelAt(Point) const;              // -1 unless stepped with labels drawn
        Cursor cursorAt(Point) const;
        void   pointerDown(const PointerEvent&, GestureController&, HostServices&);  // popup: host menu, never a write
        void   pointerDrag(const PointerEvent&, GestureController&);
        void   pointerUp(const PointerEvent&, GestureController&);
        void   doubleClick(GestureController&);         // Mode default
        bool   wheel(const WheelEvent&, GestureController&, double nowSec);
        bool   key(const KeyEvent&, GestureController&);           // §8.9
        void   a11yAction(A11yAction, double value, GestureController&);
        void   accessibility(A11yItem&) const;          // §8.9 roles
        void   specLine(char* out, size_t n) const;     // footer text for hover/focus/drag
        void   flashLabel(float seconds);               // Mode-switch landing (§8.7)
        const ValueView& view() const;

        const SlotGeom& geom() const noexcept { return geom_; }
        uint32_t a11yId() const noexcept { return a11yId_; }

        // ---- G5 additions (additive API, flagged in the S4.3 handoff) ------------------------------------------------

        // Pointer hover inside the slot (the Panel forwards pointerMove to the slider under the pointer): the hovered
        // detent label draws ink70 (02 §8.1). pointerExit clears it.
        void pointerMove(const PointerEvent&);
        void pointerExit();

        // True when nothing eases (hover, caret, label flash) and the model has no newer view: the Panel's
        // wantsFullRate() is the OR of !settled() over its widgets.
        bool settled() const;

        // Whether the stepped detent labels are drawn (the fit rule below, plus room for a tag moved to the detent
        // line); false for every other state.
        bool detentLabelsDrawn() const noexcept { return labelsDrawn_; }

        // The detent-label fit rule of 02 §8.3 for a stepped slot of width w: labels are drawn iff every adjacent pair
        // has (w_i + w_i+1)/2 + 2.5 <= w/n and the end labels stay within [x - 2, x + w + 2] (widths in kMicro). A
        // product's text-fit lint calls the same function the slider draws with.
        static bool detentLabelsFit(const FontAtlasSdf&, const Detent* detents, int n, float w) noexcept;

        static constexpr float kWheelNotch = 0.10f;    // smooth wheel delta per detent (stepped, hybrid cells)
        static constexpr float kEndCell    = 16.0f;    // hybrid end cell width (02 §8.1)
        static constexpr float kEndGap     = 4.0f;     // gap between the end cells and the continuous part

    private:
        // Private state: completed by the implementing card (G5); not part of the frozen API.
        enum class Drag : uint8_t { none, continuous, stepped, label, hybrid };
        enum class Write : uint8_t { tap, drag, wheel };

        void  refresh();                               // re-reads the view when key() changed; layout bits always
        bool  refused() const noexcept;                // locked, derived, n/a
        bool  hybrid() const noexcept;                 // continuous with end cells
        bool  tagOnSubLine() const noexcept;           // a tag and a visible word: the tag moves to the sub line
        int   nLo() const noexcept;
        int   nHi() const noexcept;
        float contLeft() const noexcept;               // the continuous part of the track, logical px
        float contRight() const noexcept;
        float cellCentre(int ord) const noexcept;      // hybrid ordinal: 0..nLo-1 low cells, nLo the range, then high
        const Detent* endCell(int ord) const noexcept; // nullptr for the range
        int   hybridOrdinal() const noexcept;
        float detentCentre(int i) const noexcept;
        int   activeDetent() const noexcept;
        float caretTarget() const noexcept;
        Rect  labelRect(int i) const noexcept;
        float travel(Point) const noexcept;            // pointer travel from down_, px (right or up increases)
        float cellTravel(int ord, float edge, float span) const noexcept;
        void  write(float host01, GestureController&, Write, double nowSec);
        void  writeDetentBy(int i, GestureController&, Write, double nowSec);
        void  writeHybrid(int ord, float t, GestureController&, Write, double nowSec);
        void  stepHybrid(int dir, float amount, GestureController&, Write, double nowSec);
        void  beginStepped(Point at, GestureController&, ParamPort&);
        void  finishDrag(GestureController&);
        int   notches(float v, bool smooth) noexcept;

        ValueModel&   model_;
        SlotGeom      geom_;
        uint32_t      a11yId_;
        AttachedWord* word_ = nullptr;
        ValueView     view_{};
        uint64_t      viewKey_ = 0;
        bool          viewValid_ = false;

        bool  labelsDrawn_ = false;                    // stepped labels drawn: the fit rule and room for a moved tag
        bool  fitRule_ = false;                        // detentLabelsFit() for the current view
        float firstLabelLeft_ = 0.0f;                  // the first detent label's left edge, relative to x
        float tagWidth_ = 0.0f;                        // the tag's width in kMicro
        float hover_ = 0.0f;                           // hover chrome 0..1 (90 ms in, 160 ms out)
        bool  hoverOn_ = false;                        // its target
        bool  alwaysChrome_ = false;
        int   hoverLabel_ = -1;                        // the detent label under the pointer
        float caretX_ = 0.0f;                          // the eased caret, logical px
        bool  landing_ = false;                        // the caret eases to a Mode switch's position (02 §8.7)
        float flash_ = 0.0f, flashHold_ = 0.0f;        // label flash amount, and its hold time left

        Drag  drag_ = Drag::none;
        Point down_{};                                 // pointer down (travel origin)
        float anchorS_ = 0.0f;                         // stepped: travel of the current detent's own position
        float edgeS_ = 0.0f;                           // continuous/hybrid: travel at which the track is 0
        float dragT_ = 0.0f;                           // the track value this drag last computed
        bool  fine_ = false, ultra_ = false;           // Shift / Cmd at the anchor
        int   dragDetent_ = -1;                        // stepped: detent; hybrid: ordinal
        float dragTravel_ = 0.0f;                      // stepped: travel from the current detent (ghost caret)
        int   armedLabel_ = -1;                        // detent label armed by pointer down
        float wheelAcc_ = 0.0f;                        // smooth wheel accumulation (stepped, hybrid cells and edges)
        double wheelLast_ = -1.0;                      // nowSec of the last wheel event
    };
}

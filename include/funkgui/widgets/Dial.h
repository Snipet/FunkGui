#pragma once

// A rotary control over a ValueModel (the RuleSlider contract of FC 02 §5.3–§5.4, §8.1 in a 60 x 80 cell): the same
// five states, the same writes through GestureController, the same keys (FC 02 §8.9) and a11y (slider; stepped in index
// space). The sweep is 270 degrees from 7:30 to 4:30 through 12:00; right-or-up travel increases.
//
// Input: drag travel = dx - dy, 240 px per full sweep (Shift 1200, Cmd 6000), re-anchored when a modifier toggles;
// stepped: p = clamp(240/(n-1), 24, 64) px per detent, committing at 0.5 p + 6 px, one gesture per drag, a ghost
// pointer; hybrid end steps sit on 20-degree end cells past each end of the sweep, entered 18 px past the edge; wheel
// and keys as RuleSlider; double-click and Delete write the Mode default; locked, derived and n/a refuse every write
// (the popup click still opens the host menu). Typed values are the product's (a field over valueBox()).
//
// FSynth G2 (FSynth docs/design/02-funkgui-and-ui.md §4.1, §14.1; the states table is §14.1's). Choices where 02 is
// silent (fg.dial.input and fg.modoverlay pin them):
// - Angles are screen angles as Canvas::arc takes them (radians, 0 = 3:00, clockwise): track position t sits at
//   135 + 270 t degrees, so 0 is 7:30, 0.5 is 12:00 and 1 is 4:30. A stepped view's detent i sits at its cell centre,
//   t = (i + 0.5) / n, which is where FCompressor's ValueView convention puts ValueView::track, so a product snaps a
//   ModOverlay end to a detent in the same space. Hybrid end cell k (1 = next to the sweep) is centred 20 (k - 0.5)
//   degrees past its end of the sweep; end cells are ordered as RuleSlider orders them (endLo[0] outermost, endHi[0]
//   innermost). A Dial takes at most two end cells a side: 4 x 20 degrees fill 80 of the 90-degree gap, and a third
//   cell a side would overlap the other side's at 6:00 (a fifth on one side lands on the other end of the sweep). The
//   Dial does not shrink or drop cells; a product with more end steps on a side uses a RuleSlider (16 px cells, any
//   number).
// - Radial marks sit just outside the track, centred at radius() + 3 (18 px primary, 14 compact): the Mode-default
//   notch and soft notches 1 x 3 ink32, detent ticks, end-cell ticks and the locked notch 1 x 5 (ink32, active
//   ink100). Detent ticks are drawn only when a cell is at least 4 px of track arc (radius() x 270 degrees / n). The
//   pointer is a 2 px radial segment from r 9 to r 17 (primary; compact r 5 to r 13), ink100; the ghost pointer the
//   same in 1 px ink32; a derived dial's hollow pointer a disc of r 3 with a 1 px ink52 ring, on the track.
// - The hover fill is a 4 px accentDim arc, premixed over the ground at the hover amount, under the track and value
//   arc: from the sweep start (bipolar continuous: 12:00) to the pointer; stepped: to the active detent; none in an
//   end cell. The value arc (continuous, inside the range) is ink70, mixed to accent by the hover amount; 12:00 starts
//   it when bipolar. Both end at the pointer's angle held to the sweep, so while the pointer eases out of an end cell
//   neither is drawn in the gap. alwaysChrome draws the track ink32.
// - Text is centred on the cell: the label (kCaption at top; compact kMicro at top + 38), the value and its unit on one
//   kMicro line at top + 56, and the sub line at top + 68: "CLAMPED" for a clamped continuous view, else the view's tag
//   in every state but n/a. Compact dials draw the label only. A derived view's value and hollow pointer are live
//   primitives when ValueView::live is set, as RuleSlider's. A derived dial's LINKED box (FC ADR-74: radius 4, a 1 px
//   ink32 border, ink16 at 35 % premixed over the ground) and the focus ring take the hit rect grown 2 px up and down,
//   {x + 2, top - 2, w - 4, h}: 02 §14.1 names the hit rect, but the label starts at its top edge and the sub line at
//   top + 68 runs 2 px below it, so a box on the hit rect itself would cross the text. contains() and the a11y bounds
//   stay the hit rect.
// - The view is re-read (ValueModel::key()) in tick() and at the start of every input call, and so is the overlay
//   (ModOverlay::key); const calls use the copies of the last re-read. The pointer eases as RuleSlider's caret does
//   (in angle: tau 90 ms, jumps over 0.15 of the sweep snap; tau 60 ms onto detents and end cells; a Mode-switch
//   landing eases tau 90 ms without the snap). A drag sticks to a soft notch (ValueView::notches, drawn as 1 x 3
//   marks) within 1/60 of the sweep, 4 of the 240 px a full sweep takes, whatever the modifiers.
// - The cursor is upDown over a writable dial (a port, continuous or stepped), grab over ringContains() when an
//   overlay with routes is set on a continuous or stepped dial, else normal. ringContains() and the RuleSlider's
//   bandContains() are geometry only: the product decides whether a press there starts a depth edit.
// - valueBox(): primary {x + 2, top + 54, 56, 14} (02 §14.5); compact {x + 2, top + 36, 50, 14}, over the label.
//
// Linux, the JUCE-free builds and a web host: widgets code, platform-neutral and JUCE-free; "Cmd" is Mods::cmd (Ctrl
// on Linux), the popup click is the host's popup rule, and a web host shows Cursor::upDown as ns-resize and grab as
// grab. Sines and cosines are taken in double and rounded once to float, so every platform records the same points.

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/panel/Input.h>
#include <funkgui/widgets/ModOverlay.h>
#include <funkgui/widgets/ValueModel.h>

#include <cstddef>
#include <cstdint>

namespace funkgui
{
    class Canvas;
    class GestureController;
    class HostServices;
    struct Theme;

    enum class DialSize : uint8_t { primary, compact };

    struct DialGeom
    {
        float    x = 0.0f, top = 0.0f;
        DialSize size = DialSize::primary;

        constexpr bool  isPrimary() const noexcept { return size == DialSize::primary; }
        constexpr float w() const noexcept { return isPrimary() ? 60.0f : 54.0f; }        // the cell
        constexpr float h() const noexcept { return isPrimary() ? 80.0f : 52.0f; }
        constexpr Rect  hit() const noexcept { return { x + 2.0f, top, w() - 4.0f, h() - 4.0f }; }   // 4 px gutters
        constexpr Point centre() const noexcept { return { x + w() * 0.5f, top + (isPrimary() ? 34.0f : 20.0f) }; }
        constexpr float radius() const noexcept { return isPrimary() ? 15.0f : 11.0f; }      // track arc
        constexpr float ringRadius() const noexcept { return radius() + 5.0f; }             // ModOverlay band
        constexpr float labelTop() const noexcept { return isPrimary() ? top : top + 38.0f; }   // kCaption | kMicro
        constexpr float valueTop() const noexcept { return top + 56.0f; }                   // kMicro, primary only
        constexpr float subTop() const noexcept { return top + 68.0f; }                     // kMicro, primary only
    };

    class Dial
    {
    public:
        Dial(ValueModel&, DialGeom, uint32_t a11yId);

        void   setMod(const ModOverlay*);               // nullptr: no overlay (the default)
        void   tick(float dt, bool hovered, bool focused, bool alwaysChrome);
        void   draw(Canvas&, const Theme&, bool focusRing) const;
        bool   contains(Point) const;
        bool   ringContains(Point) const;               // the annulus radius()+2 .. ringRadius()+4, for depth drags
        Cursor cursorAt(Point) const;                   // upDown when writable, else normal
        void   pointerDown(const PointerEvent&, GestureController&, HostServices&);
        void   pointerDrag(const PointerEvent&, GestureController&);
        void   pointerUp(const PointerEvent&, GestureController&);
        void   doubleClick(GestureController&);
        bool   wheel(const WheelEvent&, GestureController&, double nowSec);
        bool   key(const KeyEvent&, GestureController&);
        void   a11yAction(A11yAction, double value, GestureController&);
        void   accessibility(A11yItem&) const;
        void   specLine(char* out, size_t n) const;
        void   flashLabel(float seconds);               // Mode-switch landing (FC 02 §8.7; FSynth 02 §13.4)
        bool   settled() const;
        const ValueView& view() const;

        Rect   valueBox() const;                        // where a typed-value field opens (primary: the value line)
        bool   takesTypedKeys() const;                  // continuous or stepped, with a port

        const DialGeom& geom() const noexcept { return geom_; }
        // 0.18.0: moves the dial (a resizable Panel's layout, Panel.h "Resizing"). Everything reads the geometry when it
        // draws or hit-tests, so the next frame and the next event see it; a drag in progress goes on.
        void setGeom(const DialGeom& g) noexcept { geom_ = g; }
        uint32_t a11yId() const noexcept { return a11yId_; }

        static constexpr float kSweepDeg = 270.0f;
        static constexpr float kEndCellDeg = 20.0f;     // hybrid end cell
        static constexpr float kDragPx = 240.0f, kFinePx = 1200.0f, kUltraPx = 6000.0f;

    private:
        // Private state: completed by G2; not part of the API.
        enum class Drag : uint8_t { none, continuous, stepped, hybrid };
        enum class Write : uint8_t { tap, drag, wheel };

        void  refresh();                                // re-reads the view and the overlay when their keys changed
        bool  refused() const noexcept;                 // locked, derived, n/a
        bool  hybrid() const noexcept;                  // continuous with end cells
        bool  writable() const noexcept;                // a port and a continuous or stepped view
        bool  hasRoutes() const noexcept;               // an overlay with routes is set
        int   nLo() const noexcept;
        int   nHi() const noexcept;
        const Detent* endCell(int ord) const noexcept;  // hybrid ordinal: 0..nLo-1 low cells, nLo the range, then high
        int   hybridOrdinal() const noexcept;
        int   activeDetent() const noexcept;
        float angleOf(float t) const noexcept;          // track position -> screen angle, radians
        float cellAngle(int ord) const noexcept;        // a hybrid end cell's centre
        float detentTrack(int i) const noexcept;        // detent i's track position, (i + 0.5) / n
        float pointerTarget() const noexcept;           // the angle the pointer eases to
        float travel(Point) const noexcept;             // pointer travel from down_, px (right or up increases)
        float cellTravel(int ord, float edge, float span) const noexcept;
        void  write(float host01, GestureController&, Write, double nowSec);
        void  writeDetentBy(int i, GestureController&, Write, double nowSec);
        void  writeHybrid(int ord, float t, GestureController&, Write, double nowSec);
        void  stepHybrid(int dir, float amount, GestureController&, Write, double nowSec);
        void  finishDrag(GestureController&);
        int   notches(float v, bool smooth) noexcept;

        ValueModel&       model_;
        DialGeom          geom_;
        uint32_t          a11yId_;
        const ModOverlay* mod_ = nullptr;

        ModOverlay modView_{};                          // the sanitised copy of *mod_ (routes 0 when mod_ is null)
        bool       modValid_ = false;                   // modView_ holds *mod_ as of modView_.key
        ValueView  view_{};
        uint64_t   viewKey_ = 0;
        bool       viewValid_ = false;

        float hover_ = 0.0f;                            // hover chrome 0..1 (90 ms in, 160 ms out)
        bool  hoverOn_ = false;
        bool  alwaysChrome_ = false;
        float pointerA_ = 0.0f;                         // the eased pointer angle, radians
        bool  landing_ = false;                         // the pointer eases to a Mode switch's position (FC 02 §8.7)
        float flash_ = 0.0f, flashHold_ = 0.0f;         // label flash amount, and its hold time left

        Drag  drag_ = Drag::none;
        Point down_{};                                  // pointer down (travel origin)
        float anchorS_ = 0.0f;                          // stepped: travel of the current detent's own position
        float edgeS_ = 0.0f;                            // continuous/hybrid: travel at which the track is 0
        float dragT_ = 0.0f;                            // the track value this drag last computed
        bool  fine_ = false, ultra_ = false;            // Shift / Cmd at the anchor
        int   dragDetent_ = -1;                         // stepped: detent; hybrid: ordinal
        float dragTravel_ = 0.0f;                       // stepped: travel from the current detent (ghost pointer)
        float wheelAcc_ = 0.0f;                         // smooth wheel accumulation (stepped, hybrid cells and edges)
        double wheelLast_ = -1.0;                       // nowSec of the last wheel event
    };
}

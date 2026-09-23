#pragma once

// An on/off latch (02 §5.5): HR's Freeze behaviour — arm on pointer down, commit on pointer up inside, and dragging off
// cancels (BgfxEditor.cpp:1724, 1774-1788). DELTA, BYPASS and CHARACTERISTICS in FCompressor. Inks: off ink16 fill /
// ink52 text; on ink70 fill / ground text; armed ink32 fill; pressed accent text. A disabled toggle refuses and the
// footer shows its reason. Return/Space toggle it from the keyboard; it is a toggleButton to accessibility.
//
// Declared in G2 (v0.2.0, frozen at FZ1); implemented by G6 (src/widgets/LatchToggle.cpp).
//
// G6's choices where 02 is silent (fg.gallery.latch and fg.lineedit's latch.* rows pin them):
// - Drawing (HR :1209-1215): a radius-0 fill of the rectangle and the label in kLatch, centred and cap-centred, tagged
//   LATCH. Off: ink16 fill, text ink52 easing to ink100 under the pointer (90 ms in, 160 ms out). On: ink70 fill,
//   ground text. Armed (pressed): ink32 fill, accent text, whatever the state. Disabled (refused; the footer and the
//   a11y help give reason()): ink16 fill with ink32 text when off, ink32 fill with ground text when on; no hover.
// - The pointer argument of tick() is where the pointer is; a Panel passes a point outside the latch when it has left.
// - Every commit is ToggleModel::set(!on()), which a product implements as one GestureController tap. A popup press
//   opens the host menu of ToggleModel::port() (when there is one) and never arms or writes, also when disabled.
// - Keys: Return and Space toggle and return true; on a disabled latch they are refused (no write) and still return
//   true, so the host does not take them; every other key returns false.
// - A11y: toggleButton with the label as title, checkable, checked = on(), enabled(), help = reason() when disabled.
//   a11yAction press / toggle toggle it (refused when disabled); showMenu opens the host menu at the centre.

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/panel/Input.h>

#include <cstdint>
#include <vector>

namespace funkgui
{
    class Canvas;
    class GestureController;
    class ParamPort;
    struct Theme;

    class ToggleModel
    {
    public:
        virtual ~ToggleModel() = default;

        virtual bool on() const = 0;
        virtual bool enabled() const { return true; }
        virtual const char* reason() const { return nullptr; }    // shown when disabled
        virtual void set(bool, GestureController&) = 0;

        // The host parameter behind the toggle, for the right-click host menu; nullptr when there is none (G2 addition,
        // as CellModel::port).
        virtual ParamPort* port() { return nullptr; }
    };

    class LatchToggle
    {
    public:
        LatchToggle(ToggleModel&, Rect, const char* label, uint32_t a11yId);

        void   tick(float dt, Point pointer);
        void   draw(Canvas&, const Theme&, bool focusRing) const;
        bool   contains(Point) const;
        Cursor cursorAt(Point) const;                    // pointingHand when enabled
        void   pointerDown(const PointerEvent&, GestureController&);   // arms; popup -> host menu, never a write
        void   pointerDrag(const PointerEvent&);         // leaving the rectangle disarms (drag-off cancels)
        void   pointerUp(const PointerEvent&, GestureController&);     // commits when released inside while armed
        bool   key(const KeyEvent&, GestureController&); // Return / Space toggle
        bool   a11yAction(uint32_t id, A11yAction, GestureController&);   // press / toggle; false for another id
        void   accessibility(std::vector<A11yItem>&) const;               // toggleButton, checkable
        bool   armed() const noexcept { return armed_; }

        const Rect& bounds() const noexcept { return rect_; }
        uint32_t a11yId() const noexcept { return a11yId_; }

        // ---- G6 additions (additive API) -------------------------------------------------------------------------

        bool settled() const;                            // the hover ease has reached its target
        const char* reason() const;                      // the model's reason while disabled, else nullptr

    private:
        // Private state: completed by the implementing card (G6); not part of the frozen API.
        ToggleModel& model_;
        Rect         rect_;
        const char*  label_;
        uint32_t     a11yId_;
        bool         armed_ = false;
        float        hover_ = 0.0f;
        bool         hoverOn_ = false;                   // the hover ease's target
    };

    // A ToggleModel over one boolean host parameter (DELTA, BYPASS; G6 addition): on() is value01() >= 0.5, set() is one
    // GestureController tap of 1 or 0 (nothing when unchanged), and port() is the parameter, for the host menu.
    // Subclass it to disable the toggle (enabled / reason).
    class ParamToggle : public ToggleModel
    {
    public:
        explicit ParamToggle(ParamPort&);

        bool on() const override;
        void set(bool, GestureController&) override;
        ParamPort* port() override;

    private:
        ParamPort& port_;
    };
}

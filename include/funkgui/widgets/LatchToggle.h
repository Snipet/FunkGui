#pragma once

// An on/off latch (02 §5.5): HR's Freeze behaviour — arm on pointer down, commit on pointer up inside, and dragging off
// cancels (BgfxEditor.cpp:1724, 1774-1788). DELTA, BYPASS and CHARACTERISTICS in FCompressor. Inks: off ink16 fill /
// ink52 text; on ink70 fill / ground text; armed ink32 fill; pressed accent text. A disabled toggle refuses and the
// footer shows its reason. Return/Space toggle it from the keyboard; it is a toggleButton to accessibility.
//
// Declared in G2 (v0.2.0, frozen at FZ1); implemented by G6 (src/widgets/LatchToggle.cpp).

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

    private:
        // Private state: completed by the implementing card (G6); not part of the frozen API.
        ToggleModel& model_;
        Rect         rect_;
        const char*  label_;
        uint32_t     a11yId_;
        bool         armed_ = false;
        float        hover_ = 0.0f;
    };
}

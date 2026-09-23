#pragma once

// A kMicro word in a slot's label row (02 §5.5, F §3.5): AUTO on MAKEUP, EXT on DETECT, LISTEN on SC HPF. Its hit
// rectangle {x+w-34, top-5, 38, 18} is carved out of the slot's (RuleSlider checks the word first). It toggles like a
// LatchToggle (arm on down, commit on up inside). Inks: rest ink32, on ink100, hover ink70, pressed accent, disabled
// ink16 (refused; the footer shows the reason). In the Tab order it follows its slot (02 §8.9).
//
// Declared in G2 (v0.2.0, frozen at FZ1); implemented by G5 (src/widgets/AttachedWord.cpp).

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/panel/Input.h>
#include <funkgui/widgets/LatchToggle.h>
#include <funkgui/widgets/RuleSlider.h>

#include <cstdint>
#include <vector>

namespace funkgui
{
    class Canvas;
    class GestureController;
    struct Theme;

    class AttachedWord
    {
    public:
        AttachedWord(ToggleModel&, const SlotGeom&, const char* word, uint32_t a11yId);

        static constexpr Rect hitFor(const SlotGeom& g) noexcept
        {
            return { g.x + g.w - 34.0f, g.top - 5.0f, 38.0f, 18.0f };
        }

        Rect   hit() const noexcept { return hitFor(geom_); }
        bool   contains(Point p) const noexcept { return hit().contains(p); }
        void   tick(float dt, bool hovered);
        void   draw(Canvas&, const Theme&, bool focusRing) const;
        Cursor cursorAt(Point) const;                    // pointingHand when enabled
        void   pointerDown(const PointerEvent&, GestureController&);   // arms; popup -> host menu, never a write
        void   pointerDrag(const PointerEvent&);         // leaving the word disarms
        void   pointerUp(const PointerEvent&, GestureController&);     // commits when released inside while armed
        bool   key(const KeyEvent&, GestureController&); // Return / Space toggle
        bool   a11yAction(uint32_t id, A11yAction, GestureController&);   // press / toggle; false for another id
        void   accessibility(std::vector<A11yItem>&) const;               // toggleButton, checkable
        const char* reason() const;                      // the model's reason while disabled, else nullptr

        uint32_t a11yId() const noexcept { return a11yId_; }

    private:
        // Private state: completed by the implementing card (G5); not part of the frozen API.
        ToggleModel& model_;
        SlotGeom     geom_;
        const char*  word_;
        uint32_t     a11yId_;
        bool         armed_ = false;
        float        hover_ = 0.0f;
    };
}

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

    private:
        // Private state: completed by the implementing card (G5); not part of the frozen API.
        ValueModel&   model_;
        SlotGeom      geom_;
        uint32_t      a11yId_;
        AttachedWord* word_ = nullptr;
        ValueView     view_{};
        uint64_t      viewKey_ = 0;
        bool          viewValid_ = false;
    };
}

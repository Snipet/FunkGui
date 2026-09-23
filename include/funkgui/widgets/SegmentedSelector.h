#pragma once

// A row of cells, one of which is active (02 §5.5): setup cells (QUALITY, LOOKAHEAD), preferences (history span, meter
// scale, THEME), tabs and stepped parameters. It selects on pointer down (HR BgfxEditor.cpp:1709), is one Tab stop for
// the whole group (←/→, Home/End), and is a radioGroup with radioButton children to accessibility. The CellModel does
// the write, with tap semantics: nothing when the cell is already active.
//
// Declared in G2 (v0.2.0, frozen at FZ1); implemented by G6 (src/widgets/SegmentedSelector.cpp).

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

    class CellModel                                     // index-based: setup cells, preferences, tabs, stepped params
    {
    public:
        virtual ~CellModel() = default;

        virtual int  count() const = 0;
        virtual int  active() const = 0;
        virtual const char* label(int) const = 0;
        virtual const char* spoken(int) const = 0;
        virtual bool enabled(int) const { return true; }
        virtual void select(int, GestureController&) = 0;    // tap semantics; no-op when unchanged

        // The host parameter behind the cells, for the right-click host menu; nullptr for a preference or a tab.
        // (G2 addition: 02 §8.1 gives every parameter control the host menu.)
        virtual ParamPort* port() { return nullptr; }
    };

    enum class CellStyle : uint8_t { text, boxed };    // text = HR theme cells (active ink70, hover ink100, rest ink32)

    class SegmentedSelector
    {
    public:
        SegmentedSelector(CellModel&, std::vector<Rect> cells, CellStyle, const char* caption, Point captionAt,
                          uint32_t a11yId);

        void tick(float dt, Point pointer);
        void draw(Canvas&, const Theme&, bool focusRing) const;
        int  cellAt(Point) const;                        // -1 outside every cell
        bool contains(Point) const;                      // cellAt(p) >= 0
        Cursor cursorAt(Point) const;                    // pointingHand over an enabled cell
        void pointerDown(const PointerEvent&, GestureController&);   // selects on down (HR :1709); popup -> host menu
        bool key(const KeyEvent&, GestureController&);   // ←/→ Home/End; one Tab stop for the group
        void accessibility(std::vector<A11yItem>&) const;   // radioGroup + radioButton children

        // An a11y action on the group (a11yId) or on cell i's radioButton (a11yId + 1 + i); false when the id is not
        // this selector's. (G2 addition: a Panel routes a11yAction(id, …) to the widget that owns the id.)
        bool a11yAction(uint32_t id, A11yAction, double value, GestureController&);

        uint32_t a11yId() const noexcept { return a11yId_; }

    private:
        // Private state: completed by the implementing card (G6); not part of the frozen API.
        CellModel&        model_;
        std::vector<Rect> cells_;
        CellStyle         style_;
        const char*       caption_;
        Point             captionAt_;
        uint32_t          a11yId_;
        int               hovered_ = -1;
        std::vector<float> hoverAmt_;
    };
}

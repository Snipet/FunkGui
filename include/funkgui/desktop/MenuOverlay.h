#pragma once

// A popup menu drawn by the Canvas inside the Panel's own window (v0.28.0): what DesktopHost shows for
// HostServices::showMenu, where EditorHost opens a juce::PopupMenu in a window of its own and WebHost a menu in the
// DOM. A desktop host without JUCE has no menu windows, so the menu is an overlay: recorded into the frame after the
// Panel's own drawing, in the look of juce/MenuLook.h (its metrics and its palette, mixed from the request's theme),
// and fed the window's pointer, wheel and key events while it shows. Plain C++ over the Canvas and the Panel's input
// structs: no SDL, no bgfx and no JUCE, so a test drives one and reads its layout and its frame without a window
// (fg.desktop.menu, through HeadlessHost and the software rasteriser).
//
// What a request's rows become (panel/HostServices.h MenuItem; validMenu() is the rule for a request):
// - an item: its label, at its left a tick when `checked` (a dot when `radio`), at its right its `shortcut` as text;
//   `enabled = false` draws it dim and it can be neither highlighted nor chosen;
// - a separator: a hairline; a header: its label in small capitals, dim, never highlighted;
// - a submenu: a chevron at the right; the rows open in a panel beside the row when the pointer rests on it or Right,
//   Return or Space is pressed on it (a disabled one opens nothing), and the callback gets the id chosen in it.
//
// Placement, in the Panel's logical px, inside `bounds` (the Panel's size) less kEdgePx all round: the top level opens
// with its left edge on the anchor's, kGapPx under the anchor, or over it when it does not fit under and there is
// more room over; a submenu opens at its row's right (kOverlapPx over its parent), or at its left when the right has
// no room. A panel is moved left and up until it is inside, and one taller than the bounds is as tall as they allow
// and scrolls (the wheel; the keys bring the row they move to into view). `minWidth` widens the top level
// (a dropdown's width); `selectedId` highlights that top-level item at the start, scrolled into view.
//
// Input while it shows (the host sends it every event; nothing reaches the Panel):
// - the pointer highlights the enabled row under it and opens or closes submenus as it moves;
// - a release over an enabled item chooses it (so a press that opened the menu can be dragged onto an item and let
//   go); a press outside every panel dismisses the menu;
// - Up and Down move through the rows that can be highlighted, wrapping; Home, End, Page Up and Page Down; Right opens
//   a submenu, Left closes one; Return and Space choose the highlighted item (or open its submenu); Escape dismisses
//   the whole menu; a typed character moves to the next row whose label starts with what was typed since the last
//   pause of kTypeAheadSeconds, case aside (the same letter again: the next row that starts with it).
//
// The callback follows HostServices' rules: it runs at most once, with the chosen id or with 0 when the user dismissed
// the menu, never inside show(), and never after dismiss(); a second show() replaces a menu still showing, whose
// callback does not run. It runs from the input call that chose or dismissed, after the menu has closed, so it may
// call show() again.
//
// Main thread, like its host.

#include <funkgui/core/Geometry.h>
#include <funkgui/core/Theme.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Input.h>

#include <cstdint>
#include <string>
#include <vector>

namespace funkgui
{
    class Canvas;
}

namespace funkgui::desktop
{
    class MenuOverlay
    {
    public:
        // MenuLook's metrics, in logical px.
        static constexpr float kRowPx = 24.0f;           // an item
        static constexpr float kSeparatorPx = 9.0f;
        static constexpr float kHeaderPx = 24.0f;        // a section header
        static constexpr float kPadPx = 5.0f;            // over the first row and under the last
        static constexpr float kInsetPx = 5.0f;          // a row's pill from the panel's sides
        static constexpr float kRadiusPx = 8.0f;         // the panel's corners
        static constexpr float kPillRadiusPx = 5.0f;
        static constexpr float kMinPanelPx = 120.0f;     // the narrowest panel
        static constexpr float kGapPx = 4.0f;            // between the anchor and the panel
        static constexpr float kTextPx = 14.0f;          // a label (MenuLook::kMenuPx)
        static constexpr float kHeaderTextPx = 11.0f;
        // The overlay's own.
        static constexpr float kLabelPx = 26.0f;         // a label's left edge from its pill's (MenuLook's)
        static constexpr float kMarkPx = 14.0f;          // the centre of the tick or the dot from the pill's left edge
        static constexpr float kRowPadPx = 10.0f;        // after the label, the shortcut or the chevron
        static constexpr float kColumnGapPx = 24.0f;     // between the label and the shortcut or the chevron
        static constexpr float kChevronPx = 6.0f;        // a chevron's width
        static constexpr float kEdgePx = 4.0f;           // a panel keeps this far from the bounds' edges
        static constexpr float kOverlapPx = 3.0f;        // a submenu's panel over its parent's
        static constexpr float kShadowPx = 8.0f;         // the soft shadow's reach
        static constexpr double kTypeAheadSeconds = 1.0;

        MenuOverlay();
        ~MenuOverlay();
        MenuOverlay(const MenuOverlay&) = delete;
        MenuOverlay& operator=(const MenuOverlay&) = delete;

        // Opens the menu inside boundsW x boundsH and returns true; false, with `done` dropped unrun, for a request
        // that fails validMenu(). Either way a menu still showing is closed first and its callback dropped.
        bool show(const MenuRequest&, MenuCallback done, float boundsW, float boundsH);

        // Closes the menu, if one shows; its callback does not run.
        void dismiss();

        bool showing() const noexcept { return !levels_.empty(); }

        // The bounds changed (the window was resized): the panels are placed again.
        void setBounds(float boundsW, float boundsH);

        // Input, in the Panel's logical px. Each returns whether a menu was showing when it arrived (the event is then
        // the menu's, and the host gives it to nothing else).
        bool pointerMove(float x, float y);
        bool pointerDown(float x, float y);              // outside every panel: dismissed, the callback gets 0
        bool pointerUp(float x, float y);                // over an enabled item: chosen
        bool pointerExit();                              // the pointer left the window: no row of the last panel lit
        bool wheel(float x, float y, float dy);          // dy in JUCE's units (WheelEvent::dy): scrolls a tall panel
        bool key(const KeyEvent&, double nowSeconds);    // nowSeconds: the host's clock, for the type-ahead's pause

        // Records the menu into the frame, over what is there: call it after Panel::draw in the same frame. Nothing
        // while no menu shows.
        void draw(Canvas&) const;

        // Changes whenever what draw() records would change (a menu opened or closed, a highlight moved, a submenu, a
        // scroll): a host that leaves unchanged frames undrawn draws when it moved.
        uint32_t revision() const noexcept { return revision_; }

        // ---- the layout, for tests and for a host that wants to know where the menu is -----------------------------
        enum class RowKind : uint8_t { item, separator, header };

        struct Row
        {
            RowKind kind = RowKind::item;
            int     item = 0;                            // the index in its level's items
            float   y = 0.0f, h = 0.0f;                  // from the first row's top, before the scroll
            bool    selectable = false;                  // can be highlighted: an enabled item (or enabled submenu)
            bool    submenu = false;
        };

        struct Level
        {
            const std::vector<MenuItem>* items = nullptr;
            Rect  panel{};                               // where the panel is drawn
            float contentH = 0.0f;                       // every row's height: more than the panel holds when it scrolls
            float scroll = 0.0f;                         // px the rows are moved up
            int   highlighted = -1;                      // a row index, or -1
            int   open = -1;                             // the row whose submenu is the next level, or -1
            std::vector<Row> rows;
        };

        // The panels showing: the top level, then each open submenu.
        const std::vector<Level>& levels() const noexcept { return levels_; }

        // The rectangle of row `row` of level `level` as it is drawn (its pill's row: the panel's width less the
        // insets), after the scroll; empty for an index that names none.
        Rect rowRect(int level, int row) const;

    private:
        void  buildLevel(Level&, const std::vector<MenuItem>& items, float minWidth) const;
        void  placeTop();
        void  placeChild(std::size_t level);
        void  clampInto(Level&) const;
        void  openChild(std::size_t level, int row, bool highlightFirst);
        void  closeFrom(std::size_t level);              // levels [level, end) go
        void  setHighlight(std::size_t level, int row);
        void  scrollIntoView(Level&, int row);
        int   hitLevel(float x, float y) const;          // the deepest panel under the point, or -1
        int   hitRow(const Level&, float x, float y) const;
        void  finish(int id);                            // close, then run the callback with id
        void  touch() noexcept { ++revision_; }
        float textWidth(const std::string& utf8, bool header) const;

        MenuRequest  request_{};                         // the rows the levels point into, and the theme
        MenuCallback done_;
        std::vector<Level> levels_;
        float    boundsW_ = 0.0f, boundsH_ = 0.0f;
        uint32_t revision_ = 0;
        std::string typed_;                              // the type-ahead's prefix, lower-cased
        double   typedAt_ = -1.0e9;
    };
}

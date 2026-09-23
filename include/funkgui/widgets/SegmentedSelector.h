#pragma once

// A row of cells, one of which is active (02 §5.5): setup cells (QUALITY, LOOKAHEAD), preferences (history span, meter
// scale, THEME), tabs and stepped parameters. It selects on pointer down (HR BgfxEditor.cpp:1709), is one Tab stop for
// the whole group (←/→, Home/End), and is a radioGroup with radioButton children to accessibility. The CellModel does
// the write, with tap semantics: nothing when the cell is already active.
//
// Declared in G2 (v0.2.0, frozen at FZ1); implemented by G6 (src/widgets/SegmentedSelector.cpp).
//
// G6's choices where 02 is silent (fg.gallery.segmented and fg.lineedit's cells.* rows pin them):
// - Inks (02 §8.10, HR's theme cells): CellStyle::text draws each label in kCaption, centred on its cell and
//   cap-centred: active ink70, rest ink32 easing to ink100 under the pointer (90 ms in, 160 ms out), disabled ink16.
//   CellStyle::boxed fills each cell (radius 0) and draws the label in kCaption: rest ink16 fill with ink52 text easing
//   to ink100, active ink70 fill with ground text (a LatchToggle's inks), disabled ink16 fill with ink32 text. The
//   active cell is drawn active even when the model says it is disabled. Accent is never used: a cell is not "the
//   thing under the hand" once selected (HR). The caption, when not null, is kCaption ink52 with its top-left at
//   captionAt. Everything is tagged CELL.
// - The focus ring (drawFocusRing) goes round the union of the cells, grown by 2 px, so it clears their labels.
// - Keys: → and ↑ select the next enabled cell, ← and ↓ the previous one, Home / End the first / last enabled one;
//   they stop at the ends (no wrap) and write only through select(), so a key that lands on the active cell writes
//   nothing. Return and Space are consumed with no effect (the arrows already selected; 02 §8.9 "select cell"). Every
//   other key returns false. A key the group consumes returns true even when nothing could be selected.
// - Pointer: a press on an enabled cell selects it; on a disabled cell it is refused (nothing); a popup press anywhere
//   on the group opens the host menu of CellModel::port() when there is one and never writes. The pointer argument
//   of tick() is where the pointer is; a Panel passes a point outside every cell when the pointer has left.
// - A11y: the radioGroup (a11yId) spans the union of the cells, its title is the spoken title (setSpokenTitle), else
//   the caption, and its value is the active cell's spoken text; radioButton i (a11yId + 1 + i, parent a11yId) has the
//   cell's rectangle, title spoken(i) (label(i) when spoken is empty), checked when active, enabled(i), and help(i).
//   a11yAction on the group: increment / decrement step like → / ←, setValue(v) selects cell round(v); on a cell:
//   press / toggle / setValue select it; showMenu (either) opens the host menu at the item's centre; focus does nothing.
// - Cells are min(model.count(), cells.size()); a model with fewer cells than rectangles leaves the rest undrawn.

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

        // The a11y help of cell i: what it does ("Std: 2 times IIR oversampling, 4 samples latency", 02 §8.9) or, for a
        // disabled cell, why it is refused; nullptr for none. (G6 addition.)
        virtual const char* help(int) const { return nullptr; }
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

        // ---- G6 additions (additive API) -------------------------------------------------------------------------

        // True when no hover ease is moving: a Panel's wantsFullRate() is the OR of !settled() over its widgets.
        bool settled() const;

        // The group's a11y title when it differs from the drawn caption or there is none ("Theme"); nullptr: the
        // caption. The string must outlive the selector.
        void setSpokenTitle(const char*);

        // The union of the cells (the radioGroup's bounds; the focus ring is drawn round it, grown by 2 px).
        Rect bounds() const noexcept;

    private:
        int  cells() const;                              // min(model.count(), cells_.size())
        int  step(int from, int dir) const;              // the next enabled cell from `from` in `dir`, or -1
        void selectCell(int i, GestureController&);      // enabled cells only
        void showMenu(float x, float y, GestureController&);

        CellModel&         model_;
        std::vector<Rect>  cells_;
        CellStyle          style_;
        const char*        caption_;
        Point              captionAt_;
        uint32_t           a11yId_;
        const char*        spokenTitle_ = nullptr;
        int                hovered_ = -1;
        std::vector<float> hoverAmt_;
    };

    // ---- CellModel implementations (G6 additions) ------------------------------------------------------------------

    // The drawn label, the a11y text (nullptr: the label) and the a11y help (nullptr: none) of one cell.
    struct CellText
    {
        const char* label = "";
        const char* spoken = nullptr;
        const char* help = nullptr;
    };

    // The cells of one choice parameter (QUALITY, LOOKAHEAD, a stepped ratio): cell i writes host01 = i / (n - 1), the
    // normalised value of choice i of an n-choice juce::AudioParameterChoice; the active cell is the nearest one to the
    // port's value. select() is a GestureController tap, so selecting the active cell writes nothing. The texts must
    // outlive the model. Subclass it to disable a cell (enabled / help).
    class ParamCells : public CellModel
    {
    public:
        ParamCells(ParamPort&, std::vector<CellText> texts);

        int  count() const override;
        int  active() const override;
        const char* label(int) const override;
        const char* spoken(int) const override;
        const char* help(int) const override;
        void select(int, GestureController&) override;
        ParamPort* port() override;

        float host01(int i) const noexcept;              // the value cell i writes

    private:
        ParamPort&            port_;
        std::vector<CellText> texts_;
    };

    // The cells of one integer preference (02 §5.9; FCompressor: meterScaleDb 12 / 24 / 48 / 72, historySpanTenths 25 /
    // 50 / 100 / 200): cell i stands for values[i]. The active cell is the one whose value UiPreferences holds under
    // `key`; a missing or foreign value reads as `fallback` (and as cell 0 when `fallback` is not one of the values).
    // select() writes through UiPreferences::setInt (machine-wide, written through, bumps revision(); no gesture: a
    // preference is not a host parameter), and nothing when the value is already held. The key and the texts must
    // outlive the model; values and texts are paired by index. Constructing one opens the store, so no later tick or
    // draw is the first access (02 §3.7 rule 4).
    class PrefCells : public CellModel
    {
    public:
        PrefCells(const char* key, std::vector<int> values, std::vector<CellText> texts, int fallback);

        int  count() const override;
        int  active() const override;
        const char* label(int) const override;
        const char* spoken(int) const override;
        const char* help(int) const override;
        void select(int, GestureController&) override;

        int value() const;                               // the held value, snapped to the cells (fallback rule above)

    private:
        const char*           key_;
        std::vector<int>      values_;
        std::vector<CellText> texts_;
        int                   fallback_;
    };
}

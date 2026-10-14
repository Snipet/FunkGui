#pragma once

// The accessibility bridge (02 §5.6; HR BgfxEditor.cpp:249-495, AccessibleItem): a Panel's A11yItems become invisible
// juce::Components on the editor, one per item id, so VoiceOver sees the same controls the GPU draws. Each child paints
// nothing, takes no mouse and no keyboard focus, and has the item's hit rectangle as its bounds; a radioButton whose
// parent item exists is nested inside that item's component.
//
// Mapping (JUCE 8.0.4): role -> juce::AccessibilityRole (radioGroup -> group); title, description, help ->
// setTitle/setDescription/setHelpText; enabled -> setEnabled; visible -> setVisible; checkable/checked -> the
// handler's state. A slider or progress bar gets a value interface over {v, lo, hi, step} (read-only for readOnly
// items and progress bars): stepped sliders live in index space (F §8.2), a continuous one (step 0) reports
// (hi - lo) / 100 as its interval so VoiceOver's increment (current + interval) moves it; setValue calls
// Panel::a11yAction(id, setValue, v). Any other item with a spoken value gets a read-only text value. Actions: press
// (buttons, toggles, radio buttons, list items, combo boxes), toggle (toggles and checkable items), showMenu (combo
// boxes and sliders: the host menu), focus (every interactive role); each calls Panel::a11yAction.
//
// sync() is cheap when nothing changed. When the list's structure (ids, roles, parents, in order) differs from the
// last sync it rebuilds every child; otherwise it updates each child in place and notifies valueChanged for a changed
// value or checked state (titles notify through Component::setTitle). EditorHost calls it on a11yRevision() changes
// and at <= 10 Hz for values. Message thread only. The Panel must outlive the bridge's children: clear() them first.
//
// FSynth G1, additive:
// - Roles: the six a11yrole constants map to juce::AccessibilityRole::{group, list, table, row, cell, editableText}.
//   Each takes the focus action, as every interactive role does, and no press, toggle or menu action (a checkable
//   item of any role keeps the toggle action). Nesting is by A11yItem::parent whatever the roles: a group's buttons,
//   a table's rows, a row's cells.
// - Tables: a `table` item's handler has a table interface over the item's rows and cols (the WHOLE table, of which
//   only some rows may be listed): getRowHandler(r) and getCellHandler(r, c) find the listed row and cell components
//   (nullptr for a row or a cell that is not listed), a row's span is { row, 1 } and a cell's is { its row's row, 1 }
//   and { col, 1 }, there is no header, and showCell calls Panel::a11yAction(id, focus) for the cell, so the product
//   scrolls it into view. A row inside a table, and every item whose parent is a row (a `cell`, or the slider,
//   button or toggle that is the cell), also has a cell interface that names the table: JUCE's macOS backend asks a
//   row or a cell for its index only when it has one.
// - An `editableText` item always has the read-only text value, empty when its value is empty, so typing the first
//   character does not replace its handler. Typing reaches the Panel as keys, never through the bridge.
// - A change of rows, cols, row or col alone is an in-place update, like a value's.
// - handlerEpoch(id) counts the times an item's handler was dropped for a new shape, as rebuilds() counts the
//   structure syncs: what a test reads to see that a sync kept a handler (JUCE gives none to look at without a
//   window). A dropped handler loses JUCE's accessibility focus, and so does a handler nested under it; EditorHost
//   gives the focus back to its focus target after such a sync (HostServices::a11yFocus).
// On Linux this file compiles and runs as on macOS, as it did before: the components and their handlers are built
// and no assistive technology reads them (JUCE 8.0.4 has no native accessibility there).
//
// Zoom (G7c, v0.8.0): items stay in the Panel's logical px (items(), and so EditorHost's A11Y_DUMP, do not change with
// the zoom); a child's bounds are the item's rectangle times scale() (editor px per logical px, 1 by default), as the
// smallest integer rectangle containing it, and a nested child's are relative to its parent's scaled rectangle.

#include <funkgui/a11y/A11yItem.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace funkgui
{
    class Panel;

    class A11yBridge
    {
    public:
        A11yBridge();
        ~A11yBridge();                               // removes and deletes every child

        A11yBridge(const A11yBridge&) = delete;
        A11yBridge& operator=(const A11yBridge&) = delete;

        void sync(juce::Component& editor, const std::vector<A11yItem>& items, Panel& panel);
        void clear();                                // removes and deletes every child

        // G7c: editor px per logical px (EditorHost's zoom; not finite or <= 0 reads as 1). A change re-places every
        // child at once; the next sync() keeps the new scale.
        void  setScale(float editorPxPerLogicalPx);
        float scale() const noexcept { return scale_; }

        const std::vector<A11yItem>& items() const noexcept { return items_; }   // as last synced
        int  size() const noexcept { return static_cast<int>(children_.size()); }
        juce::Component* componentFor(uint32_t id) const noexcept;               // nullptr for an unknown id
        uint32_t rebuilds() const noexcept { return rebuilds_; }                 // structure syncs so far
        // FSynth G1: how many times sync() dropped this item's accessibility handler since its component was made (a
        // change that alters the handler's shape: checkable, or a spoken value appearing or disappearing). 0 for an
        // unknown id, and again after a rebuild, which makes every component anew.
        uint32_t handlerEpoch(uint32_t id) const noexcept;

        class Item;                                  // the invisible child (A11yBridge.cpp)

    private:
        bool sameStructure(const std::vector<A11yItem>&) const;
        void rebuild(juce::Component& editor, const std::vector<A11yItem>& items, Panel& panel);

        std::vector<A11yItem> items_;
        std::vector<std::unique_ptr<Item>> children_;   // in items_ order
        uint32_t rebuilds_ = 0;
        float    scale_ = 1.0f;                      // G7c
    };
}

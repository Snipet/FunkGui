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

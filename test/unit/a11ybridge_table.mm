// FUNKGUI_TEST name=fg.a11ybridge.table timeout=300 gpu=1
//
// fg.a11ybridge.table (FSynth G1; macOS only: nothing reads the bridge on Linux): A11yBridge over the six a11yrole
// constants and A11yItem's table fields (gpu/A11yBridge.h, "FSynth G1"). No window: the bridge is synced onto a plain
// juce::Component that is never put on screen, and the handlers are made with
// componentFor(id)->createAccessibilityHandler(), which needs no peer. Spec rows only.
//
// The fixture is one item of each of the six roles, none checkable (a group with a button, a list with an item, a text
// field with a value and one without), and a 64 x 8 table of which 21 rows are listed, each with eight cells: a
// `cell` item, six sliders and a button.
//
//   role.*       each of the six maps to its JUCE role: group, list, table, row, cell, editableText
//   actions.*    each of those handlers holds the focus action and none of press, toggle and showMenu; the focus
//                action reaches Panel::a11yAction; a checkable item of a new role keeps the toggle action
//   v0140.*      the ten roles of v0.14.0 map and act as they did
//   table.*      rows 64 and columns 8, no header; a row's span is { row, 1 } and it has no column; a cell's are
//                { its row's row, 1 } and { col, 1 }, whatever the cell's own role; an item outside the table has
//                neither; getRowHandler and getCellHandler answer null for a row that is not listed, a column that
//                does not exist, and a negative index; showCell asks the Panel to focus the cell
//   cell.*       a row of the table and each of its cells has a cell interface (JUCE's macOS backend asks only such
//                an element for its index); the table, the group and its button have none
//   nest.*       group -> button, list -> item, table -> row -> cell, by component parent
//   focus.*      grabFocus on such a handler gives it JUCE's accessibility focus and leaves the keyboard focus alone
//   text.*       an editableText item has a read-only text value, with a value and without one
//   place.*      a change of rows, cols, row or col alone updates in place: no rebuild, the same components
//   epoch.*      handlerEpoch counts an item's dropped handlers: typing into an editableText item drops none, a
//                value appearing on another item drops one, a rebuild starts again from 0
//
// Without a peer JUCE gives a component no handler of its own (juce_Component.cpp:2992-2995), so getRowHandler and
// getCellHandler answer null here for a listed row too; table.no_peer.* pins that, and the lookups that find a
// handler, like EditorHost's focus target, are checked by hand under VoiceOver in the gallery app (section a11ytable).

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Theme.h>
#include <funkgui/gpu/A11yBridge.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Panel.h>
#include <funkgui/test/Harness.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace T = funkgui::test;
using funkgui::A11yAction;
using funkgui::A11yBridge;
using funkgui::A11yItem;
using funkgui::A11yRole;
namespace role = funkgui::a11yrole;
using JR = juce::AccessibilityRole;
using JA = juce::AccessibilityActionType;

namespace
{
    constexpr int kRows = 64, kCols = 8, kFirstListed = 20, kListed = 21;
    constexpr uint32_t kGroup = 1, kButton = 2, kList = 3, kListItem = 4, kText = 5, kEmptyText = 6, kNote = 7;
    constexpr uint32_t kTable = 10, kRowBase = 100, kCellBase = 1000;

    constexpr uint32_t rowId(int r) { return kRowBase + static_cast<uint32_t>(r); }
    constexpr uint32_t cellId(int r, int c) { return kCellBase + static_cast<uint32_t>(r * kCols + c); }

    // A Panel that only records the accessibility actions it is asked to take.
    class LogPanel final : public funkgui::Panel
    {
    public:
        std::vector<std::pair<uint32_t, A11yAction>> actions;

        void attach(funkgui::HostServices&) override {}
        int  width() const override { return 688; }
        int  height() const override { return 444; }
        void tick(float) override {}
        void draw(funkgui::Canvas&, const funkgui::Theme&) override {}
        bool wantsFullRate() const override { return false; }
        void accessibility(std::vector<A11yItem>&) const override {}
        uint32_t a11yRevision() const override { return 0; }
        void a11yAction(uint32_t item, A11yAction a, double) override { actions.emplace_back(item, a); }
        void closeGestures() override {}
    };

    A11yItem item(uint32_t itemId, uint32_t parent, A11yRole r, const char* title, funkgui::Rect b)
    {
        A11yItem it;
        it.id = itemId;
        it.parent = parent;
        it.role = r;
        it.title = title;
        it.bounds = b;
        return it;
    }

    // The fixture. `rows` and `cols` are the table's whole size; `shift` moves every listed row's index, as a scroll
    // by whole rows that keeps the same items would.
    std::vector<A11yItem> fixture(int rows = kRows, int cols = kCols, int shift = 0)
    {
        std::vector<A11yItem> v;
        v.push_back(item(kGroup, 0, role::group, "Filter 1, ladder", { 8, 8, 120, 48 }));
        v.push_back(item(kButton, kGroup, A11yRole::button, "Filter 1 In", { 8, 20, 24, 24 }));
        v.push_back(item(kList, 0, role::list, "Connections", { 8, 64, 120, 40 }));
        v.push_back(item(kListItem, kList, A11yRole::listItem, "Osc 1 Out to Filter 1 In", { 8, 64, 120, 20 }));
        A11yItem text = item(kText, 0, role::editableText, "Preset name", { 140, 8, 200, 20 });
        text.value = "Warm Pad";
        v.push_back(text);
        v.push_back(item(kEmptyText, 0, role::editableText, "Search", { 140, 32, 200, 20 }));
        v.push_back(item(kNote, 0, A11yRole::staticText, "Status", { 140, 56, 200, 12 }));

        A11yItem table = item(kTable, 0, role::table, "Modulation matrix", { 0, 120, 688, 420 });
        table.rows = rows;
        table.cols = cols;
        v.push_back(table);
        for (int k = 0; k < kListed; ++k)
        {
            const int r = kFirstListed + k;
            const float y = 120.0f + 20.0f * static_cast<float>(k);
            A11yItem row = item(rowId(r), kTable, role::row, "Route", { 0, y, 688, 20 });
            row.row = r + shift;
            v.push_back(row);
            for (int c = 0; c < kCols; ++c)
            {
                const funkgui::Rect b{ 86.0f * static_cast<float>(c), y, 86.0f, 20.0f };
                A11yItem cell = item(cellId(r, c), rowId(r), role::cell, "Source", b);
                if (c == 0)
                    cell.value = "LFO 1";
                else if (c == kCols - 1)
                {
                    cell.role = A11yRole::button;
                    cell.title = "Remove";
                }
                else
                {
                    cell.role = A11yRole::slider;
                    cell.title = "Depth";
                    cell.value = "25 %";
                    cell.v = 25;
                    cell.lo = 0;
                    cell.hi = 100;
                    cell.step = 1;
                }
                cell.col = c;
                v.push_back(cell);
            }
        }
        return v;
    }

    std::unique_ptr<juce::AccessibilityHandler> handlerOf(const A11yBridge& bridge, uint32_t itemId)
    {
        juce::Component* c = bridge.componentFor(itemId);
        return c != nullptr ? c->createAccessibilityHandler() : nullptr;
    }

    // "focus", "press", "toggle", "menu" for the actions a handler holds, in that order.
    std::string actionsOf(const juce::AccessibilityHandler& h)
    {
        std::string s;
        const auto add = [&](JA type, const char* name) {
            if (h.getActions().contains(type))
                s += std::string(s.empty() ? "" : " ") + name;
        };
        add(JA::focus, "focus");
        add(JA::press, "press");
        add(JA::toggle, "toggle");
        add(JA::showMenu, "menu");
        return s;
    }

    using Span = juce::AccessibilityTableInterface::Span;

    bool spanIs(const juce::Optional<Span>& s, int begin)
    {
        return s.hasValue() && s->begin == begin && s->num == 1;
    }
}

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;          // NSApplication and the message thread, as in a host
    T::Probe P("fg.a11ybridge.table", "", argc, argv);

    LogPanel panel;
    juce::Component editor;                                  // never on screen: no peer, so no handler of JUCE's own
    editor.setSize(688, 560);
    A11yBridge bridge;
    bridge.sync(editor, fixture(), panel);
    P.eq("sync.children", bridge.size(), 8 + kListed * (1 + kCols));
    P.eq("sync.rebuilds", static_cast<int64_t>(bridge.rebuilds()), 1);

    // ---- the six roles: their JUCE role, and the focus action alone -------------------------------------------------
    {
        const struct { const char* key; uint32_t item; JR want; } six[] = {
            { "group", kGroup, JR::group },
            { "list", kList, JR::list },
            { "table", kTable, JR::table },
            { "row", rowId(25), JR::row },
            { "cell", cellId(25, 0), JR::cell },
            { "editable_text", kText, JR::editableText },
        };
        for (const auto& s : six)
        {
            const auto h = handlerOf(bridge, s.item);
            P.eq(std::string("role.") + s.key, h != nullptr && h->getRole() == s.want, 1);
            const std::string held = h != nullptr ? actionsOf(*h) : std::string("?");
            if (held != "focus")
                std::printf("NOTE     actions.%s holds \"%s\"\n", s.key, held.c_str());
            P.eq(std::string("actions.") + s.key + ".focus_only", held == "focus", 1);
        }

        // The focus action is the Panel's: a11yAction(id, focus).
        panel.actions.clear();
        const auto row = handlerOf(bridge, rowId(25));
        const bool invoked = row != nullptr && row->getActions().invoke(JA::focus);
        P.eq("actions.focus_reaches_panel", invoked && panel.actions.size() == 1 && panel.actions[0].first == rowId(25)
                                                && panel.actions[0].second == A11yAction::focus, 1);
    }

    // ---- the roles of v0.14.0 are what they were ---------------------------------------------------------------------
    {
        const struct { A11yRole r; JR want; const char* actions; } ten[] = {
            { A11yRole::slider, JR::slider, "focus menu" },
            { A11yRole::toggleButton, JR::toggleButton, "focus press toggle" },
            { A11yRole::button, JR::button, "focus press" },
            { A11yRole::radioGroup, JR::group, "" },
            { A11yRole::radioButton, JR::radioButton, "focus press" },
            { A11yRole::comboBox, JR::comboBox, "focus press menu" },
            { A11yRole::listItem, JR::listItem, "focus press" },
            { A11yRole::staticText, JR::staticText, "" },
            { A11yRole::image, JR::image, "" },
            { A11yRole::progressBar, JR::progressBar, "" },
        };
        std::vector<A11yItem> items;
        for (int i = 0; i < 10; ++i)
            items.push_back(item(200 + static_cast<uint32_t>(i), 0, ten[i].r, "Item", { 0, 0, 10, 10 }));
        juce::Component host;
        A11yBridge old;
        old.sync(host, items, panel);
        bool roles = true, actions = true;
        for (int i = 0; i < 10; ++i)
        {
            const auto h = handlerOf(old, 200 + static_cast<uint32_t>(i));
            roles = roles && h != nullptr && h->getRole() == ten[i].want;
            const std::string held = h != nullptr ? actionsOf(*h) : std::string("?");
            if (held != ten[i].actions)
                std::printf("NOTE     v0140 role %d holds \"%s\", want \"%s\"\n", i, held.c_str(), ten[i].actions);
            actions = actions && held == ten[i].actions;
        }
        P.eq("v0140.roles", roles, 1);
        P.eq("v0140.actions", actions, 1);

        // A checkable item keeps the toggle action whatever its role, as at v0.14.0.
        A11yItem checkable = item(300, 0, role::group, "Bypassed group", { 0, 0, 10, 10 });
        checkable.checkable = true;
        juce::Component host2;
        A11yBridge one;
        one.sync(host2, { checkable }, panel);
        const auto h = handlerOf(one, 300);
        P.eq("actions.checkable_keeps_toggle", h != nullptr && actionsOf(*h) == "focus toggle", 1);
        P.eq("actions.checkable_state", h != nullptr && h->getCurrentState().isCheckable(), 1);
    }

    // ---- the table interface ----------------------------------------------------------------------------------------
    {
        const auto tableHandler = handlerOf(bridge, kTable);
        const auto* table = tableHandler != nullptr ? tableHandler->getTableInterface() : nullptr;
        if (!P.eq("table.has_interface", table != nullptr, 1))
            return P.finish();
        P.eq("table.rows", table->getNumRows(), kRows);
        P.eq("table.columns", table->getNumColumns(), kCols);
        P.eq("table.no_header", table->getHeaderHandler() == nullptr, 1);
        P.eq("table.only_the_table_has_it",
             handlerOf(bridge, kGroup)->getTableInterface() == nullptr
                 && handlerOf(bridge, kList)->getTableInterface() == nullptr
                 && handlerOf(bridge, rowId(25))->getTableInterface() == nullptr
                 && handlerOf(bridge, cellId(25, 3))->getTableInterface() == nullptr, 1);

        // Spans, from handlers of the listed rows and cells: the first and the last listed row, and one between.
        bool rowSpans = true, rowsHaveNoColumn = true, cellRows = true, cellColumns = true;
        for (const int r : { kFirstListed, 25, kFirstListed + kListed - 1 })
        {
            const auto rowHandler = handlerOf(bridge, rowId(r));
            rowSpans = rowSpans && spanIs(table->getRowSpan(*rowHandler), r);
            rowsHaveNoColumn = rowsHaveNoColumn && !table->getColumnSpan(*rowHandler).hasValue();
            for (int c = 0; c < kCols; ++c)                  // a `cell`, six sliders and a button: all are cells
            {
                const auto cellHandler = handlerOf(bridge, cellId(r, c));
                cellRows = cellRows && spanIs(table->getRowSpan(*cellHandler), r);
                cellColumns = cellColumns && spanIs(table->getColumnSpan(*cellHandler), c);
            }
        }
        P.eq("table.span.row", rowSpans, 1);
        P.eq("table.span.row_has_no_column", rowsHaveNoColumn, 1);
        P.eq("table.span.cell_row", cellRows, 1);
        P.eq("table.span.cell_column", cellColumns, 1);
        const auto group = handlerOf(bridge, kGroup);
        const auto button = handlerOf(bridge, kButton);
        P.eq("table.span.outside_the_table", !table->getRowSpan(*group).hasValue()
                                                 && !table->getColumnSpan(*group).hasValue()
                                                 && !table->getRowSpan(*button).hasValue()
                                                 && !table->getColumnSpan(*button).hasValue(), 1);
        P.eq("table.span.the_table_itself", !table->getRowSpan(*tableHandler).hasValue()
                                                && !table->getColumnSpan(*tableHandler).hasValue(), 1);

        // A row that is not listed has no handler; nor has a column that does not exist, or a negative index.
        P.eq("table.unlisted.row_before", table->getRowHandler(5) == nullptr, 1);
        P.eq("table.unlisted.row_after", table->getRowHandler(kFirstListed + kListed) == nullptr, 1);
        P.eq("table.unlisted.cell", table->getCellHandler(5, 3) == nullptr && table->getCellHandler(63, 0) == nullptr, 1);
        P.eq("table.unlisted.beyond_the_table", table->getRowHandler(kRows) == nullptr
                                                    && table->getCellHandler(25, kCols) == nullptr, 1);
        P.eq("table.unlisted.negative", table->getRowHandler(-1) == nullptr && table->getCellHandler(-1, 0) == nullptr
                                            && table->getCellHandler(25, -1) == nullptr, 1);
        // Without a window JUCE gives a component no handler, so a listed row's lookup is null here too.
        P.eq("table.no_peer.listed_row", table->getRowHandler(25) == nullptr, 1);
        P.eq("table.no_peer.listed_cell", table->getCellHandler(25, 3) == nullptr, 1);
        P.eq("table.no_peer.component_has_no_handler",
             bridge.componentFor(rowId(25))->getAccessibilityHandler() == nullptr, 1);

        // showCell: the Panel is asked to focus the cell, which is how a product scrolls it into view.
        panel.actions.clear();
        const auto cell = handlerOf(bridge, cellId(30, 4));
        table->showCell(*cell);
        P.eq("table.show_cell", panel.actions.size() == 1 && panel.actions[0].first == cellId(30, 4)
                                    && panel.actions[0].second == A11yAction::focus, 1);
        table->showCell(*group);                             // not a cell of this table: nothing
        P.eq("table.show_cell_outside", panel.actions.size() == 1, 1);
    }

    // ---- the cell interface of a table's rows and cells -------------------------------------------------------------
    {
        const auto row = handlerOf(bridge, rowId(25));
        const auto textCell = handlerOf(bridge, cellId(25, 0));
        const auto sliderCell = handlerOf(bridge, cellId(25, 3));
        const auto buttonCell = handlerOf(bridge, cellId(25, 7));
        P.eq("cell.row_has_interface", row->getCellInterface() != nullptr, 1);
        P.eq("cell.cells_have_interface", textCell->getCellInterface() != nullptr
                                              && sliderCell->getCellInterface() != nullptr
                                              && buttonCell->getCellInterface() != nullptr, 1);
        P.eq("cell.flat", row->getCellInterface() != nullptr && row->getCellInterface()->getDisclosureLevel() == 0
                              && row->getCellInterface()->getDisclosedRows().empty(), 1);
        P.eq("cell.no_peer.table_handler", row->getCellInterface() != nullptr
                                               && row->getCellInterface()->getTableHandler() == nullptr, 1);
        P.eq("cell.others_have_none", handlerOf(bridge, kTable)->getCellInterface() == nullptr
                                          && handlerOf(bridge, kGroup)->getCellInterface() == nullptr
                                          && handlerOf(bridge, kButton)->getCellInterface() == nullptr
                                          && handlerOf(bridge, kListItem)->getCellInterface() == nullptr, 1);
        // A control that is a cell stays the control: its role, its value interface, its own actions.
        P.eq("cell.slider_stays_a_slider", sliderCell->getRole() == JR::slider && actionsOf(*sliderCell) == "focus menu"
                                               && sliderCell->getValueInterface() != nullptr
                                               && !sliderCell->getValueInterface()->isReadOnly()
                                               && sliderCell->getValueInterface()->getCurrentValue() == 25.0, 1);
        P.eq("cell.button_stays_a_button", buttonCell->getRole() == JR::button && actionsOf(*buttonCell) == "focus press",
             1);
        P.eq("cell.text_cell_value", textCell->getValueInterface() != nullptr
                                         && textCell->getValueInterface()->isReadOnly()
                                         && textCell->getValueInterface()->getCurrentValueAsString() == "LFO 1", 1);
    }

    // ---- nesting by component parent --------------------------------------------------------------------------------
    {
        const auto parentOf = [&](uint32_t itemId) { return bridge.componentFor(itemId)->getParentComponent(); };
        P.eq("nest.group_button", parentOf(kButton) == bridge.componentFor(kGroup), 1);
        P.eq("nest.list_item", parentOf(kListItem) == bridge.componentFor(kList), 1);
        P.eq("nest.table_row", parentOf(rowId(25)) == bridge.componentFor(kTable), 1);
        P.eq("nest.row_cell", parentOf(cellId(25, 0)) == bridge.componentFor(rowId(25))
                                  && parentOf(cellId(25, 7)) == bridge.componentFor(rowId(25)), 1);
        P.eq("nest.top_level", parentOf(kGroup) == &editor && parentOf(kList) == &editor && parentOf(kTable) == &editor
                                   && parentOf(kText) == &editor, 1);
        // A nested child's bounds are relative to its parent's: row 25 is the sixth listed row, 100 px down the table.
        const auto rowBounds = bridge.componentFor(rowId(25))->getBounds();
        const auto cellBounds = bridge.componentFor(cellId(25, 2))->getBounds();
        P.eq("nest.row_bounds", rowBounds == juce::Rectangle<int>(0, 100, 688, 20), 1);
        P.eq("nest.cell_bounds", cellBounds == juce::Rectangle<int>(172, 0, 86, 20), 1);
    }

    // ---- focus: JUCE's accessibility focus, never the keyboard's ----------------------------------------------------
    {
        bool grabbed = true, noKeyboard = true;
        for (const uint32_t itemId : { kButton, kGroup, rowId(25), cellId(25, 3), kText })
        {
            const auto h = handlerOf(bridge, itemId);
            h->grabFocus();
            grabbed = grabbed && h->hasFocus(false);
            noKeyboard = noKeyboard && !bridge.componentFor(itemId)->hasKeyboardFocus(true)
                      && !bridge.componentFor(itemId)->getWantsKeyboardFocus();
        }
        P.eq("focus.grabbed", grabbed, 1);
        P.eq("focus.keyboard_focus_untouched", noKeyboard && !editor.hasKeyboardFocus(true), 1);
        // The handler that held the focus is gone: so is the focus (a rebuild drops it the same way).
        const auto other = handlerOf(bridge, kGroup);
        P.eq("focus.gone_with_the_handler", !other->hasFocus(true), 1);
    }

    // ---- editableText: a read-only text value, always -----------------------------------------------------------------
    {
        const auto text = handlerOf(bridge, kText);
        const auto* v = text->getValueInterface();
        P.eq("text.has_value_interface", v != nullptr, 1);
        P.eq("text.read_only", v != nullptr && v->isReadOnly(), 1);
        P.eq("text.value", v != nullptr && v->getCurrentValueAsString() == "Warm Pad", 1);
        const auto empty = handlerOf(bridge, kEmptyText);
        const auto* ve = empty->getValueInterface();
        P.eq("text.empty_has_value_interface", ve != nullptr && ve->isReadOnly(), 1);
        P.eq("text.empty_value", ve != nullptr && ve->getCurrentValueAsString().isEmpty(), 1);
        // Writing through the bridge does nothing: typing reaches the Panel as keys.
        panel.actions.clear();
        if (v != nullptr)
            text->getValueInterface()->setValueAsString("Changed");
        P.eq("text.write_ignored", panel.actions.empty() && bridge.items()[4].value == "Warm Pad", 1);
        // An item of another role without a value has no value interface, as at v0.14.0.
        P.eq("text.other_roles_need_a_value", handlerOf(bridge, kNote)->getValueInterface() == nullptr
                                                  && handlerOf(bridge, kGroup)->getValueInterface() == nullptr, 1);
    }

    // ---- a change of the table fields alone is an in-place update ---------------------------------------------------
    {
        juce::Component* const tableBefore = bridge.componentFor(kTable);
        juce::Component* const rowBefore = bridge.componentFor(rowId(25));
        bridge.sync(editor, fixture(96, 9, 3), panel);       // a longer, wider table, every listed row 3 further down
        P.eq("place.no_rebuild", static_cast<int64_t>(bridge.rebuilds()), 1);
        P.eq("place.same_components", bridge.componentFor(kTable) == tableBefore
                                          && bridge.componentFor(rowId(25)) == rowBefore, 1);
        const auto tableHandler = handlerOf(bridge, kTable);
        const auto* table = tableHandler->getTableInterface();
        P.eq("place.new_size", table != nullptr && table->getNumRows() == 96 && table->getNumColumns() == 9, 1);
        const auto row = handlerOf(bridge, rowId(25));
        const auto cell = handlerOf(bridge, cellId(25, 3));
        P.eq("place.new_row_index", table != nullptr && spanIs(table->getRowSpan(*row), 28)
                                        && spanIs(table->getRowSpan(*cell), 28) && spanIs(table->getColumnSpan(*cell), 3),
             1);
        P.eq("place.items_as_synced", bridge.items()[7].rows == 96 && bridge.items()[7].cols == 9, 1);
        P.eq("place.no_handler_dropped", bridge.handlerEpoch(kTable) == 0 && bridge.handlerEpoch(rowId(25)) == 0, 1);
        bridge.sync(editor, fixture(), panel);
        P.eq("place.back.no_rebuild", static_cast<int64_t>(bridge.rebuilds()), 1);
    }

    // ---- handlerEpoch ------------------------------------------------------------------------------------------------
    {
        P.eq("epoch.initially_zero", bridge.handlerEpoch(kText) == 0 && bridge.handlerEpoch(kEmptyText) == 0
                                         && bridge.handlerEpoch(kNote) == 0, 1);
        P.eq("epoch.unknown_id", static_cast<int64_t>(bridge.handlerEpoch(999999)), 0);
        // Typing the first character into an empty text field, and deleting the last: the handler stays.
        std::vector<A11yItem> items = fixture();
        items[5].value = "W";
        bridge.sync(editor, items, panel);
        items[4].value.clear();
        bridge.sync(editor, items, panel);
        P.eq("epoch.editable_text_keeps_its_handler",
             bridge.handlerEpoch(kEmptyText) == 0 && bridge.handlerEpoch(kText) == 0, 1);
        const auto typed = handlerOf(bridge, kEmptyText);
        const auto emptied = handlerOf(bridge, kText);
        P.eq("epoch.editable_text_values", typed->getValueInterface()->getCurrentValueAsString() == "W"
                                               && emptied->getValueInterface()->getCurrentValueAsString().isEmpty(), 1);
        // A value appearing on an item of another role changes its handler's shape, as at v0.14.0: one drop.
        items[6].value = "Saved";
        bridge.sync(editor, items, panel);
        P.eq("epoch.value_appearing_drops_one", static_cast<int64_t>(bridge.handlerEpoch(kNote)), 1);
        items[6].value = "Saved again";
        bridge.sync(editor, items, panel);
        P.eq("epoch.value_changing_drops_none", static_cast<int64_t>(bridge.handlerEpoch(kNote)), 1);
        items[6].value.clear();
        bridge.sync(editor, items, panel);
        P.eq("epoch.value_going_drops_another", static_cast<int64_t>(bridge.handlerEpoch(kNote)), 2);
        P.eq("epoch.no_rebuild_so_far", static_cast<int64_t>(bridge.rebuilds()), 1);
        // A structure change rebuilds every child: the counts start again.
        items.pop_back();                                    // the last listed row's last cell goes
        bridge.sync(editor, items, panel);
        P.eq("epoch.rebuild", static_cast<int64_t>(bridge.rebuilds()), 2);
        P.eq("epoch.zero_after_rebuild", static_cast<int64_t>(bridge.handlerEpoch(kNote)), 0);
    }

    bridge.clear();
    return P.finish();
}

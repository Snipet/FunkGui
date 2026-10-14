// FUNKGUI_TEST name=fg.gallery.a11ytable timeout=600 gpu=0 exe=FunkGuiGalleryProbe args="--section a11ytable"
//
// The "a11ytable" gallery section (FSynth G1): the six a11yrole constants and A11yItem's table fields
// (a11y/A11yItem.h), listed as a product lists them, and HostServices::a11yFocus and announce as a product calls them.
//
// - A group of three buttons: a node of a graph, "Filter 1", with its two ports and "Open in inspector". The group
//   is one Tab stop; left and right move between its buttons.
// - A list of five items: "Connections". Up and down move between them. Return opens the focused connection, which
//   then lists its two children, an "Amount" slider and a "Remove" button, nested under the list item, and moves the
//   focus to the slider; left and right move between the connection and its two children, and Return closes it
//   again. The Panel's a11yRevision() moves with every open and close.
// - A 4 x 3 table whose cells are sliders: "Routes", one `row` item per route, each with three slider cells that give
//   their column (Depth, Lag, Shape). One Tab stop; the arrows move a drawn focus cell, clamped at the edges. The
//   table item gives the whole table's size, 4 x 3.
// - An editableText item and a staticText readout that names the focused item, so a `lines` sidecar shows where the
//   focus is.
// Tab and Shift-Tab walk the three stops. Every focus move asks the host to move the screen reader's cursor
// (a11yFocus with the focused item's id) and to say where the focus is (announce): HeadlessHost logs both,
// EditorHost does both, a web host does neither. Opening a connection is the hard case for EditorHost, twice over:
// the key press that opens it asks for the focus of an item it has only just listed, which has no component until
// the next frame syncs the bridge; and that sync rebuilds the bridge, which destroys the handler the screen reader's
// cursor was on. Closing it rebuilds the bridge under the focused connection. Run this section in the gallery app
// under VoiceOver to hear the cursor follow.
//
// States: rest (nothing focused); keys (Tab to the table, then down, down, right: the focus ring on route 3's Lag);
// open (Tab to the list, down, Return: the second connection open, its two children listed, the focus ring on its
// Amount). The fingerprint and the a11y lines of each are goldens. No JUCE and no platform code: the section also
// compiles into the gallery app and, under Emscripten, the web gallery and the web host's page. The line above
// registers the test; the tools glob compiles this file into FunkGuiGalleryProbe. Its text names generic nodes and
// controls (Source A, Filter 1, Amount), never a product's Modes: its a11y lines become goldens.

#include "GalleryPanel.h"

#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Col.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/widgets/FocusRing.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace funkgui::gallery
{
    namespace
    {
        // ---- geometry ------------------------------------------------------------------------------------------------
        constexpr Rect kGroup{ 16.0f, 16.0f, 184.0f, 56.0f };
        constexpr int  kButtons = 3;
        constexpr std::array<Rect, kButtons> kButton{ { { 24.0f, 44.0f, 36.0f, 18.0f }, { 68.0f, 44.0f, 36.0f, 18.0f },
                                                        { 112.0f, 44.0f, 80.0f, 18.0f } } };
        constexpr std::array<const char*, kButtons> kButtonText{ { "IN", "OUT", "INSPECT" } };
        constexpr std::array<const char*, kButtons> kButtonTitle{ { "Filter 1 In", "Filter 1 Out", "Open in inspector" } };

        constexpr int   kConnections = 5;
        constexpr float kListX = 16.0f, kListY = 100.0f, kListW = 184.0f, kListRowH = 18.0f;
        constexpr Rect  kList{ kListX, 84.0f, kListW, 16.0f + kListRowH * static_cast<float>(kConnections) };
        // Generic node names, no product's Mode names (the drawn text abbreviates what is spoken).
        constexpr std::array<const char*, kConnections> kConnText{
            { "SRC A \xE2\x86\x92 FILTER 1", "SRC B \xE2\x86\x92 FILTER 1", "FILTER 1 \xE2\x86\x92 MIX A",
              "SRC C \xE2\x86\x92 MIX A", "MIX A \xE2\x86\x92 BUS 1" }
        };
        constexpr std::array<const char*, kConnections> kConnTitle{
            { "Source A Out to Filter 1 In", "Source B Out to Filter 1 In", "Filter 1 Out to Mix A In 1",
              "Source C Out to Mix A In 2", "Mix A Out to Bus 1 In" }
        };
        constexpr std::array<int, kConnections> kConnAmount{ { 100, 50, 100, 25, 80 } };

        constexpr int   kRows = 4, kCols = 3;
        constexpr float kTableX = 216.0f, kTableY = 48.0f, kRowH = 24.0f, kCellX = 284.0f, kCellPitch = 58.0f,
                        kCellW = 52.0f;
        constexpr Rect  kTable{ kTableX, 32.0f, 236.0f + 4.0f, 16.0f + kRowH * static_cast<float>(kRows) };
        constexpr std::array<const char*, kCols> kColText{ { "DEPTH", "LAG", "SHAPE" } };
        constexpr std::array<const char*, kCols> kColTitle{ { "Depth", "Lag", "Shape" } };
        constexpr std::array<const char*, kRows> kRouteTitle{ { "Source A to Cutoff", "Source B to Pitch",
                                                                "Velocity to Level", "Macro 1 to Width" } };
        // Percent, 0 .. 100, per route and column.
        constexpr int kCell[kRows][kCols] = { { 25, 0, 50 }, { 60, 10, 50 }, { 100, 35, 75 }, { 40, 5, 20 } };

        constexpr Rect kName{ 216.0f, 160.0f, 236.0f, 18.0f };
        constexpr Rect kReadout{ 216.0f, 182.0f, 236.0f, 12.0f };

        // ---- ids -----------------------------------------------------------------------------------------------------
        constexpr uint32_t kIdGroup = 1, kIdButton = 2;                  // 2, 3, 4
        constexpr uint32_t kIdList = 10, kIdConn = 11;                   // 11 .. 15
        constexpr uint32_t kIdAmount = 111, kIdRemove = 112;             // the open connection's children
        constexpr uint32_t kIdTable = 20, kIdRow = 21;                   // 21 .. 24
        constexpr uint32_t kIdCell = 30;                                 // 30 + row * 3 + col
        constexpr uint32_t kIdName = 50, kIdReadout = 51;
        constexpr uint32_t kButtonIds = kButtons, kConnIds = kConnections, kCellIds = kRows * kCols;

        constexpr Rect buttonRect(int i) { return kButton[static_cast<size_t>(i)]; }
        constexpr Rect connRect(int i)
        {
            return { kListX, kListY + kListRowH * static_cast<float>(i), kListW, kListRowH };
        }
        constexpr Rect rowRect(int r)
        {
            return { kTableX, kTableY + kRowH * static_cast<float>(r), 240.0f, kRowH };
        }
        constexpr Rect cellRect(int r, int c)
        {
            return { kCellX + kCellPitch * static_cast<float>(c), kTableY + kRowH * static_cast<float>(r), kCellW, kRowH };
        }
        constexpr Rect amountRect(int i)
        {
            const Rect r = connRect(i);
            return { r.x + 116.0f, r.y, 40.0f, r.h };
        }
        constexpr Rect removeRect(int i)
        {
            const Rect r = connRect(i);
            return { r.x + 160.0f, r.y, 24.0f, r.h };
        }

        std::string percent(int v)
        {
            char buf[16];
            std::snprintf(buf, sizeof buf, "%d %%", v);
            return buf;
        }

        enum class Stop : uint8_t { none, group, list, table };

        class A11yTableSection final : public Section
        {
        public:
            A11yTableSection() : Section(480, 200)
            {
                for (int r = 0; r < kRows; ++r)
                    for (int c = 0; c < kCols; ++c)
                        cell_[static_cast<size_t>(r)][static_cast<size_t>(c)] = kCell[r][c];
                for (int i = 0; i < kConnections; ++i)
                    amount_[static_cast<size_t>(i)] = kConnAmount[static_cast<size_t>(i)];
            }

            uint32_t a11yRevision() const override { return revision_; }

            bool key(const KeyEvent& e) override
            {
                if (e.key == Key::tab)
                {
                    static constexpr Stop order[3] = { Stop::group, Stop::list, Stop::table };
                    int at = stop_ == Stop::none ? (e.mods.shift ? 0 : 2) : static_cast<int>(stop_) - 1;
                    at = (at + (e.mods.shift ? 2 : 1)) % 3;
                    stop_ = order[at];
                    focusMoved();
                    return true;
                }
                switch (stop_)
                {
                    case Stop::none:
                        return false;
                    case Stop::group:
                        if (e.key != Key::left && e.key != Key::right)
                            return false;
                        button_ = std::clamp(button_ + (e.key == Key::right ? 1 : -1), 0, kButtons - 1);
                        break;
                    case Stop::list:
                        if (e.key == Key::enter)
                        {
                            // Open or close the focused connection: its two children appear or go, so the structure
                            // of the list changes and the revision moves. Opening moves the focus to the Amount
                            // slider, an item this very key press lists; closing leaves it on the connection.
                            const bool opening = open_ != conn_;
                            open_ = opening ? conn_ : -1;
                            part_ = opening ? 1 : 0;
                            ++revision_;
                            break;
                        }
                        if ((e.key == Key::left || e.key == Key::right) && open_ == conn_)
                        {
                            part_ = std::clamp(part_ + (e.key == Key::right ? 1 : -1), 0, 2);
                            break;
                        }
                        if (e.key != Key::up && e.key != Key::down)
                            return false;
                        conn_ = std::clamp(conn_ + (e.key == Key::down ? 1 : -1), 0, kConnections - 1);
                        part_ = 0;
                        break;
                    case Stop::table:
                        if (e.key == Key::left || e.key == Key::right)
                            col_ = std::clamp(col_ + (e.key == Key::right ? 1 : -1), 0, kCols - 1);
                        else if (e.key == Key::up || e.key == Key::down)
                            row_ = std::clamp(row_ + (e.key == Key::down ? 1 : -1), 0, kRows - 1);
                        else
                            return false;
                        break;
                }
                focusMoved();
                return true;
            }

            void accessibility(std::vector<A11yItem>& out) const override
            {
                // The group and its three buttons, nested by `parent`.
                A11yItem group;
                group.id = kIdGroup;
                group.role = a11yrole::group;
                group.bounds = kGroup;
                group.title = "Filter 1";
                group.help = "Slot 1";
                out.push_back(group);
                for (int i = 0; i < kButtons; ++i)
                {
                    A11yItem b;
                    b.id = kIdButton + static_cast<uint32_t>(i);
                    b.parent = kIdGroup;
                    b.role = A11yRole::button;
                    b.bounds = buttonRect(i);
                    b.title = kButtonTitle[static_cast<size_t>(i)];
                    out.push_back(b);
                }

                // The list and its five items; the open one lists its slider and its button under itself.
                A11yItem list;
                list.id = kIdList;
                list.role = a11yrole::list;
                list.bounds = kList;
                list.title = "Connections";
                out.push_back(list);
                for (int i = 0; i < kConnections; ++i)
                {
                    A11yItem it;
                    it.id = kIdConn + static_cast<uint32_t>(i);
                    it.parent = kIdList;
                    it.role = A11yRole::listItem;
                    it.bounds = connRect(i);
                    it.title = kConnTitle[static_cast<size_t>(i)];
                    it.value = percent(amount_[static_cast<size_t>(i)]);
                    out.push_back(it);
                    if (i != open_)
                        continue;
                    A11yItem amount;
                    amount.id = kIdAmount;
                    amount.parent = it.id;
                    amount.role = A11yRole::slider;
                    amount.bounds = amountRect(i);
                    amount.title = "Amount";
                    amount.value = percent(amount_[static_cast<size_t>(i)]);
                    amount.v = amount_[static_cast<size_t>(i)];
                    amount.lo = 0;
                    amount.hi = 100;
                    amount.step = 1;
                    out.push_back(amount);
                    A11yItem remove;
                    remove.id = kIdRemove;
                    remove.parent = it.id;
                    remove.role = A11yRole::button;
                    remove.bounds = removeRect(i);
                    remove.title = "Remove";
                    out.push_back(remove);
                }

                // The table: its whole size, a row item per route, three slider cells per row with their column.
                A11yItem table;
                table.id = kIdTable;
                table.role = a11yrole::table;
                table.bounds = kTable;
                table.title = "Routes";
                table.rows = kRows;
                table.cols = kCols;
                out.push_back(table);
                for (int r = 0; r < kRows; ++r)
                {
                    A11yItem row;
                    row.id = kIdRow + static_cast<uint32_t>(r);
                    row.parent = kIdTable;
                    row.role = a11yrole::row;
                    row.bounds = rowRect(r);
                    row.title = kRouteTitle[static_cast<size_t>(r)];
                    row.row = r;
                    out.push_back(row);
                    for (int c = 0; c < kCols; ++c)
                    {
                        const int v = cell_[static_cast<size_t>(r)][static_cast<size_t>(c)];
                        A11yItem cell;
                        cell.id = cellId(r, c);
                        cell.parent = row.id;
                        cell.role = A11yRole::slider;
                        cell.bounds = cellRect(r, c);
                        cell.title = kColTitle[static_cast<size_t>(c)];
                        cell.value = percent(v);
                        cell.v = v;
                        cell.lo = 0;
                        cell.hi = 100;
                        cell.step = 1;
                        cell.col = c;
                        out.push_back(cell);
                    }
                }

                // A text field (its value is its text), and the readout of where the focus is.
                A11yItem name;
                name.id = kIdName;
                name.role = a11yrole::editableText;
                name.bounds = kName;
                name.title = "Patch name";
                name.value = "Init Patch";
                out.push_back(name);
                A11yItem readout;
                readout.id = kIdReadout;
                readout.role = A11yRole::staticText;
                readout.bounds = kReadout;
                readout.title = "Focus";
                readout.value = focusText();
                out.push_back(readout);
            }

            void a11yAction(uint32_t id, A11yAction action, double value) override
            {
                // The screen reader moved its cursor to an item, or changed a slider: the Panel follows.
                const bool isCell = id >= kIdCell && id < kIdCell + kCellIds;
                const int  cell = isCell ? static_cast<int>(id - kIdCell) : 0;
                if (action == A11yAction::focus)
                {
                    if (id >= kIdButton && id < kIdButton + kButtonIds)
                    {
                        stop_ = Stop::group;
                        button_ = static_cast<int>(id - kIdButton);
                    }
                    else if (id >= kIdConn && id < kIdConn + kConnIds)
                    {
                        stop_ = Stop::list;
                        conn_ = static_cast<int>(id - kIdConn);
                        part_ = 0;
                    }
                    else if ((id == kIdAmount || id == kIdRemove) && open_ >= 0)
                    {
                        stop_ = Stop::list;
                        conn_ = open_;
                        part_ = id == kIdAmount ? 1 : 2;
                    }
                    else if (isCell)
                    {
                        stop_ = Stop::table;
                        row_ = cell / kCols;
                        col_ = cell % kCols;
                    }
                    return;
                }
                if (action != A11yAction::setValue || !(value >= 0.0 && value <= 100.0))
                    return;
                const int v = static_cast<int>(value + 0.5);
                if (id == kIdAmount && open_ >= 0)
                    amount_[static_cast<size_t>(open_)] = v;
                else if (isCell)
                    cell_[static_cast<size_t>(cell / kCols)][static_cast<size_t>(cell % kCols)] = v;
            }

            void draw(Canvas& c, const Theme& th) override
            {
                // The group: an outlined box, its title, its three buttons.
                c.rrect(kGroup.x, kGroup.y, kGroup.w, kGroup.h, 3.0f, th.ground, 1.0f, th.ink32);
                label(c, "FILTER 1", kGroup.x + 8.0f, kGroup.y + 8.0f, th.ink70);
                for (int i = 0; i < kButtons; ++i)
                {
                    const Rect r = buttonRect(i);
                    c.rrect(r.x, r.y, r.w, r.h, 2.0f, th.ground, 1.0f, th.ink52);
                    label(c, kButtonText[static_cast<size_t>(i)], r.centreX(), r.y + 4.0f, th.ink100, Align::centre);
                }

                // The list: a title and five rows; the open row shows its amount bar and its remove button.
                label(c, "CONNECTIONS", kListX, kList.y, th.ink52);
                for (int i = 0; i < kConnections; ++i)
                {
                    const Rect r = connRect(i);
                    c.hairlineH(r.x, r.y, r.w, th.ink16);
                    label(c, kConnText[static_cast<size_t>(i)], r.x + 4.0f, r.y + 4.0f, th.ink100);
                    if (i != open_)
                        continue;
                    bar(c, th, amountRect(i), amount_[static_cast<size_t>(i)]);
                    const Rect x = removeRect(i);
                    c.rrect(x.x + 4.0f, x.y + 2.0f, 16.0f, x.h - 4.0f, 2.0f, th.ground, 1.0f, th.ink52);
                    label(c, "\xC3\x97", x.x + 12.0f, x.y + 4.0f, th.ink100, Align::centre);
                }

                // The table: column heads, a rule and a label per route, a value bar per cell.
                label(c, "ROUTES", kTableX, 16.0f, th.ink52);
                for (int col = 0; col < kCols; ++col)
                    label(c, kColText[static_cast<size_t>(col)], cellRect(0, col).x, 34.0f, th.ink52);
                for (int r = 0; r < kRows; ++r)
                {
                    const Rect row = rowRect(r);
                    c.hairlineH(row.x, row.y, row.w, th.ink16);
                    char name[16];
                    std::snprintf(name, sizeof name, "ROUTE %d", r + 1);
                    label(c, name, row.x, row.y + 7.0f, th.ink70);
                    for (int col = 0; col < kCols; ++col)
                        bar(c, th, cellRect(r, col), cell_[static_cast<size_t>(r)][static_cast<size_t>(col)]);
                }

                // The text field and the readout.
                c.rrect(kName.x, kName.y, kName.w, kName.h, 2.0f, th.ground, 1.0f, th.ink32);
                label(c, "INIT PATCH", kName.x + 6.0f, kName.y + 4.0f, th.ink100);
                {
                    // The longest line (a connection's title and ", REMOVE") fits the panel in the micro style.
                    const Canvas::Scope s(c, tags::slotSub, false);
                    c.text(("FOCUS  " + upper(focusText())).c_str(), kReadout.x, kReadout.y, type::kMicro, th.ink32);
                }

                // The focus ring, on the focused item of the focused stop.
                switch (stop_)
                {
                    case Stop::none:  break;
                    case Stop::group: drawFocusRing(c, buttonRect(button_), th.accent); break;
                    case Stop::list:  drawFocusRing(c, listFocusRect(), th.accent); break;
                    case Stop::table: drawFocusRing(c, cellRect(row_, col_), th.accent); break;
                }
            }

        private:
            static constexpr uint32_t cellId(int r, int c)
            {
                return kIdCell + static_cast<uint32_t>(r * kCols + c);
            }

            static void label(Canvas& c, const char* text, float x, float y, Col ink, Align align = Align::left)
            {
                const Canvas::Scope s(c, tags::slotLabel, false);
                c.text(text, x, y, type::kCaption, ink, align);
            }

            // A value as a bar: a 1 px track with the filled part over it, and the number beside it.
            static void bar(Canvas& c, const Theme& th, Rect r, int percentValue)
            {
                const float w = r.w - 8.0f, x = r.x + 4.0f, y = r.bottom() - 6.0f;
                c.hairlineH(x, y, w, th.ink32);
                c.rrect(x, y - 1.0f, w * static_cast<float>(percentValue) / 100.0f, 3.0f, 0.0f, th.ink100);
                char num[8];
                std::snprintf(num, sizeof num, "%d", percentValue);
                const Canvas::Scope s(c, tags::slotValue, false);
                c.text(num, x, r.y + 3.0f, type::kMicro, th.ink70);
            }

            static std::string upper(std::string s)
            {
                for (char& ch : s)
                    if (ch >= 'a' && ch <= 'z')
                        ch = static_cast<char>(ch - 'a' + 'A');
                return s;
            }

            // The part of the list that has the focus: the connection, or a child of the open one.
            int  listPart() const { return open_ == conn_ ? part_ : 0; }
            Rect listFocusRect() const
            {
                const int part = listPart();
                return part == 1 ? amountRect(conn_) : (part == 2 ? removeRect(conn_) : connRect(conn_));
            }

            uint32_t focusId() const
            {
                switch (stop_)
                {
                    case Stop::none:  return 0;
                    case Stop::group: return kIdButton + static_cast<uint32_t>(button_);
                    case Stop::list:
                    {
                        const int part = listPart();
                        return part == 1 ? kIdAmount : (part == 2 ? kIdRemove : kIdConn + static_cast<uint32_t>(conn_));
                    }
                    case Stop::table: return cellId(row_, col_);
                }
                return 0;
            }

            // What the readout shows and the host is asked to say: where the focus is.
            std::string focusText() const
            {
                switch (stop_)
                {
                    case Stop::none:
                        return "none";
                    case Stop::group:
                        return kButtonTitle[static_cast<size_t>(button_)];
                    case Stop::list:
                    {
                        static constexpr const char* kPart[3] = { ", open", ", Amount", ", Remove" };
                        return std::string(kConnTitle[static_cast<size_t>(conn_)])
                             + (open_ == conn_ ? kPart[listPart()] : "");
                    }
                    case Stop::table:
                        return std::string(kRouteTitle[static_cast<size_t>(row_)]) + ", "
                             + kColTitle[static_cast<size_t>(col_)];
                }
                return "none";
            }

            // The keyboard focus moved (or the focused connection opened or closed): the screen reader's cursor
            // follows, and the item is spoken. The keyboard focus itself is this Panel's own state.
            void focusMoved()
            {
                if (HostServices* h = host())
                {
                    h->a11yFocus(focusId());
                    h->announce(focusText());
                }
            }

            Stop stop_ = Stop::none;
            int  button_ = 0, conn_ = 0, row_ = 0, col_ = 0;
            int  open_ = -1;                                 // the open connection, or -1
            int  part_ = 0;                                  // in the open connection: 0 itself, 1 Amount, 2 Remove
            uint32_t revision_ = 0;
            std::array<std::array<int, kCols>, kRows> cell_{};
            std::array<int, kConnections> amount_{};
        };

        const Registration kA11yTable{ SectionInfo{
            "a11ytable",
            [] { return std::make_unique<A11yTableSection>(); },
            {
                State{ "rest", {} },
                State{ "keys", [](HeadlessHost& h, Panel&) { h.keys("tab,tab,tab,down,down,right"); } },
                State{ "open", [](HeadlessHost& h, Panel&) { h.keys("tab,tab,down,return"); } },
            } } };
    }
}

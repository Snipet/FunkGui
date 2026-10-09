// FUNKGUI_TEST name=fg.a11y.roles timeout=120 gpu=0
//
// fg.a11y.roles (FSynth G1): the six a11yrole constants and A11yItem's table fields (a11y/A11yItem.h), and what
// a11yDumpLine prints for them. Spec rows only; no JUCE, so it runs in every configuration.
//
// - values.*   the constants are 10 to 15, of the type A11yRole, usable in constant expressions and as an item's
//              role; the ten enumerators keep 0 to 9.
// - switch.*   a `switch` over the ten enumerators with no `default` still compiles: enumeratorName() below is one,
//              and this file builds under -Wall -Werror, whose -Wswitch names every enumerator a switch leaves out.
//              It compiles only while A11yRole has no eleventh enumerator, which is the guard for a consumer's
//              exhaustive switch (FCompressor's ui.input probe has two, under -Wswitch-enum). At run time a constant
//              falls through such a switch to the code after it.
// - line.*     the dump line of each new role, and of each table field: " table=<rows>x<cols>", " row=<r>" and
//              " col=<c>" after help=, each only when set; the role prints as its number, in the two columns the line
//              has always had.
// - v0140.*    an item that sets no table field prints exactly the line v0.14.0 printed: the lines below are written
//              out in full, as that version printed them.
// - defaults.* the fields' unset values.

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/test/Harness.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <type_traits>

namespace T = funkgui::test;
using funkgui::A11yItem;
using funkgui::A11yRole;
namespace role = funkgui::a11yrole;

namespace
{
    // The constants are constant expressions of the enum's own type.
    static_assert(std::is_same_v<decltype(role::group), const A11yRole>);
    static_assert(std::is_same_v<std::underlying_type_t<A11yRole>, uint8_t>);
    static_assert(static_cast<int>(role::group) == 10 && static_cast<int>(role::list) == 11
                  && static_cast<int>(role::table) == 12 && static_cast<int>(role::row) == 13
                  && static_cast<int>(role::cell) == 14 && static_cast<int>(role::editableText) == 15);
    static_assert(role::table != role::row && role::group != A11yRole::radioGroup);

    // The guard: every enumerator of A11yRole, and no default. An eleventh enumerator would stop this file's build.
    const char* enumeratorName(A11yRole r)
    {
        switch (r)
        {
            case A11yRole::slider:       return "slider";
            case A11yRole::toggleButton: return "toggleButton";
            case A11yRole::button:       return "button";
            case A11yRole::radioGroup:   return "radioGroup";
            case A11yRole::radioButton:  return "radioButton";
            case A11yRole::comboBox:     return "comboBox";
            case A11yRole::listItem:     return "listItem";
            case A11yRole::staticText:   return "staticText";
            case A11yRole::image:        return "image";
            case A11yRole::progressBar:  return "progressBar";
        }
        return "?";                                      // a role constant: handled before, or after, such a switch
    }

    // How code names all sixteen: the constants with ==, then the switch over the enumerators.
    const char* roleName(A11yRole r)
    {
        if (r == role::group)        return "group";
        if (r == role::list)         return "list";
        if (r == role::table)        return "table";
        if (r == role::row)          return "row";
        if (r == role::cell)         return "cell";
        if (r == role::editableText) return "editableText";
        return enumeratorName(r);
    }

    A11yItem item(A11yRole r, const char* title, funkgui::Rect b)
    {
        A11yItem it;
        it.role = r;
        it.title = title;
        it.bounds = b;
        return it;
    }

    void expect(T::Probe& P, std::string_view key, const A11yItem& it, std::string_view want)
    {
        const std::string got = funkgui::a11yDumpLine(it);
        if (got != want)
            std::printf("NOTE     %.*s:\nNOTE       got  \"%s\"\nNOTE       want \"%.*s\"\n",
                        static_cast<int>(key.size()), key.data(), got.c_str(), static_cast<int>(want.size()),
                        want.data());
        P.eq(key, got == want, 1);
    }
}

int main(int argc, char** argv)
{
    T::Probe P("fg.a11y.roles", "", argc, argv);

    // ---- the values -------------------------------------------------------------------------------------------------
    {
        static constexpr A11yRole kNew[] = { role::group, role::list, role::table, role::row, role::cell,
                                             role::editableText };
        bool numbered = true;
        for (int i = 0; i < 6; ++i)
            numbered = numbered && static_cast<int>(kNew[i]) == 10 + i;
        P.eq("values.constants_are_10_to_15", numbered, 1);
        static constexpr A11yRole kOld[] = { A11yRole::slider,      A11yRole::toggleButton, A11yRole::button,
                                             A11yRole::radioGroup,  A11yRole::radioButton,  A11yRole::comboBox,
                                             A11yRole::listItem,    A11yRole::staticText,   A11yRole::image,
                                             A11yRole::progressBar };
        bool kept = true;
        for (int i = 0; i < 10; ++i)
            kept = kept && static_cast<int>(kOld[i]) == i;
        P.eq("values.enumerators_keep_0_to_9", kept, 1);
        A11yItem it;
        it.role = role::table;                           // assignable like any role
        P.eq("values.item_role", it.role == role::table && it.role != role::row && static_cast<int>(it.role) == 12, 1);
    }

    // ---- the switch -------------------------------------------------------------------------------------------------
    {
        P.eq("switch.enumerator_named", std::string_view(enumeratorName(A11yRole::comboBox)) == "comboBox", 1);
        P.eq("switch.constant_falls_through", std::string_view(enumeratorName(role::table)) == "?", 1);
        std::string all;
        for (int i = 0; i < 16; ++i)
            all += std::string(i == 0 ? "" : " ") + roleName(static_cast<A11yRole>(i));
        P.eq("switch.sixteen_names",
             all == "slider toggleButton button radioGroup radioButton comboBox listItem staticText image progressBar "
                    "group list table row cell editableText",
             1);
    }

    // ---- the dump line of each new role and field -------------------------------------------------------------------
    //         role title(24)                 bounds(20)
    {
        A11yItem group = item(role::group, "Filter 1, ladder", { 208, 96, 120, 48 });
        group.help = "Voice FX slot 1";
        expect(P, "line.group", group, "10 Filter 1, ladder         208,96,120,48        help=Voice FX slot 1");

        expect(P, "line.list", item(role::list, "Connections", { 8, 300, 320, 96 }),
               "11 Connections              8,300,320,96");

        A11yItem table = item(role::table, "Modulation matrix", { 0, 72, 688, 416 });
        table.rows = 64;
        table.cols = 8;
        expect(P, "line.table", table, "12 Modulation matrix        0,72,688,416         table=64x8");

        A11yItem row = item(role::row, "Route 9", { 0, 232, 688, 20 });
        row.parent = 1;
        row.row = 8;
        expect(P, "line.row", row, "13 Route 9                  0,232,688,20         row=8");

        A11yItem cell = item(role::cell, "Source", { 40, 232, 96, 20 });
        cell.parent = 2;
        cell.col = 0;
        cell.value = "LFO 1";
        expect(P, "line.cell", cell, "14 Source                   40,232,96,20         value=LFO 1 col=0");

        A11yItem text = item(role::editableText, "Preset name", { 300, 12, 200, 20 });
        text.value = "Warm Pad";
        expect(P, "line.editable_text", text, "15 Preset name              300,12,200,20        value=Warm Pad");
        text.value.clear();
        expect(P, "line.editable_text_empty", text, "15 Preset name              300,12,200,20");

        // A control that is a cell keeps its own role and line, and gives its column after everything else.
        A11yItem depth = item(A11yRole::slider, "Depth", { 328, 232, 120, 20 });
        depth.parent = 2;
        depth.col = 3;
        depth.value = "+25 %";
        depth.readOnly = true;
        depth.enabled = false;
        depth.help = "Route is muted";
        expect(P, "line.slider_cell", depth,
               " 0 Depth                    328,232,120,20       value=+25 % [ro] [off] help=Route is muted col=3");
        A11yItem mute = item(A11yRole::toggleButton, "Mute", { 600, 232, 40, 20 });
        mute.checkable = true;
        mute.checked = true;
        mute.col = 7;
        expect(P, "line.toggle_cell", mute, " 1 Mute                     600,232,40,20        checked col=7");
    }

    // ---- each field only when set, in the order table, row, col -----------------------------------------------------
    {
        A11yItem it = item(role::table, "T", { 0, 0, 1, 1 });
        expect(P, "line.table.unset", it, "12 T                        0,0,1,1");
        it.rows = 3;
        expect(P, "line.table.rows_only", it, "12 T                        0,0,1,1              table=3x0");
        it.rows = 0;
        it.cols = 5;
        expect(P, "line.table.cols_only", it, "12 T                        0,0,1,1              table=0x5");
        it.rows = -4;
        it.cols = -1;
        expect(P, "line.table.negative_is_unset", it, "12 T                        0,0,1,1");
        it.rows = 21;
        it.cols = 8;
        it.row = 0;
        it.col = 0;
        expect(P, "line.all_three_in_order", it,
               "12 T                        0,0,1,1              table=21x8 row=0 col=0");
        A11yItem r = item(role::row, "R", { 0, 0, 1, 1 });
        expect(P, "line.row.unset", r, "13 R                        0,0,1,1");
        r.row = 0;
        expect(P, "line.row.zero_is_set", r, "13 R                        0,0,1,1              row=0");
        r.row = 63;
        expect(P, "line.row.last", r, "13 R                        0,0,1,1              row=63");
        A11yItem c = item(role::cell, "C", { 0, 0, 1, 1 });
        c.col = 0;
        expect(P, "line.col.zero_is_set", c, "14 C                        0,0,1,1              col=0");
        c.col = -1;
        expect(P, "line.col.unset", c, "14 C                        0,0,1,1");
        // The fields print whatever the role: a line shows a field set on the wrong kind of item.
        A11yItem odd = item(A11yRole::button, "B", { 0, 0, 1, 1 });
        odd.rows = 2;
        odd.cols = 2;
        expect(P, "line.fields_print_for_any_role", odd, " 2 B                        0,0,1,1              table=2x2");
    }

    // ---- v0.14.0's lines, byte for byte -----------------------------------------------------------------------------
    {
        A11yItem slider = item(A11yRole::slider, "Threshold", { 40, 316, 120, 80 });
        slider.value = "\xE2\x88\x92" "18 decibels";
        slider.help = "Level above which gain reduction starts";
        expect(P, "v0140.slider", slider,
               " 0 Threshold                40,316,120,80        value=\xE2\x88\x92" "18 decibels help=Level above which "
               "gain reduction starts");
        A11yItem locked = item(A11yRole::slider, "Attack", { 312, 316, 120, 80 });
        locked.value = "10 milliseconds, fixed";
        locked.readOnly = true;
        locked.enabled = false;
        locked.help = "Fixed by the Mode";
        expect(P, "v0140.slider_locked", locked,
               " 0 Attack                   312,316,120,80       value=10 milliseconds, fixed [ro] [off] help=Fixed by "
               "the Mode");
        A11yItem toggle = item(A11yRole::toggleButton, "Bypass", { 700, 96, 140, 44 });
        toggle.checkable = true;
        toggle.checked = true;
        expect(P, "v0140.toggle", toggle, " 1 Bypass                   700,96,140,44        checked");
        expect(P, "v0140.button", item(A11yRole::button, "Save", { 820, 20, 40, 16 }),
               " 2 Save                     820,20,40,16");
        A11yItem group = item(A11yRole::radioGroup, "Quality", { 640, 64, 150, 16 });
        group.help = "Std: 2 times IIR oversampling, 4 samples latency";
        expect(P, "v0140.radio_group", group,
               " 3 Quality                  640,64,150,16        help=Std: 2 times IIR oversampling, 4 samples latency");
        A11yItem radio = item(A11yRole::radioButton, "Std", { 690, 64, 48, 16 });
        radio.parent = 7;
        radio.checkable = true;
        expect(P, "v0140.radio_button", radio, " 4 Std                      690,64,48,16         unchecked");
        A11yItem combo = item(A11yRole::comboBox, "Mode", { 40, 20, 200, 28 });
        combo.value = "FET 76";
        expect(P, "v0140.combo", combo, " 5 Mode                     40,20,200,28         value=FET 76");
        expect(P, "v0140.list_item", item(A11yRole::listItem, "Opto 2A", { 40, 120, 110, 13 }),
               " 6 Opto 2A                  40,120,110,13");
        A11yItem na = item(A11yRole::staticText, "Range, not applicable", { 584, 316, 120, 80 });
        na.enabled = false;
        na.help = "This Mode has no range control";
        expect(P, "v0140.static_text", na,
               " 7 Range, not applicable    584,316,120,80       [off] help=This Mode has no range control");
        expect(P, "v0140.image", item(A11yRole::image, "Transfer curve, ratio 4 to 1", { 556, 140, 192, 192 }),
               " 8 Transfer curve, ratio 4 to 1 556,140,192,192");
        A11yItem meter = item(A11yRole::progressBar, "Gain reduction", { 900, 140, 8, 192 });
        meter.value = "\xE2\x88\x92" "4.2 dB";
        expect(P, "v0140.progress", meter, " 9 Gain reduction           900,140,8,192        value=\xE2\x88\x92" "4.2 dB");
        expect(P, "v0140.empty_item", A11yItem{}, " 7                          0,0,0,0");
    }

    // ---- the unset values -------------------------------------------------------------------------------------------
    {
        const A11yItem it;
        P.eq("defaults.table", it.rows == 0 && it.cols == 0, 1);
        P.eq("defaults.row_and_col", it.row == -1 && it.col == -1, 1);
        P.eq("defaults.role_is_static_text", it.role == A11yRole::staticText, 1);
    }

    return P.finish();
}

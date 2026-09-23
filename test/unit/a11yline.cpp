// FUNKGUI_TEST name=fg.a11yline timeout=300 gpu=0
//
// fg.a11yline: a11yDumpLine (a11y/A11yItem.h; 02 §5.6, C's G4): the exact line for each role and state of 02 §8.9's
// table (continuous, stepped, locked, derived and n/a slots, toggles, radio groups, the Mode latch, meters), the
// column padding in codepoints, the shortest-float bounds, one line per item whatever the strings hold, and no
// trailing padding. Spec rows only.

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/test/Harness.h>

#include <cstdio>
#include <limits>
#include <string>
#include <string_view>

namespace T = funkgui::test;
using funkgui::A11yItem;
using funkgui::A11yRole;

namespace
{
    std::string pad(std::string s, size_t w)
    {
        while (s.size() < w)
            s += ' ';
        return s;
    }

    // The expected line from its parts: role column, title padded to 24, bounds padded to 20, then the tail.
    std::string line(int role, const std::string& title, const std::string& bounds, const std::string& tail)
    {
        std::string s = (role < 10 ? " " : "") + std::to_string(role) + " " + pad(title, 24) + " " + pad(bounds, 20);
        if (tail.empty())
        {
            while (!s.empty() && s.back() == ' ')
                s.pop_back();
            return s;
        }
        return s + " " + tail;
    }

    A11yItem item(A11yRole role, const char* title, funkgui::Rect b)
    {
        A11yItem it;
        it.role = role;
        it.title = title;
        it.bounds = b;
        return it;
    }

    void expect(T::Probe& P, std::string_view key, const A11yItem& it, const std::string& want)
    {
        const std::string got = funkgui::a11yDumpLine(it);
        if (got != want)
            std::printf("INFO     %.*s:\nINFO       got  \"%s\"\nINFO       want \"%s\"\n",
                        static_cast<int>(key.size()), key.data(), got.c_str(), want.c_str());
        P.eq(key, got == want, 1);
    }
}

int main(int argc, char** argv)
{
    T::Probe P("fg.a11yline", "", argc, argv);

    // ---- 02 §8.9's roles and states ---------------------------------------------------------------------------------
    {
        A11yItem it = item(A11yRole::slider, "Threshold", { 40, 316, 120, 80 });
        it.value = "\xE2\x88\x92" "18 decibels";
        it.help = "Level above which gain reduction starts";
        expect(P, "slider.continuous", it,
               line(0, "Threshold", "40,316,120,80",
                    "value=\xE2\x88\x92" "18 decibels help=Level above which gain reduction starts"));
    }
    {
        A11yItem it = item(A11yRole::slider, "Ratio", { 176, 316, 120, 80 });
        it.value = "4 to 1";
        it.lo = 0; it.hi = 2; it.step = 1; it.v = 1;
        it.help = "3 steps: 2, 4, 10";
        expect(P, "slider.stepped", it, line(0, "Ratio", "176,316,120,80", "value=4 to 1 help=3 steps: 2, 4, 10"));
    }
    {
        A11yItem it = item(A11yRole::slider, "Attack", { 312, 316, 120, 80 });
        it.value = "10 milliseconds, fixed";
        it.readOnly = true;
        it.enabled = false;
        it.help = "Fixed by the Mode";
        expect(P, "slider.locked", it,
               line(0, "Attack", "312,316,120,80", "value=10 milliseconds, fixed [ro] [off] help=Fixed by the Mode"));
    }
    {
        A11yItem it = item(A11yRole::slider, "Knee", { 448, 316, 120, 80 });
        it.value = "6 decibels, follows ratio";
        it.readOnly = true;
        it.help = "Follows ratio";
        expect(P, "slider.derived", it,
               line(0, "Knee", "448,316,120,80", "value=6 decibels, follows ratio [ro] help=Follows ratio"));
    }
    {
        A11yItem it = item(A11yRole::staticText, "Range, not applicable", { 584, 316, 120, 80 });
        it.enabled = false;
        it.help = "This Mode has no range control";
        expect(P, "static_text.na", it,
               line(7, "Range, not applicable", "584,316,120,80", "[off] help=This Mode has no range control"));
    }
    {
        A11yItem on = item(A11yRole::toggleButton, "Bypass", { 700, 96, 140, 44 });
        on.checkable = true;
        on.checked = true;
        on.value = "ignored for a checkable item";
        expect(P, "toggle.checked", on, line(1, "Bypass", "700,96,140,44", "checked"));
        on.checked = false;
        on.enabled = false;
        on.help = "Unavailable while listening";
        expect(P, "toggle.unchecked_disabled", on,
               line(1, "Bypass", "700,96,140,44", "unchecked [off] help=Unavailable while listening"));
    }
    {
        A11yItem group = item(A11yRole::radioGroup, "Quality", { 640, 64, 150, 16 });
        group.help = "Std: 2 times IIR oversampling, 4 samples latency";
        expect(P, "radio.group", group,
               line(3, "Quality", "640,64,150,16", "help=Std: 2 times IIR oversampling, 4 samples latency"));
        A11yItem cell = item(A11yRole::radioButton, "Std", { 690, 64, 48, 16 });
        cell.parent = 7;
        cell.checkable = true;
        cell.checked = true;
        expect(P, "radio.button", cell, line(4, "Std", "690,64,48,16", "checked"));
    }
    {
        A11yItem it = item(A11yRole::comboBox, "Mode", { 40, 20, 200, 28 });
        it.value = "FET 76";
        expect(P, "combo.mode_latch", it, line(5, "Mode", "40,20,200,28", "value=FET 76"));
        A11yItem row = item(A11yRole::listItem, "Opto 2A", { 40, 120, 110, 13 });
        expect(P, "list_item.no_tail", row, line(6, "Opto 2A", "40,120,110,13", ""));
        A11yItem plot = item(A11yRole::image, "Transfer curve, ratio 4 to 1", { 556, 140, 192, 192 });
        expect(P, "image.long_title", plot, line(8, "Transfer curve, ratio 4 to 1", "556,140,192,192", ""));
        A11yItem meter = item(A11yRole::progressBar, "Gain reduction", { 900, 140, 8, 192 });
        meter.value = "\xE2\x88\x92" "4.2 dB";
        expect(P, "progress.meter", meter, line(9, "Gain reduction", "900,140,8,192", "value=\xE2\x88\x92" "4.2 dB"));
        A11yItem button = item(A11yRole::button, "Save", { 820, 20, 40, 16 });
        expect(P, "button.plain", button, line(2, "Save", "820,20,40,16", ""));
    }

    // ---- columns, numbers and one line per item ---------------------------------------------------------------------
    {
        // A title with multi-byte codepoints pads by codepoints: "ΔGR" is 3 columns, 4 bytes.
        const std::string got = funkgui::a11yDumpLine(item(A11yRole::staticText, "\xCE\x94GR", { 1, 2, 3, 4 }));
        const std::string want = std::string(" 7 \xCE\x94GR") + std::string(21, ' ') + " 1,2,3,4";
        P.eq("columns.codepoints", got == want, 1);
        if (got != want)
            std::printf("INFO     columns.codepoints: got \"%s\"\n", got.c_str());
    }
    expect(P, "columns.empty_item", A11yItem {}, line(7, "", "0,0,0,0", ""));
    expect(P, "columns.long_bounds_push_right",
           [] { A11yItem it = item(A11yRole::slider, "X", { 1234.5f, 5678.25f, 9999.125f, 0.0625f });
                it.value = "v"; return it; }(),
           " 0 " + pad("X", 24) + " 1234.5,5678.25,9999.125,0.0625 value=v");
    expect(P, "numbers.shortest_float", item(A11yRole::image, "N", { 0.1f, -2.5f, 38.5f, 1234.25f }),
           line(8, "N", "0.1,-2.5,38.5,1234.25", ""));
    expect(P, "numbers.minus_zero_and_large", item(A11yRole::image, "N", { -0.0f, 16777216.0f, 1e-3f, 3.0f }),
           line(8, "N", "0,16777216,0.001,3", ""));
    expect(P, "numbers.non_finite",
           item(A11yRole::image, "N", { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                                        -std::numeric_limits<float>::infinity(), 1 }),
           line(8, "N", "nan,inf,-inf,1", ""));
    {
        A11yItem it = item(A11yRole::slider, "Two\nlines", { 0, 0, 1, 1 });
        it.value = "a\tb";
        it.help = "first\r\nsecond";
        const std::string got = funkgui::a11yDumpLine(it);
        P.eq("one_line.no_cr_lf_tab", got.find_first_of("\r\n\t") == std::string::npos, 1);
        expect(P, "one_line.text", it, line(0, "Two lines", "0,0,1,1", "value=a b help=first  second"));
    }
    {
        A11yItem it = item(A11yRole::slider, "Trailing", { 0, 0, 1, 1 });
        it.value = "4 ";
        expect(P, "tail.value_kept_verbatim", it, line(0, "Trailing", "0,0,1,1", "value=4 "));
        it.value.clear();
        const std::string got = funkgui::a11yDumpLine(it);
        P.eq("tail.no_trailing_padding", !got.empty() && got.back() != ' ', 1);
        P.eq("tail.visible_not_printed", [] { A11yItem a = item(A11yRole::button, "Hidden", { 0, 0, 1, 1 });
                                              a.visible = false;
                                              return funkgui::a11yDumpLine(a); }() ==
                                             line(2, "Hidden", "0,0,1,1", ""),
             1);
    }

    return P.finish();
}

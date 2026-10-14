// FUNKGUI_TEST name=fg.lineedit.paste timeout=120 gpu=0
//
// fg.lineedit.paste (FSynth G1): text::LineEdit::paste (text/LineEdit.h). Spec rows only; no JUCE, so it runs in
// every configuration.
//
// - insert.*    the text goes in at the caret (start, middle, end; a caret out of range is clamped), and the caret
//               ends after it.
// - selected.*  while the pre-filled text is all selected the paste replaces it, and the selection is dropped.
// - cut.*       the buffer never exceeds maxLength characters: a paste is cut to the room left; with no room, or a
//               maxLength of 0, nothing changes and it returns false.
// - clean.*     a character outside ASCII 32..126 becomes '?' (one per codepoint, as text::printable writes it); a
//               tab and a line break become a space each, CR LF being one break.
// - nothing.*   a paste of nothing returns false and changes nothing, the selection included.
// - keys.*      key() still leaves the paste chord to the product: Cmd-V and Ctrl-V return false and insert nothing.

#include <funkgui/panel/Input.h>
#include <funkgui/test/Harness.h>
#include <funkgui/text/LineEdit.h>

#include <cstdio>
#include <string>
#include <string_view>

namespace T = funkgui::test;
using funkgui::text::LineEdit;

namespace
{
    // A field holding `text` with the caret at `caret`, nothing selected.
    LineEdit field(std::string_view text, int caret)
    {
        LineEdit e;
        e.buffer = std::string(text);
        e.caret = caret;
        e.allSelected = false;
        return e;
    }

    // One row: the result, the buffer, the caret and the selection after a paste.
    void row(T::Probe& P, const std::string& key, LineEdit e, std::string_view paste, int maxLength, bool wantInserted,
             std::string_view wantBuffer, int wantCaret, bool wantSelected = false)
    {
        const bool inserted = e.paste(paste, maxLength);
        const bool ok = inserted == wantInserted && e.buffer == wantBuffer && e.caret == wantCaret
                     && e.allSelected == wantSelected;
        if (!ok)
            std::printf("NOTE     %s: inserted %d, buffer '%s', caret %d, selected %d\n", key.c_str(), inserted ? 1 : 0,
                        e.buffer.c_str(), e.caret, e.allSelected ? 1 : 0);
        P.eq(key, ok, 1);
    }
}

int main(int argc, char** argv)
{
    T::Probe P("fg.lineedit.paste", "", argc, argv);

    // ---- insertion at the caret -------------------------------------------------------------------------------------
    row(P, "insert.into_empty", field("", 0), "Pad", 32, true, "Pad", 3);
    row(P, "insert.at_end", field("Warm", 4), " Pad", 32, true, "Warm Pad", 8);
    row(P, "insert.at_start", field("Pad", 0), "Warm ", 32, true, "Warm Pad", 5);
    row(P, "insert.in_the_middle", field("WarmPad", 4), " Soft ", 32, true, "Warm Soft Pad", 10);
    row(P, "insert.caret_beyond_end_is_clamped", field("Warm", 99), "!", 32, true, "Warm!", 5);
    row(P, "insert.caret_below_zero_is_clamped", field("Warm", -3), "!", 32, true, "!Warm", 1);
    row(P, "insert.one_character", field("ac", 1), "b", 3, true, "abc", 2);

    // ---- the whole text selected: replaced --------------------------------------------------------------------------
    {
        LineEdit e;
        e.set("Init Preset", 32);                        // selected, caret at the end (set()'s rule)
        P.eq("selected.set_selects", e.allSelected && e.caret == 11, 1);
        row(P, "selected.replaced", e, "Lead", 32, true, "Lead", 4);
        row(P, "selected.replaced_and_cut", e, "A very long preset name", 8, true, "A very l", 8);
        // The replacement counts against maxLength alone, not against what was there.
        row(P, "selected.replaced_at_full_length", e, "123456789012", 11, true, "12345678901", 11);
    }

    // ---- the length cut ---------------------------------------------------------------------------------------------
    row(P, "cut.to_the_room_left", field("Warm", 4), " Pad of the north", 8, true, "Warm Pad", 8);
    row(P, "cut.in_the_middle", field("abef", 2), "cdXYZ", 6, true, "abcdef", 4);
    row(P, "cut.exactly_fits", field("ab", 2), "cd", 4, true, "abcd", 4);
    row(P, "cut.no_room", field("full", 2), "x", 4, false, "full", 2);
    row(P, "cut.no_room_keeps_caret", field("full", 99), "x", 4, false, "full", 99);
    row(P, "cut.max_length_zero", field("", 0), "x", 0, false, "", 0);
    row(P, "cut.max_length_negative", field("ab", 1), "x", -1, false, "ab", 1);
    {
        LineEdit e;
        e.set("Keep", 32);
        row(P, "cut.max_length_zero_keeps_selection", e, "x", 0, false, "Keep", 4, true);
    }

    // ---- made printable ---------------------------------------------------------------------------------------------
    row(P, "clean.non_ascii_is_question_mark", field("", 0), "na\xC3\xAFve \xE2\x80\x94 caf\xC3\xA9", 32, true,
        "na?ve ? caf?", 12);
    row(P, "clean.control_character", field("", 0), std::string_view("a\x01" "b\x7f" "c", 5), 32, true, "a?b?c", 5);
    row(P, "clean.malformed_utf8", field("", 0), "a\xFF" "b", 32, true, "a?b", 3);
    row(P, "clean.tab_is_space", field("", 0), "12\t34", 32, true, "12 34", 5);
    row(P, "clean.newline_is_space", field("", 0), "line 1\nline 2", 32, true, "line 1 line 2", 13);
    row(P, "clean.carriage_return_is_space", field("", 0), "a\rb", 32, true, "a b", 3);
    row(P, "clean.crlf_is_one_space", field("", 0), "a\r\nb\r\n", 32, true, "a b ", 4);
    row(P, "clean.lf_cr_is_two", field("", 0), "a\n\rb", 32, true, "a  b", 4);
    row(P, "clean.only_a_line_break", field("ab", 1), "\n", 32, true, "a b", 2);
    row(P, "clean.cut_counts_cleaned_characters", field("", 0), "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9", 3, true, "???", 3);

    // ---- a paste of nothing -----------------------------------------------------------------------------------------
    row(P, "nothing.empty_text", field("Warm", 2), "", 32, false, "Warm", 2);
    row(P, "nothing.default_view", field("Warm", 2), std::string_view{}, 32, false, "Warm", 2);
    {
        LineEdit e;
        e.set("Keep", 32);
        row(P, "nothing.keeps_selection", e, "", 32, false, "Keep", 4, true);
    }

    // ---- key() leaves the chord to the product ----------------------------------------------------------------------
    {
        LineEdit e = field("Warm", 4);
        funkgui::KeyEvent cmdV;
        cmdV.key = funkgui::Key::character;
        cmdV.ch = U'v';
        cmdV.mods.cmd = true;
        P.eq("keys.cmd_v_is_the_products", !e.key(cmdV, 32) && e.buffer == "Warm", 1);
        funkgui::KeyEvent ctrlV = cmdV;                  // Linux: EditorHost sets cmd and ctrl together for Ctrl-V
        ctrlV.mods.ctrl = true;
        P.eq("keys.ctrl_v_is_the_products", !e.key(ctrlV, 32) && e.buffer == "Warm", 1);
        // After a paste the field edits as after typing: Backspace deletes the last pasted character.
        e.paste(" Pad", 32);
        funkgui::KeyEvent back;
        back.key = funkgui::Key::backspace;
        P.eq("keys.backspace_after_paste", e.key(back, 32) && e.buffer == "Warm Pa" && e.caret == 7, 1);
    }

    return P.finish();
}

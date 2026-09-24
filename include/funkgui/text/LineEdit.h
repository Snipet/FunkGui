#pragma once

// One-line text entry (02 §5.8; HR PresetPanel.cpp:386-421 editKey and :945-975 the selected pre-fill): a preset name,
// a search field. State only: the product draws the buffer, the selection and the caret, and decides what Return and
// Escape mean (commit, cancel), which key() leaves to it. And text::printable, which makes typed or imported text
// something the atlas can draw.
//
// G6 (HR's editKey and keyPressed, made a value type; fg.lineedit pins every rule):
// - buffer holds printable ASCII (32..126, what the atlas draws for typed text); set() and key() keep it so. caret is a
//   byte offset, clamped into the buffer by every key() call.
// - allSelected: a pre-filled buffer is selected, as a native field would have it (set() selects a non-empty text).
//   While it is: a character, Backspace or Delete replaces the whole text (the character is typed, the deletes leave it
//   empty; HR: Save As on "Keep" then typing "Temp" saved "KeepTemp"); ← drops the selection and puts the caret at 0;
//   → drops it and leaves the caret at the end; any other key drops it and is then handled as below.
// - Keys (HR editKey): Backspace deletes before the caret (Alt: back to the start of the word, over trailing spaces;
//   Cmd: everything before the caret); Delete deletes after it; ← / → move by one (Cmd: to the start / end);
//   Home / End; a character 32..126 (Key::character, or Key::space as ' ') without Cmd or Ctrl is inserted when the
//   text is shorter than maxLength characters. Those return true (Backspace at 0 and Delete at the end included, as
//   HR: an edit owns those keys); everything else returns false, including Return and Escape, which never touch the
//   selection, and a character that does not fit.

#include <funkgui/panel/Input.h>

#include <string>
#include <string_view>

namespace juce
{
    class String;
}

namespace funkgui::text
{
    // ASCII 32..126 kept, every other codepoint (and every malformed UTF-8 sequence) written as '?', so a name imported
    // from a machine with a different keyboard shows where a character was instead of silently losing it
    // (PresetPanel.cpp:63-75). For typed names only: labels and readouts are the product's own text.
    std::string  printable(std::string_view utf8);
    std::string  printable(const char* utf8);           // nullptr: ""  (these two keep a literal or a std::string
    std::string  printable(const std::string& utf8);    // from being ambiguous with the juce::String overload)
    juce::String printable(const juce::String&);

    class LineEdit
    {
    public:
        bool key(const KeyEvent&, int maxLength);

        // G6 addition (HR beginEdit): buffer = printable(text) cut to maxLength characters, caret at the end, and the
        // whole text selected when it is not empty.
        void set(std::string_view text, int maxLength);

        std::string buffer;
        int  caret = 0;
        bool allSelected = false;

    private:
        bool edit(const KeyEvent&, int maxLength);       // HR editKey, without the selection
    };
}

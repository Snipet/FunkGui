// FUNKGUI_TEST name=fg.desktop.input timeout=120 gpu=0 links=harness
//
// fg.desktop.input (v0.28.0): desktop/DesktopInput.h, the pure part of DesktopHost's input conversion, one group of
// spec rows per rule of that header. Header-only and free of SDL, so the same rows run in every configuration; what a
// window adds (SDL's events reaching these functions) is fg.desktop.host's.
//   mods.*    SDL's modifier bits as Mods: on Apple's platforms cmd is the GUI key; elsewhere Ctrl sets cmd AND ctrl
//             and the GUI key is nothing (JUCE); either key of a pair counts
//   button.*  left, middle and right are a press and say which; the back and forward buttons are not
//   popup.*   the right button; Ctrl on Apple's platforms only
//   key.*     EditorHost's key table entry by entry (the keypad's Enter is Enter); the space bar's character; a chord
//             of the command key or Ctrl with a printable ASCII key as its lower-case character; AltGr (Ctrl and Alt
//             off Apple's platforms) is no chord; a plain printable key, a function key and a modifier key refused
//   text.*    typed text code point by code point, past ASCII; the space and control characters are no character;
//             damaged UTF-8 skipped a byte at a time
//   wheel.*   SDL's sign and units as JUCE's: a notch = 50 / 256 up, not smooth; a fraction of one smooth; on Apple's
//             platforms 5 / 256 a unit, smooth; x to the right is a negative dx; flipped is reversed; never inertial;
//             no wheel on an axis is +0
// Spec rows only.

#include <funkgui/desktop/DesktopInput.h>
#include <funkgui/panel/Input.h>
#include <funkgui/test/Harness.h>

#include <bit>
#include <cstdint>
#include <string>
#include <string_view>

namespace T = funkgui::test;
namespace D = funkgui::desktop;
using funkgui::Button;
using funkgui::Key;
using funkgui::KeyEvent;
using funkgui::Mods;
using funkgui::WheelEvent;

namespace
{
    bool sameBits(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }

    // shift | cmd << 1 | alt << 2 | ctrl << 3
    int bits(Mods m) { return (m.shift ? 1 : 0) | (m.cmd ? 2 : 0) | (m.alt ? 4 : 0) | (m.ctrl ? 8 : 0); }

    constexpr int kShift = 1, kCmd = 2, kAlt = 4, kCtrl = 8;

    // SDL's single modifier bits (SDL_keycode.h), which the masks of DesktopInput.h are made of.
    constexpr uint16_t kLShift = 0x0001u, kRShift = 0x0002u, kLCtrl = 0x0040u, kRCtrl = 0x0080u, kLAlt = 0x0100u,
                       kRAlt = 0x0200u, kLGui = 0x0400u, kRGui = 0x0800u, kNum = 0x1000u, kCaps = 0x2000u;

    // The rules are usable in constant expressions (a host may table them).
    static_assert(D::modsFromSdl(kLCtrl, false).cmd && !D::modsFromSdl(kLCtrl, true).cmd);
    static_assert(D::takesButton(2) && !D::takesButton(4));
    static_assert(D::popupFromSdl(3, 0, false) && !D::popupFromSdl(1, kLCtrl, false));
    static_assert(D::wheelFromSdl(0.0f, 1.0f, false, false).dy == 0.1953125f);

    int keyOf(uint32_t code, uint16_t mods, bool apple, KeyEvent& ev)
    {
        return D::keyFromSdl(code, mods, apple, ev) ? 1 : 0;
    }
}

int main(int argc, char** argv)
{
    T::Probe P("fg.desktop.input", "", argc, argv);

    // ---- mods -------------------------------------------------------------------------------------------------------
    P.eq("mods.none", bits(D::modsFromSdl(0, false)), 0);
    P.eq("mods.lshift", bits(D::modsFromSdl(kLShift, false)), kShift);
    P.eq("mods.rshift", bits(D::modsFromSdl(kRShift, true)), kShift);
    P.eq("mods.ctrl_elsewhere", bits(D::modsFromSdl(kLCtrl, false)), kCmd | kCtrl);
    P.eq("mods.rctrl_elsewhere", bits(D::modsFromSdl(kRCtrl, false)), kCmd | kCtrl);
    P.eq("mods.ctrl_apple", bits(D::modsFromSdl(kLCtrl, true)), kCtrl);
    P.eq("mods.gui_apple", bits(D::modsFromSdl(kLGui, true)), kCmd);
    P.eq("mods.rgui_apple", bits(D::modsFromSdl(kRGui, true)), kCmd);
    P.eq("mods.gui_elsewhere", bits(D::modsFromSdl(kLGui, false)), 0);
    P.eq("mods.alt", bits(D::modsFromSdl(kRAlt, false)), kAlt);
    P.eq("mods.all_apple", bits(D::modsFromSdl(kLShift | kLCtrl | kLAlt | kLGui, true)), kShift | kCmd | kAlt | kCtrl);
    P.eq("mods.locks_are_nothing", bits(D::modsFromSdl(kNum | kCaps, false)), 0);

    // ---- buttons ----------------------------------------------------------------------------------------------------
    P.eq("button.takes_left", D::takesButton(1), 1);
    P.eq("button.takes_middle", D::takesButton(2), 1);
    P.eq("button.takes_right", D::takesButton(3), 1);
    P.eq("button.refuses_back", D::takesButton(4), 0);
    P.eq("button.refuses_forward", D::takesButton(5), 0);
    P.eq("button.refuses_none", D::takesButton(0), 0);
    P.eq("button.left", D::buttonFromSdl(1) == Button::left, 1);
    P.eq("button.middle", D::buttonFromSdl(2) == Button::middle, 1);
    P.eq("button.right", D::buttonFromSdl(3) == Button::right, 1);
    P.eq("button.none_is_left", D::buttonFromSdl(0) == Button::left, 1);

    // ---- popup ------------------------------------------------------------------------------------------------------
    P.eq("popup.right", D::popupFromSdl(3, 0, false), 1);
    P.eq("popup.right_apple", D::popupFromSdl(3, 0, true), 1);
    P.eq("popup.left", D::popupFromSdl(1, 0, true), 0);
    P.eq("popup.ctrl_left_apple", D::popupFromSdl(1, kLCtrl, true), 1);
    P.eq("popup.ctrl_left_elsewhere", D::popupFromSdl(1, kLCtrl, false), 0);
    P.eq("popup.middle", D::popupFromSdl(2, 0, false), 0);

    // ---- keys -------------------------------------------------------------------------------------------------------
    {
        struct Row
        {
            const char* name;
            uint32_t code;
            Key key;
        };
        const Row rows[] = {
            { "tab", D::kKeyTab, Key::tab },           { "up", D::kKeyUp, Key::up },
            { "down", D::kKeyDown, Key::down },        { "left", D::kKeyLeft, Key::left },
            { "right", D::kKeyRight, Key::right },     { "pageup", D::kKeyPageUp, Key::pageUp },
            { "pagedown", D::kKeyPageDown, Key::pageDown }, { "home", D::kKeyHome, Key::home },
            { "end", D::kKeyEnd, Key::end },           { "escape", D::kKeyEscape, Key::escape },
            { "enter", D::kKeyReturn, Key::enter },    { "keypad_enter", D::kKeyKeypadEnter, Key::enter },
            { "backspace", D::kKeyBackspace, Key::backspace }, { "delete", D::kKeyDelete, Key::del },
        };
        for (const Row& r : rows)
        {
            KeyEvent ev;
            const int ok = keyOf(r.code, kLShift, false, ev);
            P.eq(std::string("key.named.") + r.name, ok == 1 && ev.key == r.key && ev.ch == 0 && bits(ev.mods) == kShift,
                 1);
        }
        KeyEvent ev;
        P.eq("key.space", keyOf(D::kKeySpace, 0, false, ev) == 1 && ev.key == Key::space && ev.ch == U' ', 1);
        P.eq("key.plain_letter_refused", keyOf('a', 0, false, ev), 0);
        P.eq("key.shift_letter_refused", keyOf('a', kLShift, false, ev), 0);
        P.eq("key.ctrl_letter", keyOf('z', kLCtrl, false, ev) == 1 && ev.key == Key::character && ev.ch == U'z'
                                    && bits(ev.mods) == (kCmd | kCtrl), 1);
        P.eq("key.cmd_letter_apple", keyOf('s', kLGui, true, ev) == 1 && ev.key == Key::character && ev.ch == U's'
                                         && bits(ev.mods) == kCmd, 1);
        P.eq("key.ctrl_letter_apple", keyOf('a', kLCtrl, true, ev) == 1 && ev.ch == U'a' && bits(ev.mods) == kCtrl, 1);
        P.eq("key.gui_letter_elsewhere_refused", keyOf('s', kLGui, false, ev), 0);
        P.eq("key.chord_upper_is_lowered", keyOf('Z', kLCtrl | kLShift, false, ev) == 1 && ev.ch == U'z', 1);
        P.eq("key.chord_digit", keyOf('1', kLCtrl, false, ev) == 1 && ev.ch == U'1', 1);
        P.eq("key.altgr_is_no_chord", keyOf('q', kLCtrl | kRAlt, false, ev), 0);
        P.eq("key.ctrl_alt_apple_is_a_chord", keyOf('q', kLCtrl | kLAlt, true, ev) == 1 && ev.ch == U'q', 1);
        P.eq("key.f1_refused", keyOf(0x4000003au, 0, false, ev), 0);                 // SDLK_F1
        P.eq("key.f1_chord_refused", keyOf(0x4000003au, kLCtrl, false, ev), 0);
        P.eq("key.lshift_refused", keyOf(0x400000e1u, kLShift, false, ev), 0);       // SDLK_LSHIFT
        P.eq("key.past_ascii_chord_refused", keyOf(0xe9u, kLCtrl, false, ev), 0);    // SDLK of e acute
    }

    // ---- text -------------------------------------------------------------------------------------------------------
    {
        const std::string_view text = "a \xC3\xA9\xE2\x88\x92\xF0\x9F\x8E\x9B!";   // a, space, e acute, minus, U+1F39B, !
        std::size_t at = 0;
        const char32_t want[] = { U'a', U' ', 0xe9u, 0x2212u, 0x1f39bu, U'!' };
        bool same = true;
        for (const char32_t w : want)
            same = same && D::nextCodePoint(text, at) == w;
        P.eq("text.code_points", same, 1);
        P.eq("text.end", D::nextCodePoint(text, at), 0);
        P.eq("text.end_again", D::nextCodePoint(text, at), 0);
        std::size_t bad = 0;
        const std::string_view damaged = "\xFFx\xC3";                               // a stray byte, x, a cut sequence
        P.eq("text.damaged_skipped", D::nextCodePoint(damaged, bad), U'x');
        P.eq("text.damaged_end", D::nextCodePoint(damaged, bad), 0);
        P.eq("text.letter_is_character", D::textIsCharacter(U'a'), 1);
        P.eq("text.past_ascii_is_character", D::textIsCharacter(0x2212u), 1);
        P.eq("text.space_is_the_keys", D::textIsCharacter(U' '), 0);
        P.eq("text.control_is_none", D::textIsCharacter(0x1bu), 0);
        P.eq("text.del_is_none", D::textIsCharacter(0x7fu), 0);
    }

    // ---- the wheel --------------------------------------------------------------------------------------------------
    {
        WheelEvent w = D::wheelFromSdl(0.0f, 1.0f, false, false);
        P.eq("wheel.notch_up", sameBits(w.dy, 50.0f / 256.0f) && sameBits(w.dx, 0.0f) && !w.smooth && !w.reversed
                                   && !w.inertial, 1);
        w = D::wheelFromSdl(0.0f, -3.0f, false, false);
        P.eq("wheel.three_notches_down", sameBits(w.dy, -150.0f / 256.0f) && !w.smooth, 1);
        w = D::wheelFromSdl(0.0f, 0.25f, false, false);
        P.eq("wheel.fraction_is_smooth", sameBits(w.dy, 12.5f / 256.0f) && w.smooth, 1);
        w = D::wheelFromSdl(1.0f, 0.0f, false, false);
        P.eq("wheel.right_is_negative_dx", sameBits(w.dx, -50.0f / 256.0f) && sameBits(w.dy, 0.0f), 1);
        w = D::wheelFromSdl(0.0f, 1.0f, true, false);
        P.eq("wheel.flipped_is_reversed", w.reversed && sameBits(w.dy, 50.0f / 256.0f), 1);
        w = D::wheelFromSdl(0.0f, 10.0f, false, true);
        P.eq("wheel.apple_unit", sameBits(w.dy, 50.0f / 256.0f) && w.smooth, 1);
        w = D::wheelFromSdl(-2.0f, 0.0f, false, true);
        P.eq("wheel.apple_left", sameBits(w.dx, 10.0f / 256.0f) && w.smooth, 1);
        w = D::wheelFromSdl(0.0f, 0.0f, false, false);
        P.eq("wheel.none_is_plus_zero", sameBits(w.dx, 0.0f) && sameBits(w.dy, 0.0f), 1);
    }
    return P.finish();
}

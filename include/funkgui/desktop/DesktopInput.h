#pragma once

// SDL3's input as a Panel's (v0.28.0): the pure part of DesktopHost's input conversion, the counterpart of
// web/WebInput.h for a browser's events and of what EditorHost does with JUCE's (src/gpu/EditorHost.cpp "Input").
// Plain C++, header-only, with no SDL header: DesktopHost (src/desktop/DesktopHost.cpp) reads the SDL event's fields
// and hands them here as numbers, so every rule below is tested without a window (fg.desktop.input) and a display is
// needed only to prove the wiring. The numbers of SDL's that appear here (modifier bits, button numbers, key codes) are
// SDL 3's ABI, which SDL keeps for the whole major version; DesktopHost.cpp holds each to SDL's own header with a
// static_assert.
//
// The rules are JUCE's, as WebInput.h's are, so that a Panel sees in a desktop window what it sees in a plug-in's:
// - Modifiers. On Apple's platforms cmd is the GUI key (Command) and ctrl is Control. Elsewhere Ctrl is the command
//   key and sets cmd AND ctrl, and the GUI key (the Windows key) is nothing (JUCE's commandModifier is ctrlModifier
//   there). HostServices::commandKeyIsMeta() says which: a host on macOS answers true.
// - The popup rule (JUCE's isPopupMenu): the right button, or on Apple's platforms Ctrl with any button.
// - Buttons: left, middle and right are a press (PointerEvent::button says which, as EditorHost since F1); the back
//   and forward buttons are not.
// - Positions: window coordinates (SDL's: points on macOS, pixels on Windows and X11) to the Panel's logical px
//   through the window's size, so the pixel density, the display's content scale and the UI zoom are divided out
//   together. Fractional positions stay fractional. (web::toLogical is the arithmetic.)
// - Keys. A key press is a named key the Panel knows (EditorHost's table; the keypad's Enter is Enter), or the space
//   bar (Key::space with its character), or, while the command key or Ctrl is held, a printable ASCII key as
//   Key::character with the key's own (lower-case) character: a chord makes no text, and that is what JUCE sends and
//   what HeadlessHost::keys gives. Ctrl with Alt off Apple's platforms is AltGr, which types: it is no chord. Every
//   other press is refused here, and its text, if it has any, arrives as text.
// - Text (SDL_EVENT_TEXT_INPUT): each code point at or above U+0020 is one Key::character, except the space, which the
//   key press already delivered. No IME composition is shown (README, "Desktop": what is not there).
// - The wheel in JUCE's units and sign (positive dy: wheel up, away from the user; positive dx: left), as
//   web::wheelFromDom gives them. SDL reports notches off macOS: a whole number of them is a notched wheel, 50 / 256
//   a notch (web::kWheelPerNotch) and not smooth; a fraction is a precision touchpad or a free-spinning wheel, the same
//   size and smooth. On macOS SDL reports a tenth of the points scrolled for a trackpad or a Magic Mouse and lines for
//   a notched wheel, and does not say which: both are taken as a trackpad's, 5 / 256 a unit (JUCE's 0.5 / 256 a
//   point), smooth. `reversed` is SDL's "flipped" direction (natural scrolling); SDL does not report momentum, so
//   `inertial` is always false.
// - A pinch (SDL_EVENT_PINCH_UPDATE): its scale since the last one, as MagnifyEvent::scale.

#include <funkgui/panel/Input.h>
#include <funkgui/web/WebInput.h>

#include <cstdint>
#include <string_view>

namespace funkgui::desktop
{
    // ---- modifiers --------------------------------------------------------------------------------------------------

    // SDL_Keymod's bits (SDL_keycode.h).
    inline constexpr uint16_t kModShift = 0x0003u;       // SDL_KMOD_SHIFT: either Shift key
    inline constexpr uint16_t kModCtrl = 0x00c0u;        // SDL_KMOD_CTRL
    inline constexpr uint16_t kModAlt = 0x0300u;         // SDL_KMOD_ALT
    inline constexpr uint16_t kModGui = 0x0c00u;         // SDL_KMOD_GUI: Command, the Windows key

    // SDL's modifier state as the Panel's Mods (the header comment has the rule).
    constexpr Mods modsFromSdl(uint16_t sdlMods, bool apple) noexcept
    {
        return web::modsFromDom((sdlMods & kModShift) != 0, (sdlMods & kModCtrl) != 0, (sdlMods & kModAlt) != 0,
                                (sdlMods & kModGui) != 0, apple);
    }

    // ---- buttons ----------------------------------------------------------------------------------------------------

    inline constexpr int kButtonLeft = 1, kButtonMiddle = 2, kButtonRight = 3;   // SDL_BUTTON_LEFT, _MIDDLE, _RIGHT

    // Whether a press of this SDL button is a press for the Panel.
    constexpr bool takesButton(int sdlButton) noexcept
    {
        return sdlButton == kButtonLeft || sdlButton == kButtonMiddle || sdlButton == kButtonRight;
    }

    constexpr Button buttonFromSdl(int sdlButton) noexcept
    {
        return sdlButton == kButtonRight ? Button::right : sdlButton == kButtonMiddle ? Button::middle : Button::left;
    }

    // PointerEvent::popup: the right button, or Ctrl on Apple's platforms.
    constexpr bool popupFromSdl(int sdlButton, uint16_t sdlMods, bool apple) noexcept
    {
        return web::popupFromDom(sdlButton == kButtonRight, (sdlMods & kModCtrl) != 0, apple);
    }

    // ---- keys -------------------------------------------------------------------------------------------------------

    // SDL_Keycode values (SDL_keycode.h): the ASCII ones are their characters, the others a scancode with bit 30.
    inline constexpr uint32_t kKeyBackspace = 0x08u, kKeyTab = 0x09u, kKeyReturn = 0x0du, kKeyEscape = 0x1bu,
                              kKeySpace = 0x20u, kKeyDelete = 0x7fu;
    inline constexpr uint32_t kKeyRight = 0x4000004fu, kKeyLeft = 0x40000050u, kKeyDown = 0x40000051u,
                              kKeyUp = 0x40000052u, kKeyHome = 0x4000004au, kKeyPageUp = 0x4000004bu,
                              kKeyEnd = 0x4000004du, kKeyPageDown = 0x4000004eu, kKeyKeypadEnter = 0x40000058u;

    // A key press (SDL_EVENT_KEY_DOWN's key code and modifier state) as the Panel's KeyEvent; false, and `out` not a
    // key to deliver, for a press that is no key of the Panel's by itself (the header comment has the rule).
    constexpr bool keyFromSdl(uint32_t keycode, uint16_t sdlMods, bool apple, KeyEvent& out) noexcept
    {
        struct Named
        {
            uint32_t code;
            Key key;
        };
        constexpr Named named[] = {
            { kKeyTab, Key::tab },       { kKeyUp, Key::up },           { kKeyDown, Key::down },
            { kKeyLeft, Key::left },     { kKeyRight, Key::right },     { kKeyPageUp, Key::pageUp },
            { kKeyPageDown, Key::pageDown }, { kKeyHome, Key::home },   { kKeyEnd, Key::end },
            { kKeyEscape, Key::escape }, { kKeyReturn, Key::enter },    { kKeyKeypadEnter, Key::enter },
            { kKeyBackspace, Key::backspace }, { kKeyDelete, Key::del },
        };
        out = KeyEvent{};
        out.mods = modsFromSdl(sdlMods, apple);
        for (const Named& n : named)
            if (keycode == n.code)
            {
                out.key = n.key;
                return true;
            }
        if (keycode == kKeySpace)
        {
            out.key = Key::space;
            out.ch = U' ';
            return true;
        }
        const bool ctrl = (sdlMods & kModCtrl) != 0, alt = (sdlMods & kModAlt) != 0;
        const bool altGr = !apple && ctrl && alt;
        const bool chord = (out.mods.cmd || ctrl) && !altGr;
        if (!chord || keycode < 0x21u || keycode > 0x7eu)
            return false;                            // text, if any, arrives as text
        out.key = Key::character;
        out.ch = keycode >= U'A' && keycode <= U'Z' ? keycode - U'A' + U'a' : static_cast<char32_t>(keycode);
        return true;
    }

    // The next code point of a UTF-8 text from `at` on (SDL_EVENT_TEXT_INPUT's text), which it moves past it; 0 at the
    // end. A byte that starts no well-formed sequence is skipped, one at a time.
    constexpr char32_t nextCodePoint(std::string_view utf8, std::size_t& at) noexcept
    {
        while (at < utf8.size())
        {
            const auto lead = static_cast<unsigned char>(utf8[at]);
            const std::size_t length = lead < 0x80u ? 1u : (lead & 0xe0u) == 0xc0u ? 2u : (lead & 0xf0u) == 0xe0u ? 3u
                                     : (lead & 0xf8u) == 0xf0u ? 4u : 0u;
            if (length != 0u && at + length <= utf8.size())
            {
                const char32_t cp = web::singleCodePoint(utf8.substr(at, length));
                if (cp != 0 || lead == 0u)
                {
                    at += length;
                    if (cp != 0)
                        return cp;
                    continue;                        // a NUL inside the text: skipped
                }
            }
            ++at;
        }
        return 0;
    }

    // Whether a code point of typed text is a Key::character for the Panel: at or above U+0020 and not the space
    // (the key press delivered it), nor DEL.
    constexpr bool textIsCharacter(char32_t cp) noexcept { return cp > 0x20u && cp != 0x7fu; }

    // ---- the wheel --------------------------------------------------------------------------------------------------

    inline constexpr double kWheelPerUnitApple = 5.0 / 256.0;    // SDL's tenth of a point as JUCE's 0.5 / 256 a point

    // SDL_EVENT_MOUSE_WHEEL's x and y as the Panel's dx, dy, smooth and reversed (x, y and mods are the host's to
    // fill). `flipped`: its direction is SDL_MOUSEWHEEL_FLIPPED.
    constexpr WheelEvent wheelFromSdl(float sdlX, float sdlY, bool flipped, bool apple) noexcept
    {
        WheelEvent w;
        const auto whole = [](float v) { return v == static_cast<float>(static_cast<long>(v)); };
        const double unit = apple ? kWheelPerUnitApple : web::kWheelPerNotch;
        // 0 - x, not -x: no wheel on an axis is +0, as JUCE delivers it.
        w.dx = static_cast<float>((0.0 - static_cast<double>(sdlX)) * unit);
        w.dy = static_cast<float>(static_cast<double>(sdlY) * unit);
        w.smooth = apple || !(whole(sdlX) && whole(sdlY));
        w.reversed = flipped;
        return w;
    }
}

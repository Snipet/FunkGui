#pragma once

// A browser's input as a Panel's (v0.13.0; FCompressor ADR-93, web Sprint C): the pure part of WebHost's input
// conversion, the counterpart of what EditorHost does with JUCE's events (src/gpu/EditorHost.cpp "Input"). Plain C++,
// header-only, no Emscripten and no DOM: WebHost (src/web/WebHost.cpp) reads the DOM event's fields in JavaScript and
// hands them here as numbers and text, so every rule below is tested natively and under node (fg.web.input) and a
// browser is needed only to prove the wiring (test/web/host.cpp).
//
// The rules are JUCE's, so that a Panel sees in a browser what it sees in a plug-in window:
// - Modifiers. On Apple's platforms cmd is Meta (the Command key) and ctrl is Control. Elsewhere Ctrl is the command
//   key and sets cmd AND ctrl, and Meta (the Windows key) is nothing: JUCE's commandModifier is ctrlModifier there
//   (juce_ModifierKeys.h). A product's chord that reads cmd alone (FCompressor's undo) works on both.
// - The popup rule (JUCE's isPopupMenu): the right button, or on Apple's platforms Ctrl with any button.
// - Positions: client px to the Panel's logical px through the canvas's box as the browser lays it out, so the UI
//   zoom and any scaling the page applies are divided out together. Fractional client px stay fractional.
// - Keys: EditorHost's table. A named key the Panel knows, the space bar (with its character), else exactly one
//   character of text at or above U+0020, lower-cased A..Z while cmd is held (JUCE sends the key code and no text
//   then, which is what HeadlessHost::keys gives). Everything else is refused and stays the browser's: modifier keys
//   ("Shift"), dead keys ("Dead"), function keys, an IME's "Process".
// - The wheel in JUCE's units and sign (positive dy = wheel up, away from the user; the DOM's deltaY is the opposite).
//   Pixel mode, which Chrome reports for trackpads and for mouse wheels alike, is JUCE's precise macOS device: 0.5 /
//   256 per px, smooth. Line and page mode are a notched wheel, not smooth: 50 / 256 per notch (JUCE on X11), a notch
//   being three lines or one page. A browser tells a page neither whether the direction is inverted ("natural"
//   scrolling) nor whether the event is momentum: `reversed` and `inertial` are always false.
// - Click counts: a DOM pointerdown carries none (its detail is 0), so they are counted here by JUCE's rule
//   (juce_MouseInputSourceImpl.h): a press counts one more than the run of earlier presses it follows, each within
//   400 ms of it (800 ms for the second and third before it), less than 8 px from it in x and in y and with the same
//   button, at most 4; and it counts 1 once the pointer has moved 4 px from the press or the button was held for more
//   than 300 ms. A host sends pointerUp and then, for a count of 2 or more, doubleClick (JUCE's order).
// - The Panel's Cursor as a CSS cursor keyword.

#include <funkgui/panel/Input.h>

#include <string_view>

namespace funkgui::web
{
    // ---- platform and modifiers -------------------------------------------------------------------------------------

    // Whether a browser's platform name (navigator.userAgentData.platform: "macOS"; navigator.platform: "MacIntel",
    // "iPhone", "iPad") is one of Apple's: where the command key is Meta and Ctrl-click is the popup click.
    constexpr bool applePlatform(std::string_view platform) noexcept
    {
        constexpr std::string_view names[] = { "mac", "iphone", "ipad", "ipod" };
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
        for (const std::string_view name : names)
            for (std::size_t at = 0; at + name.size() <= platform.size(); ++at)
            {
                std::size_t i = 0;
                while (i < name.size() && lower(platform[at + i]) == name[i])
                    ++i;
                if (i == name.size())
                    return true;
            }
        return false;
    }

    // A DOM event's shiftKey, ctrlKey, altKey and metaKey as the Panel's Mods.
    constexpr Mods modsFromDom(bool shift, bool ctrl, bool alt, bool meta, bool apple) noexcept
    {
        Mods m;
        m.shift = shift;
        m.cmd = apple ? meta : ctrl;
        m.alt = alt;
        m.ctrl = ctrl;
        return m;
    }

    // MouseEvent.button: the buttons a host takes. The middle button (1) and the back and forward buttons are ignored.
    inline constexpr int kButtonLeft = 0, kButtonRight = 2;

    constexpr bool takesButton(int button) noexcept { return button == kButtonLeft || button == kButtonRight; }

    // PointerEvent::popup: the right button, or Ctrl on Apple's platforms.
    constexpr bool popupFromDom(bool rightButton, bool ctrl, bool apple) noexcept
    {
        return rightButton || (apple && ctrl);
    }

    // ---- positions --------------------------------------------------------------------------------------------------

    // One client coordinate in the Panel's logical px: `origin` and `cssSize` are the canvas's box on that axis
    // (getBoundingClientRect), `logicalSize` the Panel's width or height. With the canvas sized by WebHost this is the
    // division by the UI zoom EditorHost does, and exact wherever that is. 0 for a box with no size.
    constexpr float toLogical(double client, double origin, double cssSize, int logicalSize) noexcept
    {
        return cssSize > 0.0 ? static_cast<float>((client - origin) * static_cast<double>(logicalSize) / cssSize)
                             : 0.0f;
    }

    // ---- keys -------------------------------------------------------------------------------------------------------

    // The one code point a UTF-8 text holds, or 0 when it holds none, more than one, or is not well-formed UTF-8.
    constexpr char32_t singleCodePoint(std::string_view utf8) noexcept
    {
        if (utf8.empty())
            return 0;
        const auto byte = [utf8](std::size_t i) { return static_cast<char32_t>(static_cast<unsigned char>(utf8[i])); };
        const char32_t lead = byte(0);
        std::size_t length = 0;
        char32_t cp = 0, smallest = 0;
        if (lead < 0x80u)                { length = 1; cp = lead;         smallest = 0; }
        else if ((lead & 0xe0u) == 0xc0u) { length = 2; cp = lead & 0x1fu; smallest = 0x80u; }
        else if ((lead & 0xf0u) == 0xe0u) { length = 3; cp = lead & 0x0fu; smallest = 0x800u; }
        else if ((lead & 0xf8u) == 0xf0u) { length = 4; cp = lead & 0x07u; smallest = 0x10000u; }
        if (length == 0 || utf8.size() != length)
            return 0;
        for (std::size_t i = 1; i < length; ++i)
        {
            if ((byte(i) & 0xc0u) != 0x80u)
                return 0;
            cp = (cp << 6) | (byte(i) & 0x3fu);
        }
        const bool surrogate = cp >= 0xd800u && cp <= 0xdfffu;
        return cp < smallest || cp > 0x10ffffu || surrogate ? 0 : cp;
    }

    // A keydown's `key` (KeyboardEvent.key, as UTF-8) with its modifiers as the Panel's KeyEvent; false, and `out`
    // not a key to deliver, for a key no Panel takes (the header comment has the rule).
    constexpr bool keyFromDom(std::string_view key, Mods mods, KeyEvent& out) noexcept
    {
        struct Named
        {
            std::string_view name;
            Key key;
        };
        constexpr Named named[] = {
            { "Tab", Key::tab },         { "ArrowUp", Key::up },       { "ArrowDown", Key::down },
            { "ArrowLeft", Key::left },  { "ArrowRight", Key::right }, { "PageUp", Key::pageUp },
            { "PageDown", Key::pageDown }, { "Home", Key::home },      { "End", Key::end },
            { "Escape", Key::escape },   { "Enter", Key::enter },      { "Backspace", Key::backspace },
            { "Delete", Key::del },
        };
        out = KeyEvent{};
        out.mods = mods;
        for (const Named& n : named)
            if (key == n.name)
            {
                out.key = n.key;
                return true;
            }
        if (key == " ")
        {
            out.key = Key::space;
            out.ch = U' ';
            return true;
        }
        const char32_t cp = singleCodePoint(key);
        if (cp < 0x20u)
            return false;                            // no text: a named key the table lacks, a dead key, a modifier
        out.key = Key::character;
        out.ch = mods.cmd && cp >= U'A' && cp <= U'Z' ? cp - U'A' + U'a' : cp;
        return true;
    }

    // ---- the wheel --------------------------------------------------------------------------------------------------

    inline constexpr int kDeltaPixel = 0, kDeltaLine = 1, kDeltaPage = 2;     // WheelEvent.deltaMode

    inline constexpr double kWheelPerPixel = 0.5 / 256.0;     // JUCE on macOS, a precise device: per point scrolled
    inline constexpr double kWheelPerNotch = 50.0 / 256.0;    // JUCE on X11: one notch of a wheel
    inline constexpr double kLinesPerNotch = 3.0;             // what a browser in line mode reports for one notch

    // A DOM wheel event's deltas as the Panel's: dx, dy and smooth (x, y and mods are the host's to fill; reversed
    // and inertial stay false). An unknown deltaMode is read as pixels.
    constexpr WheelEvent wheelFromDom(double deltaX, double deltaY, int deltaMode) noexcept
    {
        WheelEvent w;
        const bool notched = deltaMode == kDeltaLine || deltaMode == kDeltaPage;
        const double unit = deltaMode == kDeltaLine ? kWheelPerNotch / kLinesPerNotch
                          : deltaMode == kDeltaPage ? kWheelPerNotch
                                                    : kWheelPerPixel;
        // 0 - delta, not -delta: no wheel on an axis is +0, as JUCE delivers it.
        w.dx = static_cast<float>(deltaMode == kDeltaLine ? (0.0 - deltaX) / kLinesPerNotch * kWheelPerNotch
                                                          : (0.0 - deltaX) * unit);
        w.dy = static_cast<float>(deltaMode == kDeltaLine ? (0.0 - deltaY) / kLinesPerNotch * kWheelPerNotch
                                                          : (0.0 - deltaY) * unit);
        w.smooth = !notched;
        return w;
    }

    // ---- click counts -----------------------------------------------------------------------------------------------

    // JUCE's multiple-click count over DOM pointer events (the header comment has the rule). Times are the events'
    // timeStamp in ms, positions their client px.
    class ClickCounter
    {
    public:
        static constexpr double kMultiClickMs = 400.0;       // MouseEvent::getDoubleClickTimeout()
        static constexpr double kLongPressMs = 300.0;
        static constexpr double kNearPx = 8.0;                // a mouse (JUCE takes 25 for a touch)
        static constexpr double kMovedPx = 4.0;
        static constexpr int    kMost = 4;

        // A press. Returns its count: PointerEvent::clicks of the pointerDown.
        constexpr int down(double timeMs, double x, double y, int button) noexcept
        {
            for (int i = kMost - 1; i > 0; --i)
                downs_[i] = downs_[i - 1];
            downs_[0] = Down{ timeMs, x, y, button, true };
            moved_ = false;
            return count(timeMs);
        }

        // The pointer at (x, y) while the button is held: 4 px from the press makes it a drag for good.
        constexpr void moved(double x, double y) noexcept
        {
            const double dx = x - downs_[0].x, dy = y - downs_[0].y;
            moved_ = moved_ || dx * dx + dy * dy >= kMovedPx * kMovedPx;
        }

        // The count of the last press as it stands at `nowMs`: PointerEvent::clicks of a drag or of the release.
        constexpr int count(double nowMs) const noexcept
        {
            if (moved_ || nowMs > downs_[0].time + kLongPressMs)
                return 1;
            int n = 1;
            for (int i = 1; i < kMost; ++i)
            {
                if (!partOfRun(downs_[i], kMultiClickMs * (i < 2 ? i : 2)))
                    break;
                ++n;
            }
            return n;
        }

    private:
        struct Down
        {
            double time = 0.0, x = 0.0, y = 0.0;
            int    button = 0;
            bool   set = false;
        };

        constexpr bool partOfRun(const Down& earlier, double withinMs) const noexcept
        {
            const Down& last = downs_[0];
            const double dx = last.x - earlier.x, dy = last.y - earlier.y;
            return earlier.set && last.time - earlier.time < withinMs && (dx < 0.0 ? -dx : dx) < kNearPx
                && (dy < 0.0 ? -dy : dy) < kNearPx && last.button == earlier.button;
        }

        Down downs_[kMost]{};
        bool moved_ = false;
    };

    // ---- the cursor -------------------------------------------------------------------------------------------------

    // The CSS cursor keyword for a Panel's Cursor (EditorHost's five).
    constexpr const char* cssCursor(Cursor c) noexcept
    {
        switch (c)
        {
            case Cursor::normal:       return "default";
            case Cursor::leftRight:    return "ew-resize";
            case Cursor::upDown:       return "ns-resize";
            case Cursor::pointingHand: return "pointer";
            case Cursor::crosshair:    return "crosshair";
        }
        return "default";
    }
}

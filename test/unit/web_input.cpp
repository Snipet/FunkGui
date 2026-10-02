// FUNKGUI_TEST name=fg.web.input timeout=120 gpu=0 links=harness
//
// fg.web.input (v0.13.0; FCompressor ADR-93, web Sprint C): web/WebInput.h, the pure part of WebHost's input
// conversion, one group of spec rows per rule of that header. Header-only and free of Emscripten, so the same rows
// run natively with and without JUCE and as wasm32 under node; what a browser adds (the DOM events reaching these
// functions) is test/web/host.cpp's.
//   platform.*  which browser platform names are Apple's
//   mods.*      shift, ctrl, alt and meta as Mods: on Apple's platforms cmd is Meta; elsewhere Ctrl sets cmd AND ctrl
//               and Meta is nothing (JUCE)
//   button.*    the buttons a host takes (left and right; never the middle one)
//   popup.*     the right button; Ctrl on Apple's platforms only
//   logical.*   client px to the Panel's px at 100, 125, 150 and 175 %, bit for bit, with a fractional canvas origin
//   key.*       EditorHost's key table entry by entry; the space bar's character; A..Z lower-cased under cmd only;
//               text past ASCII; dead keys, modifier keys, function keys and damaged text refused
//   wheel.*     the DOM's sign and units as JUCE's: 512 px = -1.0 smooth; a notch of three lines or one page =
//               50 / 256, not smooth; never reversed, never inertial; no wheel on an axis is +0
//   clicks.*    JUCE's click count: 400 ms, 800 ms for the second and third press before, 8 px in x and in y, the same
//               button, at most 4, and 1 after a move of 4 px or a hold of more than 300 ms
//   cursor.*    the five CSS cursor keywords
// Spec rows only.

#include <funkgui/panel/Input.h>
#include <funkgui/test/Harness.h>
#include <funkgui/web/WebInput.h>

#include <bit>
#include <cstdint>
#include <cstring>
#include <string>

namespace T = funkgui::test;
namespace web = funkgui::web;
using funkgui::Cursor;
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

    // The rules are usable in constant expressions (a host may table them).
    static_assert(web::applePlatform("MacIntel") && !web::applePlatform("Win32"));
    static_assert(web::modsFromDom(false, true, false, false, false).cmd);
    static_assert(web::popupFromDom(true, false, false) && !web::popupFromDom(false, true, false));
    static_assert(web::toLogical(56.25, 0.0, 360.0, 240) == 37.5f);
    static_assert(web::singleCodePoint("\xE2\x88\x92") == 0x2212u);
    static_assert(web::wheelFromDom(0.0, 512.0, web::kDeltaPixel).dy == -1.0f);
    static_assert(web::cssCursor(Cursor::pointingHand)[0] == 'p');
}

int main(int argc, char** argv)
{
    T::Probe P("fg.web.input", "", argc, argv);

    // ---- platform ---------------------------------------------------------------------------------------------------
    P.eq("platform.macintel", web::applePlatform("MacIntel"), 1);
    P.eq("platform.macos", web::applePlatform("macOS"), 1);                // navigator.userAgentData.platform
    P.eq("platform.iphone", web::applePlatform("iPhone"), 1);
    P.eq("platform.ipad", web::applePlatform("iPad"), 1);
    P.eq("platform.win32", web::applePlatform("Win32"), 0);
    P.eq("platform.windows", web::applePlatform("Windows"), 0);
    P.eq("platform.linux", web::applePlatform("Linux x86_64"), 0);
    P.eq("platform.android", web::applePlatform("Android"), 0);
    P.eq("platform.empty", web::applePlatform(""), 0);

    // ---- mods -------------------------------------------------------------------------------------------------------
    P.eq("mods.apple.none", bits(web::modsFromDom(false, false, false, false, true)), 0);
    P.eq("mods.apple.shift", bits(web::modsFromDom(true, false, false, false, true)), kShift);
    P.eq("mods.apple.alt", bits(web::modsFromDom(false, false, true, false, true)), kAlt);
    P.eq("mods.apple.meta_is_cmd", bits(web::modsFromDom(false, false, false, true, true)), kCmd);
    P.eq("mods.apple.ctrl_is_ctrl_only", bits(web::modsFromDom(false, true, false, false, true)), kCtrl);
    P.eq("mods.apple.all", bits(web::modsFromDom(true, true, true, true, true)), kShift | kCmd | kAlt | kCtrl);
    P.eq("mods.other.none", bits(web::modsFromDom(false, false, false, false, false)), 0);
    P.eq("mods.other.shift", bits(web::modsFromDom(true, false, false, false, false)), kShift);
    P.eq("mods.other.alt", bits(web::modsFromDom(false, false, true, false, false)), kAlt);
    P.eq("mods.other.ctrl_is_cmd_and_ctrl", bits(web::modsFromDom(false, true, false, false, false)), kCmd | kCtrl);
    P.eq("mods.other.meta_is_nothing", bits(web::modsFromDom(false, false, false, true, false)), 0);
    P.eq("mods.other.ctrl_shift", bits(web::modsFromDom(true, true, false, false, false)), kShift | kCmd | kCtrl);

    // ---- buttons and the popup rule ---------------------------------------------------------------------------------
    P.eq("button.left_taken", web::takesButton(0), 1);
    P.eq("button.middle_ignored", web::takesButton(1), 0);
    P.eq("button.right_taken", web::takesButton(2), 1);
    P.eq("button.back_ignored", web::takesButton(3) || web::takesButton(4) || web::takesButton(-1), 0);
    P.eq("popup.left", web::popupFromDom(false, false, true) || web::popupFromDom(false, false, false), 0);
    P.eq("popup.right_apple", web::popupFromDom(true, false, true), 1);
    P.eq("popup.right_other", web::popupFromDom(true, false, false), 1);
    P.eq("popup.ctrl_click_apple", web::popupFromDom(false, true, true), 1);
    P.eq("popup.ctrl_click_other", web::popupFromDom(false, true, false), 0);

    // ---- client px to logical px ------------------------------------------------------------------------------------
    // A 240 x 160 Panel on a canvas at (16.5, 40.25) whose box is the zoomed size: (x z, y z) from the canvas's
    // corner is (x, y), bit for bit (every product here is exact, as in fg.editorhost.zoom's pointer rows).
    for (const int z : { 100, 125, 150, 175 })
    {
        const std::string k = "logical." + std::to_string(z);
        const double s = z / 100.0, left = 16.5, top = 40.25, cssW = 240.0 * s, cssH = 160.0 * s;
        const float xs[] = { 37.5f, 239.0f, 0.0f, 12.25f }, ys[] = { 12.25f, 0.5f, 0.0f, 159.5f };
        bool exact = true;
        for (int i = 0; i < 4; ++i)
        {
            exact = exact && sameBits(web::toLogical(left + xs[i] * s, left, cssW, 240), xs[i]);
            exact = exact && sameBits(web::toLogical(top + ys[i] * s, top, cssH, 160), ys[i]);
        }
        P.eq(k + ".exact", exact, 1);
        // Outside the canvas (a captured drag): negative and past the size, still exact.
        P.eq(k + ".left_of_canvas", sameBits(web::toLogical(left - 10.0 * s, left, cssW, 240), -10.0f), 1);
        P.eq(k + ".past_the_bottom", sameBits(web::toLogical(top + 200.0 * s, top, cssH, 160), 200.0f), 1);
    }
    // A page that scales the canvas (a CSS transform, a flex shrink): the box is what counts, not the zoom.
    P.eq("logical.scaled_box", sameBits(web::toLogical(100.0, 0.0, 480.0, 240), 50.0f), 1);
    P.eq("logical.no_box", sameBits(web::toLogical(100.0, 0.0, 0.0, 240), 0.0f), 1);

    // ---- keys -------------------------------------------------------------------------------------------------------
    {
        const struct { const char* row; const char* dom; Key key; } table[] = {
            { "tab", "Tab", Key::tab },             { "up", "ArrowUp", Key::up },
            { "down", "ArrowDown", Key::down },     { "left", "ArrowLeft", Key::left },
            { "right", "ArrowRight", Key::right },  { "page_up", "PageUp", Key::pageUp },
            { "page_down", "PageDown", Key::pageDown }, { "home", "Home", Key::home },
            { "end", "End", Key::end },             { "escape", "Escape", Key::escape },
            { "enter", "Enter", Key::enter },       { "backspace", "Backspace", Key::backspace },
            { "delete", "Delete", Key::del },
        };
        for (const auto& t : table)
        {
            KeyEvent ev;
            ev.ch = U'x';                            // a named key carries no character
            const bool taken = web::keyFromDom(t.dom, Mods{}, ev);
            P.eq(std::string("key.") + t.row, taken && ev.key == t.key && ev.ch == 0, 1);
        }
        KeyEvent ev;
        P.eq("key.space", web::keyFromDom(" ", Mods{}, ev) && ev.key == Key::space, 1);
        P.eq("key.space_char", static_cast<int64_t>(ev.ch), U' ');

        Mods shiftAlt;
        shiftAlt.shift = shiftAlt.alt = true;
        P.eq("key.mods_kept", web::keyFromDom("ArrowLeft", shiftAlt, ev) && bits(ev.mods) == (kShift | kAlt), 1);

        P.eq("key.character", web::keyFromDom("a", Mods{}, ev) && ev.key == Key::character, 1);
        P.eq("key.character_ch", static_cast<int64_t>(ev.ch), U'a');
        P.eq("key.upper_without_cmd", web::keyFromDom("A", Mods{}, ev) && ev.ch == U'A', 1);
        Mods shift;
        shift.shift = true;
        P.eq("key.upper_with_shift", web::keyFromDom("Z", shift, ev) && ev.ch == U'Z', 1);
        Mods cmd;
        cmd.cmd = true;
        P.eq("key.cmd_lower_stays", web::keyFromDom("z", cmd, ev) && ev.ch == U'z' && ev.key == Key::character, 1);
        P.eq("key.cmd_lowercases", web::keyFromDom("Z", cmd, ev) && ev.ch == U'z', 1);
        Mods cmdShift;                               // Ctrl+Shift+Z off a Mac arrives as "Z": the redo chord
        cmdShift.cmd = cmdShift.shift = cmdShift.ctrl = true;
        P.eq("key.cmd_shift_lowercases",
             web::keyFromDom("Z", cmdShift, ev) && ev.ch == U'z' && bits(ev.mods) == (kShift | kCmd | kCtrl), 1);
        Mods ctrlOnly;                               // Control on a Mac is not the command key
        ctrlOnly.ctrl = true;
        P.eq("key.ctrl_alone_keeps_case", web::keyFromDom("Z", ctrlOnly, ev) && ev.ch == U'Z', 1);
        P.eq("key.cmd_digit", web::keyFromDom("1", cmd, ev) && ev.ch == U'1', 1);
        P.eq("key.cmd_past_ascii_kept", web::keyFromDom("\xC3\x84", cmd, ev) && ev.ch == 0xc4u, 1);   // U+00C4
        P.eq("key.punctuation", web::keyFromDom("-", Mods{}, ev) && ev.ch == U'-', 1);
        P.eq("key.tilde", web::keyFromDom("~", Mods{}, ev) && ev.ch == U'~', 1);
        P.eq("key.two_bytes", web::keyFromDom("\xC3\xA9", Mods{}, ev) && ev.ch == 0xe9u, 1);          // U+00E9
        P.eq("key.three_bytes", web::keyFromDom("\xE2\x88\x92", Mods{}, ev) && ev.ch == 0x2212u, 1);  // U+2212
        P.eq("key.four_bytes", web::keyFromDom("\xF0\x9D\x84\x9E", Mods{}, ev) && ev.ch == 0x1d11eu, 1);

        const char* refused[] = { "Shift",   "Control",     "Alt",       "Meta",   "CapsLock", "Dead",  "F5",
                                  "Process", "Unidentified", "ContextMenu", "Insert", "ab",       "",      "\t",
                                  "\x1b",    "\xC3",        "\xC3\x28",  "\xC0\x80", "\xED\xA0\x80", "\xE2\x88\x92x" };
        int n = 0;
        for (const char* r : refused)
        {
            KeyEvent none;
            n += web::keyFromDom(r, Mods{}, none) ? 0 : 1;
        }
        P.eq("key.refused", n, static_cast<int64_t>(sizeof refused / sizeof refused[0]));
        P.eq("key.refused.dead", web::keyFromDom("Dead", Mods{}, ev), 0);
        P.eq("key.refused.shift", web::keyFromDom("Shift", Mods{}, ev), 0);
        P.eq("key.refused.f5", web::keyFromDom("F5", cmd, ev), 0);
    }

    // ---- the wheel --------------------------------------------------------------------------------------------------
    {
        const WheelEvent px = web::wheelFromDom(0.0, 512.0, web::kDeltaPixel);
        P.eq("wheel.pixel_512_is_minus_one", sameBits(px.dy, -1.0f), 1);
        P.eq("wheel.pixel_smooth", px.smooth, 1);
        P.eq("wheel.pixel_no_x_is_plus_zero", sameBits(px.dx, 0.0f), 1);
        P.eq("wheel.never_reversed_or_inertial", px.reversed || px.inertial, 0);
        const WheelEvent up = web::wheelFromDom(0.0, -120.0, web::kDeltaPixel);     // Chrome: one notch, away
        P.near("wheel.pixel_up_is_positive", up.dy, 120.0 * 0.5 / 256.0, 1.0e-9);
        const WheelEvent right = web::wheelFromDom(256.0, 0.0, web::kDeltaPixel);
        P.eq("wheel.pixel_x", sameBits(right.dx, -0.5f) && sameBits(right.dy, 0.0f), 1);
        const WheelEvent line = web::wheelFromDom(0.0, 3.0, web::kDeltaLine);       // Firefox: one notch, towards
        P.eq("wheel.line_notch", sameBits(line.dy, -50.0f / 256.0f), 1);
        P.eq("wheel.line_not_smooth", line.smooth, 0);
        const WheelEvent lineUp = web::wheelFromDom(-6.0, -6.0, web::kDeltaLine);
        P.eq("wheel.line_two_notches_up", sameBits(lineUp.dy, 100.0f / 256.0f) && sameBits(lineUp.dx, lineUp.dy), 1);
        const WheelEvent page = web::wheelFromDom(0.0, 1.0, web::kDeltaPage);
        P.eq("wheel.page_notch", sameBits(page.dy, -50.0f / 256.0f), 1);
        P.eq("wheel.page_not_smooth", page.smooth || page.reversed || page.inertial, 0);
        const WheelEvent odd = web::wheelFromDom(0.0, 512.0, 7);
        P.eq("wheel.unknown_mode_is_pixels", sameBits(odd.dy, -1.0f) && odd.smooth, 1);
        const WheelEvent still = web::wheelFromDom(0.0, 0.0, web::kDeltaLine);
        P.eq("wheel.line_zero_is_plus_zero", sameBits(still.dx, 0.0f) && sameBits(still.dy, 0.0f), 1);
    }

    // ---- click counts -----------------------------------------------------------------------------------------------
    {
        using web::ClickCounter;
        ClickCounter first;
        P.eq("clicks.first", first.down(1000.0, 50.0, 60.0, 0), 1);
        P.eq("clicks.first_at_release", first.count(1080.0), 1);

        ClickCounter a;
        a.down(1000.0, 50.0, 60.0, 0);
        P.eq("clicks.second_at_399ms", a.down(1399.0, 50.0, 60.0, 0), 2);
        P.eq("clicks.second_at_release", a.count(1450.0), 2);
        ClickCounter b;
        b.down(1000.0, 50.0, 60.0, 0);
        P.eq("clicks.second_at_400ms_is_one", b.down(1400.0, 50.0, 60.0, 0), 1);
        ClickCounter c;
        c.down(1000.0, 50.0, 60.0, 0);
        P.eq("clicks.second_at_401ms_is_one", c.down(1401.0, 50.0, 60.0, 0), 1);

        ClickCounter near;
        near.down(1000.0, 50.0, 60.0, 0);
        P.eq("clicks.second_within_8px", near.down(1100.0, 57.75, 52.25, 0), 2);
        ClickCounter farX;
        farX.down(1000.0, 50.0, 60.0, 0);
        P.eq("clicks.second_8px_in_x_is_one", farX.down(1100.0, 58.0, 60.0, 0), 1);
        ClickCounter farY;
        farY.down(1000.0, 50.0, 60.0, 0);
        P.eq("clicks.second_8px_in_y_is_one", farY.down(1100.0, 50.0, 52.0, 0), 1);
        ClickCounter other;
        other.down(1000.0, 50.0, 60.0, 0);
        P.eq("clicks.other_button_is_one", other.down(1100.0, 50.0, 60.0, 2), 1);

        // The third and the fourth press: 400 ms from the press before, 800 ms from the two before that.
        ClickCounter run;
        run.down(1000.0, 50.0, 60.0, 0);
        run.down(1300.0, 50.0, 60.0, 0);
        P.eq("clicks.third", run.down(1600.0, 50.0, 60.0, 0), 3);
        P.eq("clicks.fourth_within_800ms_of_the_first", run.down(1750.0, 50.0, 60.0, 0), 4);
        P.eq("clicks.fifth_is_still_four", run.down(1760.0, 50.0, 60.0, 0), 4);
        ClickCounter slow;
        slow.down(1000.0, 50.0, 60.0, 0);
        slow.down(1300.0, 50.0, 60.0, 0);
        slow.down(1600.0, 50.0, 60.0, 0);
        P.eq("clicks.fourth_900ms_after_the_first_is_three", slow.down(1900.0, 50.0, 60.0, 0), 3);
        // The quirk that follows from JUCE's rule: the third press is compared with the first directly, so it counts 3
        // within 800 ms of it even when the second press was too late to be a double click.
        ClickCounter late;
        late.down(1000.0, 50.0, 60.0, 0);
        P.eq("clicks.second_410ms_after_is_one", late.down(1410.0, 50.0, 60.0, 0), 1);
        P.eq("clicks.third_799ms_after_the_first", late.down(1799.0, 50.0, 60.0, 0), 3);
        ClickCounter later;
        later.down(1000.0, 50.0, 60.0, 0);
        later.down(1410.0, 50.0, 60.0, 0);
        P.eq("clicks.third_800ms_after_the_first_is_two", later.down(1800.0, 50.0, 60.0, 0), 2);

        // A drag or a long press counts 1 from then on; the next press starts from the presses as they were.
        ClickCounter drag;
        drag.down(1000.0, 50.0, 60.0, 0);
        drag.down(1100.0, 50.0, 60.0, 0);
        drag.moved(53.9, 60.0);
        P.eq("clicks.moved_under_4px_keeps", drag.count(1150.0), 2);
        drag.moved(52.9, 62.9);                      // 4.1 px along the diagonal
        P.eq("clicks.moved_4px_is_one", drag.count(1150.0), 1);
        drag.moved(50.0, 60.0);                      // back at the press: still a drag
        P.eq("clicks.moved_back_is_still_one", drag.count(1150.0), 1);
        P.eq("clicks.press_after_a_drag", drag.down(1200.0, 50.0, 60.0, 0), 3);
        ClickCounter hold;
        hold.down(1000.0, 50.0, 60.0, 0);
        hold.down(1100.0, 50.0, 60.0, 0);
        P.eq("clicks.held_300ms_keeps", hold.count(1400.0), 2);
        P.eq("clicks.held_over_300ms_is_one", hold.count(1400.5), 1);
    }

    // ---- the cursor -------------------------------------------------------------------------------------------------
    P.eq("cursor.normal", std::strcmp(web::cssCursor(Cursor::normal), "default"), 0);
    P.eq("cursor.left_right", std::strcmp(web::cssCursor(Cursor::leftRight), "ew-resize"), 0);
    P.eq("cursor.up_down", std::strcmp(web::cssCursor(Cursor::upDown), "ns-resize"), 0);
    P.eq("cursor.pointing_hand", std::strcmp(web::cssCursor(Cursor::pointingHand), "pointer"), 0);
    P.eq("cursor.crosshair", std::strcmp(web::cssCursor(Cursor::crosshair), "crosshair"), 0);

    return P.finish();
}

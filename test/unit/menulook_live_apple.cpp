// FUNKGUI_TEST name=fg.menulook.live timeout=120 gpu=0 labels=live
//
// fg.menulook.live: MenuLook's keys on a real menu window (juce/MenuLook.h "Keys"; 0.25.0). Labels fg;live: it opens a
// popup menu on the desktop, so it needs a window server and is never in `verify`. Run it serialised:
//   lockf -k -t 900 /tmp/fcmp-gui.lock ctest --test-dir <build> -R fg.menulook.live
//
// A menu of 40 rows (a header, a disabled row, the rest choosable) is shown in this look with one column, so it
// scrolls, and the 30th row initially selected. Every key is given to the menu window's peer as the system gives it
// (ComponentPeer::handleKeyPress, which runs the window's key listeners first), with no run-loop turn between the keys,
// so the pointer's own highlighting never runs. The row that is highlighted is the one whose accessibility handler has
// JUCE's focus, which JUCE gives a row as it highlights it.
//   initial.*  the selected row opens highlighted and between the scroll strips (JUCE's own scrolling leaves it under
//              the bottom one)
//   keys.*     Down, Up (wrapping past the header), End, Home, Page Down, each to the right row, every one between the
//              strips; a disabled row is passed over
//   type.*     type-ahead: a letter, a longer prefix, the same letter again, a pause, a key with no row, Space inside a
//              prefix
//   choose.*   Return chooses the highlighted row (JUCE's key), from the run loop; Escape dismisses with 0
// Spec rows only.

#include <funkgui/core/Theme.h>
#include <funkgui/juce/MenuLook.h>
#include <funkgui/test/Harness.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <CoreFoundation/CoreFoundation.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

namespace T = funkgui::test;
using funkgui::MenuLook;

namespace
{
    void spin(double seconds)
    {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
    }

    const char* const kWords[] = { "Acid", "Bass", "Bell", "Brass", "Choir", "Chord", "Drone", "Drum", "Echo", "Flute",
                                   "Glass", "Harp", "Keys", "Lead", "Mallet", "Noise", "Organ", "Pad", "Pluck", "Reed",
                                   "Saw", "Sine", "Strings", "Sub", "Sweep", "Synth", "Tape", "Tine", "Vox", "Wash",
                                   "Wave", "Whirl", "Wind", "Wood", "Wurli", "Xylo", "Yarn", "Zap", "Zither", "Zone" };

    // Row 0 is the header; rows 1..40 are the words (ids 1..40); "Chord" (id 6) is disabled.
    juce::PopupMenu bigMenu()
    {
        juce::PopupMenu m;
        m.addSectionHeader("Sounds");
        for (int i = 0; i < 40; ++i)
            m.addItem(i + 1, i == 5 ? "Chord pad" : kWords[i], i != 5);
        return m;
    }

    // The menu window's row index that holds JUCE's accessibility focus, or -1.
    int highlighted(juce::Component& window)
    {
        for (int i = 0; i < window.getNumChildComponents(); ++i)
            if (const juce::AccessibilityHandler* h = window.getChildComponent(i)->getAccessibilityHandler())
                if (h->hasFocus(false))
                    return i;
        return -1;
    }

    // The row is shown between the scroll strips; the first and the last row may sit in the panel's padding instead,
    // where the menu cannot scroll further and JUCE shows no strip.
    bool betweenStrips(juce::Component& window, int index)
    {
        juce::Component* row = index >= 0 && index < window.getNumChildComponents() ? window.getChildComponent(index)
                                                                                   : nullptr;
        if (row == nullptr)
            return false;
        std::printf("row %d  y %d bottom %d visible %d  window h %d\n", index, row->getY(), row->getBottom(),
                    row->isVisible() ? 1 : 0, window.getHeight());
        const int border = MenuLook::kShadowPx + MenuLook::kPadPx;
        const int top = index == 1 ? border : 24;
        const int bottom = window.getHeight() - (index == window.getNumChildComponents() - 1 ? border : 24);
        return row->isVisible() && row->getY() >= top && row->getBottom() <= bottom;
    }

    bool press(juce::Component& window, const juce::KeyPress& key)
    {
        juce::ComponentPeer* peer = window.getPeer();
        return peer != nullptr && peer->handleKeyPress(key);
    }

    bool type(juce::Component& window, juce::juce_wchar c)
    {
        return press(window, juce::KeyPress(static_cast<int>(c), juce::ModifierKeys(), c));
    }
}

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;
    T::Probe P("fg.menulook.live", "", argc, argv);
    const auto look = std::make_unique<MenuLook>(funkgui::Theme::graphite());

    int chosen = -1;
    {
        juce::PopupMenu m = bigMenu();
        m.setLookAndFeel(look.get());
        // Low on the display, so the 40 rows cannot fit and the window has room to move: JUCE's scrolling to the
        // selected row moves it.
        const juce::Rectangle<int> user = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->userArea;
        const juce::Rectangle<int> anchor(user.getX() + 200, user.getCentreY(), 120, 24);
        m.showMenuAsync(juce::PopupMenu::Options()
                            .withTargetScreenArea(look->targetAreaFor(anchor))
                            .withMaximumNumColumns(1)
                            .withInitiallySelectedItem(30)
                            .withItemThatMustBeVisible(30),
                        [&chosen](int id) { chosen = id; });
    }
    juce::Component* window = juce::Component::getCurrentlyModalComponent();
    if (!P.eq("menu.open", window != nullptr && window->getPeer() != nullptr, 1))
        return P.finish();
    juce::Component& w = *window;
    std::printf("window %d %d %d %d, rows %d\n", w.getX(), w.getY(), w.getWidth(), w.getHeight(),
                w.getNumChildComponents());
    P.eq("menu.scrolls", w.getHeight() < 41 * MenuLook::kRowPx, 1);

    P.eq("initial.highlighted", highlighted(w), 30);
    P.eq("initial.between_strips", betweenStrips(w, 30), 1);

    P.eq("keys.down_taken", press(w, juce::KeyPress(juce::KeyPress::downKey)), 1);
    P.eq("keys.down", highlighted(w) == 31 && betweenStrips(w, 31), 1);
    press(w, juce::KeyPress(juce::KeyPress::endKey));
    P.eq("keys.end", highlighted(w) == 40 && betweenStrips(w, 40), 1);
    press(w, juce::KeyPress(juce::KeyPress::downKey));
    P.eq("keys.down_wraps_past_header", highlighted(w) == 1 && betweenStrips(w, 1), 1);
    press(w, juce::KeyPress(juce::KeyPress::upKey));
    P.eq("keys.up_wraps", highlighted(w) == 40 && betweenStrips(w, 40), 1);
    press(w, juce::KeyPress(juce::KeyPress::homeKey));
    P.eq("keys.home", highlighted(w) == 1 && betweenStrips(w, 1), 1);
    for (int i = 0; i < 5; ++i)
        press(w, juce::KeyPress(juce::KeyPress::downKey));
    P.eq("keys.disabled_passed_over", highlighted(w), 7);   // 2, 3, 4, 5, then 7: row 6 ("Chord pad") is disabled
    press(w, juce::KeyPress(juce::KeyPress::pageDownKey));
    const int paged = highlighted(w);
    P.eq("keys.page_down", paged > 7 && paged <= 7 + w.getHeight() / MenuLook::kRowPx && betweenStrips(w, paged), 1);

    press(w, juce::KeyPress(juce::KeyPress::homeKey));
    P.eq("type.letter_taken", type(w, 'w'), 1);
    P.eq("type.letter", highlighted(w) == 30 && betweenStrips(w, 30), 1);       // "Wash"
    type(w, 'i');
    P.eq("type.prefix", highlighted(w), 33);                                     // "Wind"
    type(w, 'z');
    P.eq("type.no_row_stays", highlighted(w), 33);                               // "wiz": none, the key still taken
    std::this_thread::sleep_for(std::chrono::milliseconds(MenuLook::kTypeAheadMs + 100));
    type(w, 'z');
    P.eq("type.after_pause_new_prefix", highlighted(w), 38);                     // "Zap"
    type(w, 'z');
    P.eq("type.same_letter_moves_on", highlighted(w), 39);                       // "Zither"
    std::this_thread::sleep_for(std::chrono::milliseconds(MenuLook::kTypeAheadMs + 100));
    type(w, 'c');
    type(w, 'h');
    type(w, 'o');
    P.eq("type.disabled_passed_over", highlighted(w), 5);                        // "Choir", not "Chord pad"
    std::this_thread::sleep_for(std::chrono::milliseconds(MenuLook::kTypeAheadMs + 100));
    P.eq("type.cmd_key_not_taken", press(w, juce::KeyPress('a', juce::ModifierKeys::commandModifier, 'a')), 0);
    P.eq("type.highlight_kept", highlighted(w), 5);

    P.eq("choose.return_taken", press(w, juce::KeyPress(juce::KeyPress::returnKey)), 1);
    spin(0.3);
    P.eq("choose.chosen", chosen, 5);
    P.eq("choose.closed", juce::Component::getCurrentlyModalComponent() == nullptr, 1);

    // Space: inside a typed prefix it types; alone it chooses (JUCE's).
    chosen = -1;
    {
        juce::PopupMenu m;
        m.setLookAndFeel(look.get());
        m.addItem(1, "Big hall");
        m.addItem(2, "Big room");
        m.addItem(3, "Bright");
        m.showMenuAsync(juce::PopupMenu::Options()
                            .withTargetScreenArea(look->targetAreaFor({ 300, 300, 100, 24 }))
                            .withMaximumNumColumns(1),
                        [&chosen](int id) { chosen = id; });
    }
    juce::Component* second = juce::Component::getCurrentlyModalComponent();
    if (P.eq("space.menu_open", second != nullptr && second->getPeer() != nullptr, 1))
    {
        for (const char c : std::string("big r"))
            type(*second, static_cast<juce::juce_wchar>(c));
        P.eq("space.inside_prefix", highlighted(*second), 1);                    // "Big room"
        std::this_thread::sleep_for(std::chrono::milliseconds(MenuLook::kTypeAheadMs + 100));
        P.eq("space.alone_chooses", press(*second, juce::KeyPress(juce::KeyPress::spaceKey, {}, ' ')), 1);
        spin(0.3);
        P.eq("space.chosen", chosen, 2);
    }

    chosen = -1;
    {
        juce::PopupMenu m;
        m.setLookAndFeel(look.get());
        m.addItem(1, "One");
        m.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(look->targetAreaFor({ 300, 300, 100, 24 })),
                        [&chosen](int id) { chosen = id; });
    }
    if (juce::Component* third = juce::Component::getCurrentlyModalComponent())
    {
        P.eq("escape.taken_by_juce", press(*third, juce::KeyPress(juce::KeyPress::escapeKey)), 1);
        spin(0.3);
        P.eq("escape.dismissed_with_zero", chosen, 0);
    }
    juce::PopupMenu::dismissAllActiveMenus();
    spin(0.3);
    return P.finish();
}

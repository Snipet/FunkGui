#pragma once

// The theme on JUCE's popup menus (02 §5.8; HR PresetPanel.cpp:126-150). A menu is a separate native window — the one
// place a panel leaves the canvas — so it borrows the canvas's palette and type rather than looking like a different
// application: background ground, text ink100, headers ink52, the highlighted row ink16 with ink100 text, and the
// bundled face (text/BundledFont.h) at 14 px.
//
// A product sets it on the menus it shows itself (juce::PopupMenu::Options or setLookAndFeel) and calls setTheme() when
// UiPreferences::revision() moves. The host's own parameter menu (HostServices::showParamMenu) is the host's and keeps
// the host's look. Message thread. A menu still open when the editor closes holds a pointer to its look, so the owner
// calls juce::PopupMenu::dismissAllActiveMenus() before destroying one (HR ~PresetPanel).
//
// G6: HR's MenuLook, moved into FunkGui, constructed from a Theme; setTheme() is HR's (an addition to 02's one-line
// declaration). The typeface is made once per MenuLook from the embedded face; when it cannot be made the menu falls
// back to JUCE's default face at the same height.

#include <funkgui/core/Theme.h>

#include <juce_gui_basics/juce_gui_basics.h>

namespace funkgui
{
    class MenuLook : public juce::LookAndFeel_V4
    {
    public:
        explicit MenuLook(const Theme&);

        void setTheme(const Theme&);                     // the five popup-menu colours from the theme's tokens

        juce::Font getPopupMenuFont() override;          // the bundled face at kMenuPx

        static constexpr float kMenuPx = 14.0f;

    private:
        juce::Typeface::Ptr face_;
    };
}

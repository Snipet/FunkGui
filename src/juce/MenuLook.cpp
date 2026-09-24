#include <funkgui/juce/MenuLook.h>

#include <funkgui/text/BundledFont.h>

// MenuLook (02 §5.8): HR's popup-menu look (PresetPanel.cpp:126-150) in FunkGui. The rules are in the header.

namespace funkgui
{
    namespace
    {
        juce::Colour toJuce(Col c)
        {
            return juce::Colour(c.r, c.g, c.b, c.a);
        }
    }

    MenuLook::MenuLook(const Theme& th)
        : face_(juce::Typeface::createSystemTypefaceFor(BundledFont::data(), BundledFont::size()))
    {
        setTheme(th);
    }

    void MenuLook::setTheme(const Theme& th)
    {
        setColour(juce::PopupMenu::backgroundColourId,            toJuce(th.ground));
        setColour(juce::PopupMenu::textColourId,                  toJuce(th.ink100));
        setColour(juce::PopupMenu::headerTextColourId,            toJuce(th.ink52));
        setColour(juce::PopupMenu::highlightedBackgroundColourId, toJuce(th.ink16));
        setColour(juce::PopupMenu::highlightedTextColourId,       toJuce(th.ink100));
    }

    juce::Font MenuLook::getPopupMenuFont()
    {
        return face_ != nullptr ? juce::Font(juce::FontOptions().withTypeface(face_).withHeight(kMenuPx))
                                : juce::Font(juce::FontOptions(kMenuPx));
    }
}

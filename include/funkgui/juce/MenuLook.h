#pragma once

// The theme on JUCE's popup menus (02 §5.8; HR PresetPanel.cpp:126-150). A menu is a separate native window — the one
// place a panel leaves the canvas — so it borrows the canvas's palette and type rather than looking like a different
// application.
//
// A product sets it on the menus it shows itself (juce::PopupMenu::Options or setLookAndFeel) and calls setTheme() when
// UiPreferences::revision() moves. The host's own parameter menu (HostServices::showParamMenu) is the host's and keeps
// the host's look. Message thread. A menu still open when the editor closes holds a pointer to its look, so the owner
// calls juce::PopupMenu::dismissAllActiveMenus() before destroying one (HR ~PresetPanel).
//
// G6: HR's MenuLook, moved into FunkGui, constructed from a Theme; setTheme() is HR's (an addition to 02's one-line
// declaration). The typeface is made once per MenuLook from the embedded face; when it cannot be made the menu falls
// back to JUCE's default face at the same height.
//
// 0.25.0: MenuLook draws the whole menu in the canvas's style, where it used to colour LookAndFeel_V4's:
// - The panel: a rounded rectangle (kRadiusPx) of the theme's surface (Palette, mixed from the theme's tokens) with a
//   hairline border one device pixel wide (a hairline on Retina too) and a soft shadow, drawn into a margin of
//   kShadowPx round it. That needs a menu window that is not opaque. JUCE makes the window opaque when
//   PopupMenu::backgroundColourId is opaque or the platform has no transparent windows
//   (Desktop::canUseSemiTransparentWindows(): macOS and Windows have them, Linux under a compositing window manager),
//   so backgroundColourId is the surface at alpha 254. Where the window is opaque anyway (Linux without a compositor,
//   or a product that sets an opaque backgroundColourId) the panel is square and fills the window, with no margin
//   and the platform's own shadow (getMenuWindowFlags()); otherwise the window asks for no shadow of the platform's.
// - Rows (kRowPx) are inset kInsetPx from the panel's sides, the first kPadPx under its top and the last kPadPx over
//   its bottom; the row under the pointer or the keys is a pill (kPillRadiusPx) of Palette::hover. A tick, or a dot
//   for a radio item (radioItem()), sits in a column before the label; a submenu's chevron and a shortcut
//   (Item::shortcutKeyDescription, which JUCE also fills from a command manager) at the right. A disabled row is
//   Palette::disabled and takes no pill. A separator (kSeparatorPx) is a hairline; a section header
//   (PopupMenu::addSectionHeader, kHeaderPx) is its title in capitals, tracked and dim, over the labels' column.
// - Scrolling: with one column (Options::withMaximumNumColumns(1), as HostServicesJuce asks) a menu taller than the
//   display scrolls, under the panel's own scroll strips. JUCE takes no smaller bound, so the display's height is the
//   maximum. Rows that scroll into the shadow's margin are hidden until they come back. A row the keys move to, and
//   the initially selected row (Options::withInitiallySelectedItem with withItemThatMustBeVisible), is scrolled
//   between the strips (JUCE's own scrolling to it can leave it under the bottom one); like the keys, this needs the
//   rows' accessibility handlers.
// - Keys (preparePopupMenuWindow gives each menu window a KeyListener): Up and Down move, wrapping, and scroll the row
//   into view (JUCE's own do not scroll); Home, End, Page Up and Page Down; typing moves to the next row whose label
//   starts with what was typed, case aside (a pause of kTypeAheadMs starts a new prefix; the same letter again moves
//   to the next row with it; Space continues a typed prefix, and otherwise chooses, as JUCE's Space does). Return,
//   Space, Escape, Left and Right stay JUCE's. These keys work through JUCE's accessibility handlers of the rows,
//   which exist only while the menu is accessible (JUCE turns that off for a menu whose target component is not); there
//   the keys are JUCE's own and there is no type-ahead.
// - Both themes come from the same tokens. Retina: paths and text are drawn at the device's resolution by JUCE's
//   backing scale; the shadow is a blur at the logical size.
// The look is not the canvas's own drawing (no SDF, no FontService) but JUCE's 2-D renderer in the canvas's palette
// and face, so a menu matches the panel it opens from without the GPU.

#include <funkgui/core/Col.h>
#include <funkgui/core/Theme.h>
#include <funkgui/panel/HostServices.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace funkgui
{
    class MenuLook : public juce::LookAndFeel_V4
    {
    public:
        explicit MenuLook(const Theme&);
        ~MenuLook() override;

        void setTheme(const Theme&);                     // the palette, and the popup-menu colour ids from it

        juce::Font getPopupMenuFont() override;          // the bundled face at kMenuPx

        static constexpr float kMenuPx = 14.0f;

        // ---- 0.25.0 ---------------------------------------------------------------------------------------------------

        // The menu's colours, mixed from a theme's tokens (dark or light by its ground against ink100).
        struct Palette
        {
            Col surface;                                 // the panel
            Col edge;                                    // its hairline border
            Col hover;                                   // the pill of the row under the pointer or the keys
            Col text;                                    // a label (ink100)
            Col dim;                                     // shortcuts, chevrons, section headers (ink52)
            Col disabled;                                // a disabled row (ink32)
            Col rule;                                    // a separator
            Col mark;                                    // the tick and the radio dot (ink100)
            Col shadow;                                  // the shadow at its darkest (its alpha is its strength)
        };
        static Palette paletteFor(const Theme&);
        const Palette& palette() const noexcept { return palette_; }

        // The metrics, in the menu's logical px (JUCE scales a menu by its target component's scale and the display's).
        static constexpr int   kShadowPx = 8;            // the margin round the panel that the shadow is drawn into
        static constexpr int   kPadPx = 5;               // the panel's padding over the first row and under the last
        static constexpr float kRadiusPx = 8.0f;         // the panel's corners
        static constexpr int   kRowPx = 24;              // an item
        static constexpr int   kSeparatorPx = 9;
        static constexpr int   kHeaderPx = 24;           // a section header
        static constexpr int   kInsetPx = 5;             // a row's pill from the panel's sides
        static constexpr float kPillRadiusPx = 5.0f;
        static constexpr int   kMinPanelPx = 120;        // the narrowest panel
        static constexpr int   kGapPx = 4;               // between the anchor and the panel (targetAreaFor)
        static constexpr int   kTypeAheadMs = 1000;      // typing after a pause this long starts a new prefix

        // The margin round the panel of this look's menus: kShadowPx when the window can be transparent (above), else
        // 0. A menu window of w x h has its panel at (m, m, w - 2m, h - 2m).
        int shadowPx() const;

        // The target screen area to give JUCE (PopupMenu::Options::withTargetScreenArea) so that the panel, not the
        // window with its shadow's margin, opens beside `anchor` (screen px): the panel's left edge on the anchor's,
        // kGapPx under it, or over it when JUCE puts the menu there. `menuScale` is JUCE's scale for the menu
        // (Component::getApproximateScaleFactorForComponent of its target component), which the margin is multiplied
        // by. An anchor shorter than twice (margin - gap), such as a click's point, gets a gap of about the margin.
        juce::Rectangle<int> targetAreaFor(juce::Rectangle<int> anchor, float menuScale = 1.0f) const;

        // A radio item: one choice of a set, `chosen` drawn with a dot instead of a tick. The Item is ticked when
        // chosen and then carries a small dot Drawable as its image, so another LookAndFeel shows a dot too.
        static juce::PopupMenu::Item radioItem(int itemId, const juce::String& text, bool chosen, bool enabled = true);
        static bool isRadio(const juce::PopupMenu::Item&);

        // A request's rows (panel/HostServices.h) as a juce::PopupMenu: separators, section headers, submenus (JUCE's,
        // which return the chosen id from any level), radio items (radioItem) and shortcuts (the items' shortcut
        // text). No look is set on it, and the request's anchor, theme, selectedId and minWidth are the caller's to
        // apply (HostServicesJuce does: Options and setTheme). Labels are UTF-8.
        static juce::PopupMenu popupMenuFor(const MenuRequest&);

        // ---- PopupMenu::LookAndFeelMethods (0.25.0) ------------------------------------------------------------------
        void drawPopupMenuBackground(juce::Graphics&, int width, int height) override;
        void drawPopupMenuBackgroundWithOptions(juce::Graphics&, int width, int height,
                                                const juce::PopupMenu::Options&) override;
        void drawPopupMenuItem(juce::Graphics&, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                               bool isHighlighted, bool isTicked, bool hasSubMenu, const juce::String& text,
                               const juce::String& shortcutKeyText, const juce::Drawable* icon,
                               const juce::Colour* textColour) override;
        void drawPopupMenuItemWithOptions(juce::Graphics&, const juce::Rectangle<int>& area, bool isHighlighted,
                                          const juce::PopupMenu::Item&, const juce::PopupMenu::Options&) override;
        void drawPopupMenuSectionHeader(juce::Graphics&, const juce::Rectangle<int>& area,
                                        const juce::String& sectionName) override;
        void drawPopupMenuSectionHeaderWithOptions(juce::Graphics&, const juce::Rectangle<int>& area,
                                                   const juce::String& sectionName,
                                                   const juce::PopupMenu::Options&) override;
        void drawPopupMenuUpDownArrow(juce::Graphics&, int width, int height, bool isScrollUpArrow) override;
        void drawPopupMenuUpDownArrowWithOptions(juce::Graphics&, int width, int height, bool isScrollUpArrow,
                                                 const juce::PopupMenu::Options&) override;
        void getIdealPopupMenuItemSize(const juce::String& text, bool isSeparator, int standardMenuItemHeight,
                                       int& idealWidth, int& idealHeight) override;
        void getIdealPopupMenuItemSizeWithOptions(const juce::String& text, bool isSeparator,
                                                  int standardMenuItemHeight, int& idealWidth, int& idealHeight,
                                                  const juce::PopupMenu::Options&) override;
        int  getPopupMenuBorderSize() override;
        int  getPopupMenuBorderSizeWithOptions(const juce::PopupMenu::Options&) override;
        int  getMenuWindowFlags() override;
        void preparePopupMenuWindow(juce::Component& newWindow) override;
        // A menu shown inside a parent component (Options::withParentComponent) gets JUCE's resizable frame over it;
        // the panel has its own edge, so this look draws none.
        void drawResizableFrame(juce::Graphics&, int w, int h, const juce::BorderSize<int>&) override;

    private:
        struct Keys;                                     // every menu window's KeyListener (MenuLook.cpp)
        struct Rows;                                     // hides the rows in the shadow's margin (MenuLook.cpp)

        void drawRow(juce::Graphics&, const juce::Rectangle<int>& area, bool separator, bool enabled, bool highlighted,
                     bool ticked, bool radio, bool hasSubMenu, const juce::String& text, const juce::String& shortcut,
                     const juce::Drawable* icon, const juce::Colour* textColour);
        juce::Font headerFont();

        juce::Typeface::Ptr face_;
        Palette palette_{};
        std::unique_ptr<Keys> keys_;
        std::unique_ptr<Rows> rows_;
    };
}

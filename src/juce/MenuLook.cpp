#include <funkgui/juce/MenuLook.h>

#include "MenuKeys.h"

#include <funkgui/text/BundledFont.h>

#include <cmath>
#include <utility>
#include <vector>

// MenuLook (02 §5.8): HR's popup-menu look (PresetPanel.cpp:126-150) in FunkGui, drawn in the canvas's style since
// 0.25.0. The rules are in the header. Choices where it is silent:
//
// - A row's layout, from its pill's left edge: the mark's column at 8..20, the label at 26; at the right, the chevron
//   ends 10 px in and the shortcut ends there too, or 6 px before the chevron. A row is measured as JUCE hands it over
//   (the label, three spaces and the shortcut), so the spaces are the gap between the two.
// - JUCE asks for a section header's size through getIdealPopupMenuItemSize with a standard height of -1 and then adds
//   half the height and a quarter of the width (HeaderItemComponent); the size returned for -1 is what makes those
//   come to kHeaderPx and the title's width. Nothing else passes -1.
// - The header's component is the row's less the border at each side (ItemComponent::resized), so its title starts at
//   kInsetPx + 26 - kPadPx from it, over the labels.
// - The shadow: one blur of the panel's shape, 7 px, 2 px down, in Palette::shadow, under the panel's fill. The
//   margin is 8 px, so its last pixel under the panel is clipped where its alpha is all but gone.
// - A radio item's dot Drawable is a 6 px disc in mid grey (seen on both a light and a dark menu of another look), named
//   by its component ID, which Drawable copies with the Item.
// - The keys work on the menu window's children, which are its rows in order (MenuWindow adds nothing else). A row is
//   selectable when its accessibility handler can be pressed (JUCE gives that action to a row that can be chosen and
//   to one that opens a submenu); the current row is the one whose handler has the accessibility focus, which JUCE
//   gives a row as it highlights it; a move is the handler's focus action, which is JUCE's own way to highlight a
//   row and scroll it into view. Page Up and Page Down move by the rows that fit in the window, less one.
// - The rows in the shadow's margin: a row is hidden while any part of it lies in the window's top or bottom
//   kShadowPx (only a menu that scrolls has such rows), checked whenever a row or the window moves or resizes. The
//   scroll strips cover the panel's top and bottom 24 px less the margin, so a hidden row is under a strip anyway
//   save for up to kRowPx - 16 px of it. A hidden row takes no pointer (JUCE finds rows by getComponentAt).
// - Scrolling to a row: JUCE's ensureItemComponentIsVisible moves the window as well as the rows where the display
//   has room, and counts the window's move against the rows', so the row lands that much off where it was meant to
//   (into a strip, or the margin, where it would be hidden). The keys and the initially selected row are scrolled
//   between the strips first, by JUCE's wheel scrolling, which moves only the rows; JUCE's own step then finds the row
//   in view and only highlights it. This needs the rows' accessibility handlers, as the keys do.

namespace funkgui
{
    namespace
    {
        juce::Colour toJuce(Col c)
        {
            return juce::Colour(c.r, c.g, c.b, c.a);
        }

        float luma(Col c)
        {
            return 0.2126f * static_cast<float>(c.r) + 0.7152f * static_cast<float>(c.g)
                 + 0.0722f * static_cast<float>(c.b);
        }

        constexpr const char* kRadioId = "funkgui.menu.radio";
        constexpr const char* kMarginProperty = "funkgui.menu.margin";

        // A row's layout from its pill's left and right edges (the comment above).
        constexpr float kMarkLeft = 8.0f, kMarkW = 12.0f, kTextLeft = 26.0f;
        constexpr float kRightPad = 10.0f, kChevronW = 5.0f, kChevronGap = 6.0f, kShortcutGap = 12.0f;
        constexpr float kHeaderTextH = 11.5f;            // a section header's font height
        constexpr float kHeaderKerning = 0.08f;

        // One device pixel in logical px (a hairline).
        float devicePx(juce::Graphics& g)
        {
            const float s = g.getInternalContext().getPhysicalPixelScaleFactor();
            return s > 0.0f ? 1.0f / s : 1.0f;
        }

        // A horizontal hairline from x0 to x1 on the device pixel at or above y.
        void hairline(juce::Graphics& g, float x0, float x1, float y, juce::Colour c)
        {
            const float px = devicePx(g);
            g.setColour(c);
            g.fillRect(juce::Rectangle<float>(x0, std::floor(y / px) * px, x1 - x0, px));
        }

        void tick(juce::Graphics& g, float cx, float cy, juce::Colour c)
        {
            juce::Path p;
            p.startNewSubPath(cx - 4.0f, cy + 0.2f);
            p.lineTo(cx - 1.3f, cy + 2.9f);
            p.lineTo(cx + 4.0f, cy - 3.1f);
            g.setColour(c);
            g.strokePath(p, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // A chevron of half-width w and depth d at (cx, cy), pointing right (dir 0), up (1) or down (2).
        void chevron(juce::Graphics& g, float cx, float cy, float w, float d, int dir, juce::Colour c)
        {
            juce::Path p;
            if (dir == 0)
            {
                p.startNewSubPath(cx - d * 0.5f, cy - w);
                p.lineTo(cx + d * 0.5f, cy);
                p.lineTo(cx - d * 0.5f, cy + w);
            }
            else
            {
                const float s = dir == 1 ? -1.0f : 1.0f;
                p.startNewSubPath(cx - w, cy - s * d * 0.5f);
                p.lineTo(cx, cy + s * d * 0.5f);
                p.lineTo(cx + w, cy - s * d * 0.5f);
            }
            g.setColour(c);
            g.strokePath(p, juce::PathStrokeType(1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        int textWidth(const juce::Font& f, const juce::String& s)
        {
            return juce::GlyphArrangement::getStringWidthInt(f, s);
        }

        constexpr int kScrollZonePx = 24;                // JUCE's PopupMenuSettings::scrollZone: the strips' height

        // Scrolls a menu window's rows so that `row` lies between the scroll strips, by JUCE's own wheel scrolling
        // (MenuWindow::mouseWheelMove scrolls by -10 x deltaY x the strip's height), which moves the rows and never the
        // window. Nothing for a menu whose rows all fit (none lies outside the window's border).
        void scrollIntoView(juce::Component& window, const juce::Component& row, int border)
        {
            bool scrolls = false;
            for (const juce::Component* r : window.getChildren())
                scrolls = scrolls || r->getY() < border || r->getBottom() > window.getHeight() - border;
            const int top = kScrollZonePx, bottom = window.getHeight() - kScrollZonePx;
            if (!scrolls || bottom - top < row.getHeight())
                return;
            const int delta = row.getBottom() > bottom ? row.getBottom() - bottom
                            : row.getY() < top         ? row.getY() - top
                                                       : 0;
            if (delta == 0)
                return;
            juce::MouseWheelDetails wheel{};
            wheel.deltaY = -static_cast<float>(delta) / (10.0f * static_cast<float>(kScrollZonePx));
            const juce::Time now = juce::Time::getCurrentTime();
            const juce::MouseEvent e(juce::Desktop::getInstance().getMainMouseSource(), {}, {}, 0.0f, 0.0f, 0.0f, 0.0f,
                                     0.0f, &window, &window, now, {}, now, 0, false);
            window.mouseWheelMove(e, wheel);
        }
    }

    // ---- The keys -----------------------------------------------------------------------------------------------------

    struct MenuLook::Keys final : juce::KeyListener
    {
        explicit Keys(MenuLook& look) : look_(look) {}

        juce::String typed;
        juce::uint32 lastTyped = 0;
        juce::Component::SafePointer<juce::Component> typedIn;

        bool keyPressed(const juce::KeyPress& key, juce::Component* window) override
        {
            if (window == nullptr)
                return false;
            std::vector<juce::AccessibilityHandler*> handlers;
            std::vector<bool> selectable;
            juce::StringArray labels;
            int current = -1;
            bool any = false;
            for (juce::Component* row : window->getChildren())
            {
                juce::AccessibilityHandler* h = row->getAccessibilityHandler();
                const bool can = h != nullptr && h->getActions().contains(juce::AccessibilityActionType::press);
                if (h != nullptr && h->hasFocus(false))
                    current = static_cast<int>(handlers.size());
                handlers.push_back(h);
                selectable.push_back(can);
                labels.add(h != nullptr ? h->getTitle() : juce::String());
                any = any || can;
            }
            if (!any)
                return false;                            // no handlers (an inaccessible menu): JUCE's own keys

            const int code = key.getKeyCode();
            const juce::ModifierKeys mods = key.getModifiers();
            const juce::juce_wchar ch = key.getTextCharacter();
            const juce::uint32 now = juce::Time::getMillisecondCounter();
            const bool typing = typed.isNotEmpty() && typedIn.getComponent() == window
                                && now - lastTyped < static_cast<juce::uint32>(kTypeAheadMs);
            int to = -1;
            if (code == juce::KeyPress::downKey)
                to = menukeys::step(selectable, current, 1);
            else if (code == juce::KeyPress::upKey)
                to = menukeys::step(selectable, current, -1);
            else if (code == juce::KeyPress::homeKey)
                to = menukeys::edge(selectable, false);
            else if (code == juce::KeyPress::endKey)
                to = menukeys::edge(selectable, true);
            else if (code == juce::KeyPress::pageDownKey || code == juce::KeyPress::pageUpKey)
            {
                const int rows = juce::jmax(1, window->getHeight() / kRowPx - 1);
                to = menukeys::page(selectable, current, rows, code == juce::KeyPress::pageDownKey ? 1 : -1);
            }
            else if (ch >= ' ' && ch != 0x7f && !mods.isCommandDown() && !mods.isCtrlDown() && !mods.isAltDown()
                     && (ch != ' ' || typing))
            {
                if (!typing)
                    typed.clear();
                typed += juce::String::charToString(ch);
                lastTyped = now;
                typedIn = window;
                to = menukeys::typeAhead(labels, selectable, current, typed);
                if (to < 0)
                    return true;                         // no row matches: the key is the menu's all the same
            }
            else
                return false;                            // Return, Space, Escape, Left, Right: JUCE's

            if (to >= 0 && to != current && handlers[static_cast<size_t>(to)] != nullptr)
            {
                // Between the strips first: JUCE's own scrolling to the row (in the focus action) also moves the
                // window where it can, and then leaves the row off by that much.
                scrollIntoView(*window, *window->getChildComponent(to), look_.getPopupMenuBorderSize());
                handlers[static_cast<size_t>(to)]->getActions().invoke(juce::AccessibilityActionType::focus);
            }
            return true;
        }

    private:
        MenuLook& look_;
    };

    // ---- The rows in the shadow's margin --------------------------------------------------------------------------------

    struct MenuLook::Rows final : juce::ComponentListener
    {
        static void apply(juce::Component& window, juce::Component& row, int margin)
        {
            const bool inside = row.getY() >= margin && row.getBottom() <= window.getHeight() - margin;
            if (row.isVisible() != inside)
                row.setVisible(inside);
        }

        static void check(juce::Component& window)
        {
            const int margin = static_cast<int>(window.getProperties()[kMarginProperty]);
            for (juce::Component* row : window.getChildren())
                apply(window, *row, margin);
        }

        void componentMovedOrResized(juce::Component& c, bool, bool) override
        {
            if (c.getProperties().contains(kMarginProperty))
                check(c);                                // the window
            else if (juce::Component* window = c.getParentComponent())
                apply(*window, c, static_cast<int>(window->getProperties()[kMarginProperty]));
        }
    };

    // ---- MenuLook -------------------------------------------------------------------------------------------------------

    MenuLook::MenuLook(const Theme& th)
        : face_(juce::Typeface::createSystemTypefaceFor(BundledFont::data(), BundledFont::size())),
          keys_(std::make_unique<Keys>(*this)), rows_(std::make_unique<Rows>())
    {
        setTheme(th);
    }

    // A dismissed menu's window and rows outlive the dismissal (JUCE deletes them from the message loop, or at its
    // shutdown) and tell their listeners as they move and as they go: take this look's off them first.
    MenuLook::~MenuLook()
    {
        for (const auto& c : attached_)
            if (juce::Component* component = c.getComponent())
            {
                component->removeKeyListener(keys_.get());
                component->removeComponentListener(rows_.get());
            }
    }

    MenuLook::Palette MenuLook::paletteFor(const Theme& th)
    {
        constexpr Col white{ 255, 255, 255, 255 }, black{ 0, 0, 0, 255 };
        const bool dark = luma(th.ground) < luma(th.ink100);
        Palette p;
        p.surface = dark ? mix(th.ground, th.ink100, 0.05f) : mix(th.ground, white, 0.72f);
        p.edge = dark ? mix(th.ground, th.ink100, 0.16f) : mix(th.ground, th.ink100, 0.15f);
        p.hover = dark ? mix(th.ground, th.ink100, 0.14f) : mix(p.surface, th.ink100, 0.08f);
        p.text = th.ink100;
        p.dim = th.ink52;
        p.disabled = th.ink32;
        p.rule = dark ? mix(th.ground, th.ink100, 0.12f) : mix(p.surface, th.ink100, 0.11f);
        p.mark = th.ink100;
        p.shadow = black.withAlpha(dark ? 0.5f : 0.2f);
        return p;
    }

    void MenuLook::setTheme(const Theme& th)
    {
        // Asked here, not at every draw: on Linux it is a round trip to the X server. A product calls setTheme before
        // each menu it shows (HostServicesJuce does), so the answer is as fresh as JUCE's own for the window.
        transparentWindows_ = juce::Desktop::canUseSemiTransparentWindows();
        palette_ = paletteFor(th);
        // Not opaque, so the window is not (the header); the panel is drawn opaque in this colour.
        setColour(juce::PopupMenu::backgroundColourId,            toJuce(palette_.surface).withAlpha(juce::uint8{ 254 }));
        setColour(juce::PopupMenu::textColourId,                  toJuce(palette_.text));
        setColour(juce::PopupMenu::headerTextColourId,            toJuce(palette_.dim));
        setColour(juce::PopupMenu::highlightedBackgroundColourId, toJuce(palette_.hover));
        setColour(juce::PopupMenu::highlightedTextColourId,       toJuce(palette_.text));
    }

    juce::Font MenuLook::getPopupMenuFont()
    {
        return face_ != nullptr ? juce::Font(juce::FontOptions().withTypeface(face_).withHeight(kMenuPx))
                                : juce::Font(juce::FontOptions(kMenuPx));
    }

    juce::Font MenuLook::headerFont()
    {
        return getPopupMenuFont().withHeight(kHeaderTextH).withExtraKerningFactor(kHeaderKerning);
    }

    int MenuLook::shadowPx() const
    {
        return transparentWindows_ && !findColour(juce::PopupMenu::backgroundColourId).isOpaque() ? kShadowPx : 0;
    }

    juce::Rectangle<int> MenuLook::targetAreaFor(juce::Rectangle<int> anchor, float menuScale) const
    {
        const float s = menuScale > 0.0f ? menuScale : 1.0f;
        const int m = juce::roundToInt(static_cast<float>(shadowPx()) * s);
        const int gap = juce::roundToInt(static_cast<float>(kGapPx) * s);
        // JUCE puts the window's top on the area's bottom (or its bottom on the area's top): pulling both edges in by
        // the margin less the gap leaves the gap between the anchor and the panel.
        const int d = m - gap;
        juce::Rectangle<int> r(anchor.getX() - m, anchor.getY(), anchor.getWidth() + 2 * m, anchor.getHeight());
        if (d <= 0)
            return r.expanded(0, -d);
        if (anchor.getHeight() >= 2 * d)
            return r.reduced(0, d);
        return r.withY(anchor.getY() + anchor.getHeight() / 2).withHeight(1);
    }

    juce::PopupMenu::Item MenuLook::radioItem(int itemId, const juce::String& text, bool chosen, bool enabled)
    {
        juce::PopupMenu::Item item(text);
        item.itemID = itemId;
        item.isEnabled = enabled;
        item.isTicked = chosen;
        if (chosen)
        {
            auto dot = std::make_unique<juce::DrawablePath>();
            juce::Path p;
            p.addEllipse(0.0f, 0.0f, 6.0f, 6.0f);
            dot->setPath(p);
            dot->setFill(juce::Colour(0xff808080));
            dot->setComponentID(kRadioId);
            item.image = std::move(dot);
        }
        return item;
    }

    bool MenuLook::isRadio(const juce::PopupMenu::Item& item)
    {
        return item.image != nullptr && item.image->getComponentID() == kRadioId;
    }

    namespace
    {
        juce::String fromUtf8(const std::string& s)
        {
            return juce::String::fromUTF8(s.data(), static_cast<int>(s.size()));
        }

        void addRows(juce::PopupMenu& m, const std::vector<MenuItem>& items)
        {
            for (const MenuItem& it : items)
            {
                if (it.separator)
                    m.addSeparator();
                else if (it.header)
                    m.addSectionHeader(fromUtf8(it.label));
                else if (!it.submenu.empty())
                {
                    juce::PopupMenu sub;
                    addRows(sub, it.submenu);
                    m.addSubMenu(fromUtf8(it.label), sub, it.enabled);
                }
                else
                {
                    juce::PopupMenu::Item item = it.radio ? MenuLook::radioItem(it.id, fromUtf8(it.label), it.checked,
                                                                                it.enabled)
                                                          : juce::PopupMenu::Item(fromUtf8(it.label));
                    item.itemID = it.id;
                    item.isEnabled = it.enabled;
                    item.isTicked = it.checked;
                    item.shortcutKeyDescription = fromUtf8(it.shortcut);
                    m.addItem(std::move(item));
                }
            }
        }
    }

    juce::PopupMenu MenuLook::popupMenuFor(const MenuRequest& request)
    {
        juce::PopupMenu m;
        addRows(m, request.items);
        return m;
    }

    // ---- The panel ------------------------------------------------------------------------------------------------------

    void MenuLook::drawPopupMenuBackground(juce::Graphics& g, int width, int height)
    {
        const int m = shadowPx();
        const float px = devicePx(g);
        const float w = static_cast<float>(width), h = static_cast<float>(height);
        if (m == 0)                                      // an opaque window: the panel fills it, square
        {
            g.fillAll(toJuce(palette_.surface));
            g.setColour(toJuce(palette_.edge));
            g.drawRect(juce::Rectangle<float>(0.0f, 0.0f, w, h), px);
            return;
        }
        const float fm = static_cast<float>(m);
        const juce::Rectangle<float> panel(fm, fm, w - 2.0f * fm, h - 2.0f * fm);
        juce::Path shape;
        shape.addRoundedRectangle(panel, kRadiusPx);
        juce::DropShadow(toJuce(palette_.shadow), 7, { 0, 2 }).drawForPath(g, shape);
        g.setColour(toJuce(palette_.surface));
        g.fillPath(shape);
        juce::Path edge;
        edge.addRoundedRectangle(panel.reduced(px * 0.5f), kRadiusPx - px * 0.5f);
        g.setColour(toJuce(palette_.edge));
        g.strokePath(edge, juce::PathStrokeType(px));
    }

    void MenuLook::drawPopupMenuBackgroundWithOptions(juce::Graphics& g, int width, int height,
                                                      const juce::PopupMenu::Options&)
    {
        drawPopupMenuBackground(g, width, height);
    }

    void MenuLook::drawResizableFrame(juce::Graphics&, int, int, const juce::BorderSize<int>&) {}

    int MenuLook::getPopupMenuBorderSize()
    {
        return shadowPx() + kPadPx;
    }

    int MenuLook::getPopupMenuBorderSizeWithOptions(const juce::PopupMenu::Options&)
    {
        return getPopupMenuBorderSize();
    }

    int MenuLook::getMenuWindowFlags()
    {
        return shadowPx() > 0 ? 0 : juce::ComponentPeer::windowHasDropShadow;
    }

    void MenuLook::preparePopupMenuWindow(juce::Component& window)
    {
        std::erase_if(attached_, [](const auto& c) { return c.getComponent() == nullptr; });   // deleted by JUCE
        window.addKeyListener(keys_.get());
        attached_.emplace_back(&window);
        const int m = shadowPx();
        if (m > 0)
        {
            window.getProperties().set(kMarginProperty, m);
            window.addComponentListener(rows_.get());
            for (juce::Component* row : window.getChildren())
            {
                row->addComponentListener(rows_.get());
                attached_.emplace_back(row);
            }
            Rows::check(window);
        }
        // The initially selected row (Options::withInitiallySelectedItem), which JUCE highlights, and so focuses, as it
        // makes the rows: JUCE's scrolling to it (withItemThatMustBeVisible) moves the window and leaves the row that
        // much lower, under the bottom strip; between the strips instead.
        for (juce::Component* row : window.getChildren())
        {
            const juce::AccessibilityHandler* h = row->getAccessibilityHandler();
            if (h != nullptr && h->hasFocus(false))
            {
                scrollIntoView(window, *row, getPopupMenuBorderSize());
                break;
            }
        }
    }

    // ---- Rows ---------------------------------------------------------------------------------------------------------

    void MenuLook::getIdealPopupMenuItemSize(const juce::String& text, bool isSeparator, int standardMenuItemHeight,
                                             int& idealWidth, int& idealHeight)
    {
        if (isSeparator)
        {
            idealWidth = kMinPanelPx - 2 * kPadPx;
            idealHeight = kSeparatorPx;
            return;
        }
        if (standardMenuItemHeight == -1)                // a section header (the comment at the top)
        {
            const int need = textWidth(headerFont(), text.toUpperCase())
                           + static_cast<int>(kTextLeft) + kInsetPx - kPadPx + kInsetPx + 4;
            int w = (need * 4) / 5;
            while (w + w / 4 < need)
                ++w;
            idealWidth = w;
            idealHeight = (kHeaderPx * 2) / 3;           // + half of it = kHeaderPx
            return;
        }
        const int left = kInsetPx + static_cast<int>(kTextLeft);
        const int right = static_cast<int>(kRightPad + kChevronW + kChevronGap) + kInsetPx;
        idealWidth = juce::jmax(kMinPanelPx - 2 * kPadPx, textWidth(getPopupMenuFont(), text) + left + right - 2 * kPadPx);
        idealHeight = standardMenuItemHeight > 0 ? standardMenuItemHeight : kRowPx;
    }

    void MenuLook::getIdealPopupMenuItemSizeWithOptions(const juce::String& text, bool isSeparator,
                                                        int standardMenuItemHeight, int& idealWidth, int& idealHeight,
                                                        const juce::PopupMenu::Options&)
    {
        getIdealPopupMenuItemSize(text, isSeparator, standardMenuItemHeight, idealWidth, idealHeight);
    }

    void MenuLook::drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator,
                                     bool isActive, bool isHighlighted, bool isTicked, bool hasSubMenu,
                                     const juce::String& text, const juce::String& shortcutKeyText,
                                     const juce::Drawable* icon, const juce::Colour* textColour)
    {
        const bool radio = icon != nullptr && icon->getComponentID() == kRadioId;
        drawRow(g, area, isSeparator, isActive, isHighlighted, isTicked, radio, hasSubMenu, text, shortcutKeyText,
                radio ? nullptr : icon, textColour);
    }

    void MenuLook::drawPopupMenuItemWithOptions(juce::Graphics& g, const juce::Rectangle<int>& area,
                                                bool isHighlighted, const juce::PopupMenu::Item& item,
                                                const juce::PopupMenu::Options&)
    {
        const juce::Colour* colour = item.colour != juce::Colour() ? &item.colour : nullptr;
        const bool hasSubMenu = item.subMenu != nullptr && (item.itemID == 0 || item.subMenu->getNumItems() > 0);
        const bool radio = isRadio(item);
        drawRow(g, area, item.isSeparator, item.isEnabled, isHighlighted, item.isTicked, radio, hasSubMenu, item.text,
                item.shortcutKeyDescription, radio ? nullptr : item.image.get(), colour);
    }

    void MenuLook::drawRow(juce::Graphics& g, const juce::Rectangle<int>& area, bool separator, bool enabled,
                           bool highlighted, bool ticked, bool radio, bool hasSubMenu, const juce::String& text,
                           const juce::String& shortcut, const juce::Drawable* icon, const juce::Colour* textColour)
    {
        const float m = static_cast<float>(shadowPx());
        const juce::Rectangle<float> row = area.toFloat();
        const float left = row.getX() + m + static_cast<float>(kInsetPx);
        const float right = row.getRight() - m - static_cast<float>(kInsetPx);
        if (separator)
        {
            hairline(g, left + 4.0f, right - 4.0f, row.getCentreY(), toJuce(palette_.rule));
            return;
        }
        const juce::Rectangle<float> pill(left, row.getY() + 1.0f, right - left, row.getHeight() - 2.0f);
        if (highlighted && enabled)
        {
            g.setColour(toJuce(palette_.hover));
            g.fillRoundedRectangle(pill, kPillRadiusPx);
        }
        const juce::Colour ink = !enabled ? toJuce(palette_.disabled)
                                 : textColour != nullptr ? *textColour
                                                         : toJuce(palette_.text);
        const juce::Colour dim = enabled ? toJuce(palette_.dim) : toJuce(palette_.disabled);
        const float cy = pill.getCentreY();
        const float markX = left + kMarkLeft + kMarkW * 0.5f;
        if (ticked)
        {
            const juce::Colour mark = enabled ? toJuce(palette_.mark) : toJuce(palette_.disabled);
            if (radio)
            {
                g.setColour(mark);
                g.fillEllipse(markX - 3.0f, cy - 3.0f, 6.0f, 6.0f);
            }
            else
                tick(g, markX, cy, mark);
        }
        else if (icon != nullptr)
            icon->drawWithin(g, juce::Rectangle<float>(left + kMarkLeft - 2.0f, cy - 7.0f, kMarkW + 4.0f, 14.0f),
                             juce::RectanglePlacement::centred | juce::RectanglePlacement::onlyReduceInSize,
                             enabled ? 1.0f : 0.5f);

        float textRight = right - kRightPad;
        if (hasSubMenu)
        {
            chevron(g, textRight - kChevronW * 0.5f, cy, 3.5f, kChevronW, 0, highlighted && enabled ? ink : dim);
            textRight -= kChevronW + kChevronGap;
        }
        const juce::Font font = getPopupMenuFont();
        g.setFont(font);
        if (shortcut.isNotEmpty())
        {
            const float sw = static_cast<float>(textWidth(font, shortcut));
            g.setColour(dim);
            g.drawText(shortcut, juce::Rectangle<float>(textRight - sw, row.getY(), sw + 1.0f, row.getHeight()),
                       juce::Justification::centredRight, false);
            textRight -= sw + kShortcutGap;
        }
        const float textX = left + kTextLeft;
        g.setColour(ink);
        g.drawText(text, juce::Rectangle<float>(textX, row.getY(), juce::jmax(0.0f, textRight - textX), row.getHeight()),
                   juce::Justification::centredLeft, true);
    }

    void MenuLook::drawPopupMenuSectionHeader(juce::Graphics& g, const juce::Rectangle<int>& area,
                                              const juce::String& sectionName)
    {
        const float x = static_cast<float>(area.getX() + kInsetPx - kPadPx) + kTextLeft;
        const juce::Rectangle<float> box(x, static_cast<float>(area.getY() + 6),
                                         juce::jmax(0.0f, static_cast<float>(area.getRight()) - x - 4.0f),
                                         static_cast<float>(area.getHeight() - 8));
        g.setFont(headerFont());
        g.setColour(toJuce(palette_.dim));
        g.drawText(sectionName.toUpperCase(), box, juce::Justification::centredLeft, true);
    }

    void MenuLook::drawPopupMenuSectionHeaderWithOptions(juce::Graphics& g, const juce::Rectangle<int>& area,
                                                         const juce::String& sectionName,
                                                         const juce::PopupMenu::Options&)
    {
        drawPopupMenuSectionHeader(g, area, sectionName);
    }

    // ---- Scrolling --------------------------------------------------------------------------------------------------------

    void MenuLook::drawPopupMenuUpDownArrow(juce::Graphics& g, int width, int height, bool isScrollUpArrow)
    {
        const float m = static_cast<float>(shadowPx());
        const float px = devicePx(g);
        const float w = static_cast<float>(width), h = static_cast<float>(height);
        // The zone is the window's top (or bottom) `height` px; the panel's part of it is inside the margin and the
        // edge. The bottom zone's origin is the window's height less `height` (MenuWindow::paintOverChildren).
        constexpr float kTall = 4096.0f;
        const juce::Rectangle<float> panel = isScrollUpArrow
                                                 ? juce::Rectangle<float>(m, m, w - 2.0f * m, kTall)
                                                 : juce::Rectangle<float>(m, h - m - kTall, w - 2.0f * m, kTall);
        juce::Path inside;
        inside.addRoundedRectangle(panel.reduced(px), juce::jmax(0.0f, kRadiusPx - px));
        const juce::Rectangle<float> strip = isScrollUpArrow ? juce::Rectangle<float>(m, m, w - 2.0f * m, h - m)
                                                             : juce::Rectangle<float>(m, 0.0f, w - 2.0f * m, h - m);
        const juce::Colour surface = toJuce(palette_.surface);
        juce::Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(inside);
        g.setColour(surface);
        g.fillRect(strip);
        constexpr float kFade = 6.0f;                    // the rows fade in beside the strip
        const float edgeY = isScrollUpArrow ? h : 0.0f;
        const float farY = isScrollUpArrow ? h + kFade : -kFade;
        g.setGradientFill(juce::ColourGradient(surface, 0.0f, edgeY, surface.withAlpha(0.0f), 0.0f, farY, false));
        g.fillRect(juce::Rectangle<float>(m, juce::jmin(edgeY, farY), w - 2.0f * m, kFade));
        const float cy = isScrollUpArrow ? (m + h) * 0.5f : (h - m) * 0.5f;
        chevron(g, w * 0.5f, cy, 4.0f, 2.5f, isScrollUpArrow ? 1 : 2, toJuce(palette_.dim));
    }

    void MenuLook::drawPopupMenuUpDownArrowWithOptions(juce::Graphics& g, int width, int height, bool isScrollUpArrow,
                                                       const juce::PopupMenu::Options&)
    {
        drawPopupMenuUpDownArrow(g, width, height, isScrollUpArrow);
    }
}

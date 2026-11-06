// FUNKGUI_TEST name=fg.menulook timeout=180 gpu=0
//
// fg.menulook: MenuLook, JUCE's popup menus drawn in the canvas's style (juce/MenuLook.h, 0.25.0), and pictures of
// every kind of row in both themes.
// - palette.*, colours.*, font.*: the palette from each theme's tokens (the labels' inks, the surface a shade off the
//   ground, the hover pill, edge and rule apart from the surface, the shadow's strength), the popup-menu colour ids
//   from it (the background not opaque, so the window is not), setTheme(), the bundled face at 14 px.
// - metrics.*: the margin (kShadowPx where the platform has transparent windows, else 0), the border JUCE lays rows
//   out from, the window flags (no platform shadow under the look's own), the rows' ideal sizes (a section header's
//   after JUCE's own additions).
// - target.*: targetAreaFor, which puts the panel and not the window's margin against the anchor.
// - radio.*: radioItem and isRadio, through a copy of the Item.
// - request.*: popupMenuFor, a HostServices request as JUCE's rows.
// - keys.*: MenuKeys (src/juce), where Up, Down, Home, End, Page Up and Page Down and type-ahead move.
// - menu.<kind>.<theme>.*: a real juce::PopupMenu in this look, shown inside a canvas component that is on no desktop
//   (JUCE lays it out and paints it as it does a window: it needs a display and no window), snapshotted at a scale of 2
//   (Retina). The rows check its window against the anchor (the panel's left edge on the anchor's, kGapPx under it),
//   its rows' heights, a scrolled menu's hidden rows, and pixels of the picture: the corner shows the canvas, the
//   padding the surface, the highlighted row the hover pill, a separator the rule, the radio dot and the tick the
//   mark, a chevron and the scroll strips' chevrons the dim ink, a disabled row less contrast than an enabled one,
//   and the shadow under the panel darker than the canvas.
// - png.*: each picture as menulook-<kind>-<theme>.png and all of them as menulook-sheet.png (graphite above, paper
//   below), in the test's sandbox.
// The keys on a real window, the scrolling with them, type-ahead through JUCE's handlers and the initially selected row
// of a long menu are fg.menulook.live (a window server). Spec rows only.

#include <funkgui/core/Col.h>
#include <funkgui/core/Theme.h>
#include <funkgui/juce/MenuLook.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/test/Harness.h>

#include "../../src/juce/MenuKeys.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace T = funkgui::test;
using funkgui::MenuLook;

namespace
{
    juce::Colour toJuce(funkgui::Col c) { return juce::Colour(c.r, c.g, c.b, c.a); }
    int64_t argb(juce::Colour c) { return static_cast<int64_t>(c.getARGB()); }
    int64_t argb(funkgui::Col c) { return argb(toJuce(c)); }

    float luma(juce::Colour c)
    {
        return 0.2126f * static_cast<float>(c.getRed()) + 0.7152f * static_cast<float>(c.getGreen())
             + 0.0722f * static_cast<float>(c.getBlue());
    }
    float luma(funkgui::Col c) { return luma(toJuce(c)); }

    int channelDiff(juce::Colour a, juce::Colour b)
    {
        return juce::jmax(std::abs(a.getRed() - b.getRed()), std::abs(a.getGreen() - b.getGreen()),
                          std::abs(a.getBlue() - b.getBlue()));
    }

    // The canvas a menu opens over: the theme's ground, as the editor's.
    struct Canvas final : juce::Component
    {
        juce::Colour ground;
        void paint(juce::Graphics& g) override { g.fillAll(ground); }
    };

    constexpr float kSnap = 2.0f;                        // the snapshot's scale: a Retina display's

    // A picture's pixel at logical (x, y): the device pixel at the centre of that logical one.
    juce::Colour at(const juce::Image& img, float x, float y)
    {
        return img.getPixelAt(static_cast<int>(x * kSnap + 0.5f), static_cast<int>(y * kSnap + 0.5f));
    }

    // The largest difference from `from` over a logical box of the picture.
    int contrastIn(const juce::Image& img, juce::Rectangle<float> box, juce::Colour from)
    {
        int best = 0;
        for (int y = static_cast<int>(box.getY() * kSnap); y < static_cast<int>(box.getBottom() * kSnap); ++y)
            for (int x = static_cast<int>(box.getX() * kSnap); x < static_cast<int>(box.getRight() * kSnap); ++x)
                best = juce::jmax(best, channelDiff(img.getPixelAt(x, y), from));
        return best;
    }

    bool writePng(const juce::Image& img, const juce::String& name)
    {
        const juce::File f = juce::File::getCurrentWorkingDirectory().getChildFile(name);
        f.deleteFile();
        juce::FileOutputStream out(f);
        if (!out.openedOk())
            return false;
        juce::PNGImageFormat png;
        const bool ok = png.writeImageToStream(img, out);
        if (ok)
            std::printf("png  %s\n", f.getFullPathName().toRawUTF8());
        return ok;
    }

    juce::PopupMenu::Item item(int id, const char* text, const char* shortcut = "", bool enabled = true,
                               bool ticked = false)
    {
        juce::PopupMenu::Item it(text);
        it.itemID = id;
        it.isEnabled = enabled;
        it.isTicked = ticked;
        it.shortcutKeyDescription = shortcut;
        return it;
    }

    // ---- the kinds of menu the pictures show --------------------------------------------------------------------------

    struct Kind
    {
        const char* name;
        std::function<juce::PopupMenu()> build;
        int selected;                                    // the row highlighted (the hover pill), by id
        int canvasH;
        int wheelPx = 0;                                 // scrolled this far down before the picture
    };

    juce::PopupMenu plainMenu()
    {
        juce::PopupMenu m;
        m.addItem(item(1, "Open..."));
        m.addItem(item(2, "Save", "Cmd+S"));
        m.addItem(item(3, "Save As...", "Shift+Cmd+S"));
        m.addSeparator();
        m.addItem(item(4, "Revert", "", false));
        m.addItem(item(5, "Close", "Cmd+W"));
        return m;
    }

    juce::PopupMenu marksMenu()
    {
        juce::PopupMenu m;
        m.addItem(item(1, "Show grid", "", true, true));
        m.addItem(item(2, "Snap to grid"));
        m.addItem(item(3, "Locked", "", false, true));
        m.addSeparator();
        m.addItem(MenuLook::radioItem(4, "Small", false));
        m.addItem(MenuLook::radioItem(5, "Medium", true));
        m.addItem(MenuLook::radioItem(6, "Large", false));
        return m;
    }

    juce::PopupMenu sectionsMenu()
    {
        juce::PopupMenu m;
        m.addSectionHeader("Factory");
        m.addItem(item(1, "Init"));
        m.addItem(item(2, "Bright Pad"));
        m.addSectionHeader("User");
        m.addItem(item(3, "My Bass"));
        m.addItem(item(4, "Lead 2"));
        return m;
    }

    juce::PopupMenu submenusMenu()
    {
        juce::PopupMenu recent, templates, off;
        recent.addItem(item(10, "Bass"));
        templates.addItem(item(11, "Pad"));
        off.addItem(item(12, "Hidden"));
        juce::PopupMenu m;
        m.addSubMenu("Recent", recent);
        m.addSubMenu("Templates", templates, true, nullptr, false, 7);   // an id, so it can open highlighted
        m.addSubMenu("Archived", off, false);
        m.addSeparator();
        m.addItem(item(9, "Clear recent"));
        return m;
    }

    juce::PopupMenu scrollMenu()
    {
        juce::PopupMenu m;
        for (int i = 1; i <= 40; ++i)
            m.addItem(item(i, juce::String::formatted("Project %02d", i).toRawUTF8()));
        return m;
    }

    // A request through popupMenuFor: a header, radio items, a shortcut, a submenu.
    juce::PopupMenu requestMenu()
    {
        funkgui::MenuRequest r;
        funkgui::MenuItem h;
        h.label = "Quality";
        h.header = true;
        r.items.push_back(h);
        funkgui::MenuItem draft{ 1, "Draft" }, best{ 2, "Best", true, true };
        draft.radio = best.radio = true;
        r.items.push_back(draft);
        r.items.push_back(best);
        r.items.push_back({ .separator = true });
        funkgui::MenuItem render{ 3, "Render" };
        render.shortcut = "Cmd+R";
        r.items.push_back(render);
        funkgui::MenuItem more;
        more.label = "More";
        more.submenu = { { 4, "Export" } };
        r.items.push_back(more);
        return MenuLook::popupMenuFor(r);
    }

    const Kind kKinds[] = {
        { "plain", plainMenu, 2, 260 },
        { "marks", marksMenu, 5, 260 },
        { "sections", sectionsMenu, 3, 260 },
        { "submenus", submenusMenu, 7, 260 },
        { "request", requestMenu, 3, 260 },
        { "scroll", scrollMenu, 0, 300, 200 },
    };

    const juce::Rectangle<int> kAnchor{ 30, 16, 120, 24 };       // in the canvas: a dropdown's trigger
    constexpr int kCanvasW = 300;
    const juce::Point<int> kCanvasAt{ 200, 200 };                // where the canvas sits "on screen" (on no desktop)

    // One menu shown in a canvas and its picture. The window stays a child of the canvas, hidden, until JUCE shuts
    // down (the run loop never turns here, so the dismissed menu's own end is never delivered).
    struct Shown
    {
        std::unique_ptr<Canvas> canvas;
        juce::Component* window = nullptr;
        juce::Image picture;
    };

    Shown show(MenuLook& look, const Kind& kind, const funkgui::Theme& th)
    {
        Shown s;
        s.canvas = std::make_unique<Canvas>();
        s.canvas->ground = toJuce(th.ground);
        s.canvas->setBounds(kCanvasAt.x, kCanvasAt.y, kCanvasW, kind.canvasH);
        juce::PopupMenu m = kind.build();
        m.setLookAndFeel(&look);
        auto options = juce::PopupMenu::Options()
                           .withParentComponent(s.canvas.get())
                           .withTargetScreenArea(look.targetAreaFor(kAnchor + kCanvasAt))
                           .withMaximumNumColumns(1);
        if (kind.selected > 0)
            options = options.withInitiallySelectedItem(kind.selected).withItemThatMustBeVisible(kind.selected);
        m.showMenuAsync(options, [](int) {});
        for (int i = s.canvas->getNumChildComponents(); --i >= 0;)
            if (s.canvas->getChildComponent(i)->getName() == "menu")
            {
                s.window = s.canvas->getChildComponent(i);
                break;
            }
        if (s.window != nullptr && kind.wheelPx > 0)
        {
            // A user's wheel: MenuWindow::mouseWheelMove scrolls by -10 x deltaY x its 24 px strip.
            juce::MouseWheelDetails wheel{};
            wheel.deltaY = -static_cast<float>(kind.wheelPx) / 240.0f;
            const juce::Time now = juce::Time::getCurrentTime();
            const juce::MouseEvent e(juce::Desktop::getInstance().getMainMouseSource(), {}, {}, 0.0f, 0.0f, 0.0f, 0.0f,
                                     0.0f, s.window, s.window, now, {}, now, 0, false);
            s.window->mouseWheelMove(e, wheel);
        }
        if (s.window != nullptr)
            s.picture = s.canvas->createComponentSnapshot(s.canvas->getLocalBounds(), true, kSnap);
        juce::PopupMenu::dismissAllActiveMenus();
        return s;
    }

    // The row of `window` that holds the item with this text (its accessibility handler is absent off the desktop,
    // so the rows are told apart by position: the n-th child is the n-th item JUCE kept).
    juce::Component* rowAt(juce::Component* window, int index)
    {
        return window != nullptr && index >= 0 && index < window->getNumChildComponents()
                   ? window->getChildComponent(index)
                   : nullptr;
    }
}

int main(int argc, char** argv)
{
    // The looks outlive JUCE's shutdown, which deletes the dismissed menus' windows that still point at them.
    std::unique_ptr<MenuLook> looks[2];
    const juce::ScopedJuceInitialiser_GUI juceInit;      // MenuLook's typeface; JUCE's displays for a menu's layout
    T::Probe P("fg.menulook", "", argc, argv);
    const funkgui::Theme themes[2] = { funkgui::Theme::graphite(), funkgui::Theme::paper() };
    const char* themeNames[2] = { "graphite", "paper" };
    looks[0] = std::make_unique<MenuLook>(themes[0]);
    looks[1] = std::make_unique<MenuLook>(themes[1]);

    // ---- palette, colour ids, font -----------------------------------------------------------------------------------
    for (int t = 0; t < 2; ++t)
    {
        const funkgui::Theme& th = themes[t];
        const MenuLook::Palette p = MenuLook::paletteFor(th);
        const std::string k = std::string("palette.") + themeNames[t] + ".";
        P.eq(k + "labels_inks", p.text.r == th.ink100.r && p.text.g == th.ink100.g && p.dim.r == th.ink52.r
                                    && p.disabled.r == th.ink32.r && p.mark.r == th.ink100.r, 1);
        const float off = std::fabs(luma(p.surface) - luma(th.ground));
        P.eq(k + "surface_a_shade_off_ground", off >= 2.0f && off <= 40.0f, 1);
        P.eq(k + "hover_apart_from_surface", std::fabs(luma(p.hover) - luma(p.surface)) >= 8.0f, 1);
        P.eq(k + "edge_apart_from_surface", std::fabs(luma(p.edge) - luma(p.surface)) >= 8.0f, 1);
        P.eq(k + "rule_apart_from_surface", std::fabs(luma(p.rule) - luma(p.surface)) >= 6.0f, 1);
        P.eq(k + "shadow_black", p.shadow.r == 0 && p.shadow.g == 0 && p.shadow.b == 0, 1);
        P.eq(k + "shadow_alpha", p.shadow.a, t == 0 ? 127 : 51);
        P.eq(k + "look_palette_is_its_theme", argb(looks[t]->palette().surface), argb(p.surface));
    }
    {
        MenuLook look(themes[0]);
        const MenuLook::Palette p = MenuLook::paletteFor(themes[0]);
        const juce::Colour bg = look.findColour(juce::PopupMenu::backgroundColourId);
        P.eq("colours.background_is_surface", argb(bg.withAlpha(juce::uint8{ 255 })), argb(p.surface));
        P.eq("colours.background_not_opaque", bg.getAlpha() == 254 && !bg.isOpaque(), 1);
        P.eq("colours.text", argb(look.findColour(juce::PopupMenu::textColourId)), argb(p.text));
        P.eq("colours.header", argb(look.findColour(juce::PopupMenu::headerTextColourId)), argb(p.dim));
        P.eq("colours.highlight", argb(look.findColour(juce::PopupMenu::highlightedBackgroundColourId)), argb(p.hover));
        P.eq("colours.highlight_text", argb(look.findColour(juce::PopupMenu::highlightedTextColourId)), argb(p.text));
        look.setTheme(themes[1]);
        const MenuLook::Palette q = MenuLook::paletteFor(themes[1]);
        P.eq("colours.set_theme", argb(look.findColour(juce::PopupMenu::textColourId)) == argb(q.text)
                                      && argb(look.palette().hover) == argb(q.hover), 1);

        const juce::Font f = look.getPopupMenuFont();
        P.eq("font.14px", f.getHeight() == MenuLook::kMenuPx && MenuLook::kMenuPx == 14.0f, 1);
        // The embedded face: made from the bundled bytes, and monospaced, which the fallback face is not.
        const float narrow = juce::GlyphArrangement::getStringWidth(f, "iiii");
        const float wide = juce::GlyphArrangement::getStringWidth(f, "MMMM");
        P.eq("font.bundled_face", f.getTypefacePtr() != nullptr && narrow > 0.0f && narrow == wide, 1);
    }

    // ---- metrics --------------------------------------------------------------------------------------------------------
    MenuLook& look0 = *looks[0];
    const int m = look0.shadowPx();
    {
        const bool transparent = juce::Desktop::canUseSemiTransparentWindows();
        std::printf("metrics  transparent windows %d, margin %d\n", transparent ? 1 : 0, m);
        P.eq("metrics.margin", m, transparent ? MenuLook::kShadowPx : 0);
        P.eq("metrics.border", look0.getPopupMenuBorderSize(), m + MenuLook::kPadPx);
        P.eq("metrics.border_with_options", look0.getPopupMenuBorderSizeWithOptions(juce::PopupMenu::Options()),
             m + MenuLook::kPadPx);
        P.eq("metrics.window_flags", look0.getMenuWindowFlags(), m > 0 ? 0 : juce::ComponentPeer::windowHasDropShadow);
        MenuLook opaque(themes[0]);
        opaque.setColour(juce::PopupMenu::backgroundColourId, juce::Colours::black);
        P.eq("metrics.opaque_background_no_margin", opaque.shadowPx() == 0
                                                        && opaque.getMenuWindowFlags()
                                                               == juce::ComponentPeer::windowHasDropShadow, 1);

        int w = 0, h = 0;
        look0.getIdealPopupMenuItemSize("Save", false, 0, w, h);
        P.eq("metrics.item_height", h, MenuLook::kRowPx);
        P.eq("metrics.item_short_is_min_width", w, MenuLook::kMinPanelPx - 2 * MenuLook::kPadPx);
        int wLong = 0;
        look0.getIdealPopupMenuItemSize("A label long enough to need more room", false, 0, wLong, h);
        const int textW = juce::GlyphArrangement::getStringWidthInt(look0.getPopupMenuFont(),
                                                                    "A label long enough to need more room");
        P.eq("metrics.item_long_holds_text", wLong >= textW + 40 && wLong > w, 1);
        look0.getIdealPopupMenuItemSize("Save", false, 30, w, h);
        P.eq("metrics.item_standard_height", h, 30);
        look0.getIdealPopupMenuItemSize("", true, 0, w, h);
        P.eq("metrics.separator_height", h, MenuLook::kSeparatorPx);
        look0.getIdealPopupMenuItemSize("User presets", false, -1, w, h);
        P.eq("metrics.header_height_after_juce", h + h / 2, MenuLook::kHeaderPx);   // HeaderItemComponent's addition
        juce::Font hf = look0.getPopupMenuFont();
        const int titleW = juce::GlyphArrangement::getStringWidthInt(hf.withHeight(11.5f), "USER PRESETS");
        P.eq("metrics.header_width_holds_title", w + w / 4 >= titleW + 26, 1);
    }

    // ---- targetAreaFor ----------------------------------------------------------------------------------------------
    {
        const int d = m - MenuLook::kGapPx;
        const juce::Rectangle<int> a(100, 200, 80, 24);
        const juce::Rectangle<int> r = look0.targetAreaFor(a);
        const juce::Rectangle<int> want = d > 0 ? juce::Rectangle<int>(100 - m, 200 + d, 80 + 2 * m, 24 - 2 * d)
                                                : juce::Rectangle<int>(100, 200 + d, 80, 24 - 2 * d);
        P.eq("target.dropdown", r == want, 1);
        // JUCE puts the window's top on the area's bottom: the panel (the window less its margin) is kGapPx under.
        P.eq("target.panel_left_on_anchor", r.getX() + m, a.getX());
        P.eq("target.panel_gap_under", r.getBottom() + m - a.getBottom(), MenuLook::kGapPx);
        P.eq("target.panel_gap_over", a.getY() - (r.getY() - m), MenuLook::kGapPx);
        const juce::Rectangle<int> point = look0.targetAreaFor({ 100, 200, 1, 1 });
        P.eq("target.point_kept_whole", point.getHeight() >= 0 && point.getX() + m == 100
                                            && point.getY() >= 199 && point.getY() <= 201, 1);
        const juce::Rectangle<int> scaled = look0.targetAreaFor(a, 2.0f);
        P.eq("target.scaled_margin", scaled.getX(), 100 - 2 * m);
    }

    // ---- radio items ------------------------------------------------------------------------------------------------
    {
        const juce::PopupMenu::Item chosen = MenuLook::radioItem(5, "Medium", true);
        P.eq("radio.chosen", chosen.isTicked && chosen.itemID == 5 && chosen.isEnabled && chosen.image != nullptr
                                 && MenuLook::isRadio(chosen), 1);
        const juce::PopupMenu::Item copy = chosen;       // Item's copy clones the Drawable, ID and all
        P.eq("radio.survives_copy", MenuLook::isRadio(copy), 1);
        const juce::PopupMenu::Item other = MenuLook::radioItem(6, "Large", false, false);
        P.eq("radio.unchosen_plain", !other.isTicked && !other.isEnabled && other.image == nullptr, 1);
        juce::PopupMenu::Item icon("Icon");
        icon.image = std::make_unique<juce::DrawableRectangle>();
        P.eq("radio.other_image_not_radio", MenuLook::isRadio(icon), 0);
    }

    // ---- popupMenuFor -----------------------------------------------------------------------------------------------
    {
        const juce::PopupMenu jm = requestMenu();
        std::vector<const juce::PopupMenu::Item*> rows;
        for (juce::PopupMenu::MenuItemIterator it(jm); it.next();)
            rows.push_back(&it.getItem());
        const bool shape = rows.size() == 6 && rows[0]->isSectionHeader && rows[0]->text == "Quality"
                           && rows[1]->itemID == 1 && !rows[1]->isTicked && !MenuLook::isRadio(*rows[1])
                           && rows[2]->itemID == 2 && rows[2]->isTicked && MenuLook::isRadio(*rows[2])
                           && rows[3]->isSeparator && rows[4]->shortcutKeyDescription == "Cmd+R"
                           && rows[5]->subMenu != nullptr && rows[5]->subMenu->getNumItems() == 1;
        P.eq("request.rows", shape, 1);
        funkgui::MenuRequest utf;
        utf.items.push_back({ 1, "Caf\xC3\xA9" });
        const juce::PopupMenu utfMenu = MenuLook::popupMenuFor(utf);
        juce::PopupMenu::MenuItemIterator it(utfMenu);
        P.eq("request.utf8_label", it.next() && it.getItem().text == juce::String(juce::CharPointer_UTF8("Caf\xC3\xA9")),
             1);
    }

    // ---- MenuKeys ---------------------------------------------------------------------------------------------------
    {
        namespace mk = funkgui::menukeys;
        // header, Bass, bright pad, separator, Brass (disabled), Lead, "  bell"
        const std::vector<bool> sel{ false, true, true, false, false, true, true };
        juce::StringArray labels;
        labels.add("FACTORY"); labels.add("Bass"); labels.add("bright pad"); labels.add("");
        labels.add("Brass (off)"); labels.add("Lead"); labels.add("  bell");
        P.eq("keys.down_from_none_first", mk::step(sel, -1, 1), 1);
        P.eq("keys.up_from_none_last", mk::step(sel, -1, -1), 6);
        P.eq("keys.down_skips_unselectable", mk::step(sel, 2, 1), 5);
        P.eq("keys.down_wraps", mk::step(sel, 6, 1), 1);
        P.eq("keys.up_wraps", mk::step(sel, 1, -1), 6);
        P.eq("keys.none_selectable", mk::step({ false, false }, -1, 1), -1);
        P.eq("keys.home", mk::edge(sel, false), 1);
        P.eq("keys.end", mk::edge(sel, true), 6);
        P.eq("keys.page_down_near_side", mk::page(sel, 1, 3, 1), 2);
        P.eq("keys.page_down_exact", mk::page(sel, 2, 3, 1), 5);
        P.eq("keys.page_down_past_end", mk::page(sel, 5, 3, 1), 6);
        P.eq("keys.page_up_near_side", mk::page(sel, 6, 3, -1), 5);
        P.eq("keys.page_up_past_start", mk::page(sel, 2, 5, -1), 1);
        P.eq("keys.page_down_from_none", mk::page(sel, -1, 2, 1), 1);
        P.eq("keys.type_first", mk::typeAhead(labels, sel, -1, "b"), 1);
        P.eq("keys.type_case_aside", mk::typeAhead(labels, sel, -1, "B"), 1);
        P.eq("keys.type_same_letter_moves_on", mk::typeAhead(labels, sel, 1, "b"), 2);
        P.eq("keys.type_prefix_keeps_current", mk::typeAhead(labels, sel, 2, "br"), 2);
        P.eq("keys.type_skips_disabled", mk::typeAhead(labels, sel, 2, "bra"), -1);
        P.eq("keys.type_leading_spaces", mk::typeAhead(labels, sel, 2, "be"), 6);
        P.eq("keys.type_repeat_cycles", mk::typeAhead(labels, sel, 1, "bb"), 2);
        P.eq("keys.type_repeat_cycles_on", mk::typeAhead(labels, sel, 2, "bbb"), 6);
        P.eq("keys.type_repeat_wraps", mk::typeAhead(labels, sel, 6, "bbbb"), 1);
        P.eq("keys.type_header_not_chosen", mk::typeAhead(labels, sel, -1, "f"), -1);
        P.eq("keys.type_no_match", mk::typeAhead(labels, sel, 1, "x"), -1);
        P.eq("keys.type_empty", mk::typeAhead(labels, sel, 1, ""), -1);
        P.eq("keys.type_wraps", mk::typeAhead(labels, sel, 5, "ba"), 1);
    }

    // ---- real menus, pictures -----------------------------------------------------------------------------------------
    const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
    if (!P.eq("menu.display_known", display != nullptr, 1))
        return P.finish();                               // JUCE places a menu on a display (a headless Linux has none)

    std::vector<Shown> shown;
    std::vector<juce::Image> sheetRow[2];
    for (int t = 0; t < 2; ++t)
    {
        MenuLook& look = *looks[t];
        const MenuLook::Palette pal = look.palette();
        const juce::Colour ground = toJuce(themes[t].ground), surface = toJuce(pal.surface);
        for (const Kind& kind : kKinds)
        {
            const std::string k = std::string("menu.") + kind.name + "." + themeNames[t] + ".";
            Shown s = show(look, kind, themes[t]);
            if (!P.eq(k + "shown", s.window != nullptr && s.picture.isValid(), 1))
                continue;
            const juce::Rectangle<int> win = s.window->getBounds();
            const juce::Rectangle<int> panel = win.reduced(m);
            const juce::Image& img = s.picture;
            std::printf("%swindow %d %d %d %d, rows %d\n", k.c_str(), win.getX(), win.getY(), win.getWidth(),
                        win.getHeight(), s.window->getNumChildComponents());

            // Against the anchor: the panel's left edge on its left edge, kGapPx under it. (JUCE moves a menu taller
            // than its room up: the scroll kind.)
            P.eq(k + "panel_left_on_anchor", panel.getX(), kAnchor.getX());
            if (std::string(kind.name) != "scroll")
                P.eq(k + "panel_gap_under_anchor", panel.getY() - kAnchor.getBottom(), MenuLook::kGapPx);
            else
                P.eq(k + "inside_canvas", s.canvas->getLocalBounds().contains(win), 1);
            P.eq(k + "panel_min_width", panel.getWidth() >= MenuLook::kMinPanelPx, 1);

            // The picture: the canvas at the window's corner (the margin), the surface in the top padding, the
            // shadow under the panel.
            const juce::Colour corner = at(img, static_cast<float>(win.getX()) + 0.5f, static_cast<float>(win.getY()) + 0.5f);
            if (m > 0)
            {
                P.eq(k + "corner_shows_canvas", channelDiff(corner, ground) < channelDiff(corner, surface), 1);
                const juce::Colour under = at(img, static_cast<float>(panel.getCentreX()),
                                              static_cast<float>(panel.getBottom()) + 2.0f);
                P.eq(k + "shadow_under_panel", luma(under) < luma(ground) - 0.5f, 1);
                const juce::Colour round = at(img, static_cast<float>(panel.getX()) + 1.0f,
                                              static_cast<float>(panel.getY()) + 1.0f);
                P.eq(k + "rounded_corner", channelDiff(round, surface) > 2, 1);
            }
            const juce::Colour pad = at(img, static_cast<float>(panel.getRight()) - 12.0f,
                                        static_cast<float>(panel.getY()) + 2.5f);
            P.eq(k + "padding_is_surface", channelDiff(pad, surface) <= 2, 1);

            const auto rowRect = [&](int index) {
                juce::Component* r = rowAt(s.window, index);
                return r == nullptr ? juce::Rectangle<float>() : (r->getBounds() + win.getPosition()).toFloat();
            };
            const auto pillAt = [&](int index) {
                const juce::Rectangle<float> r = rowRect(index);
                return at(img, static_cast<float>(panel.getX() + MenuLook::kInsetPx) + 3.0f, r.getCentreY());
            };
            const auto labelBox = [&](int index) {
                const juce::Rectangle<float> r = rowRect(index);
                return juce::Rectangle<float>(static_cast<float>(panel.getX() + MenuLook::kInsetPx) + 26.0f,
                                              r.getY() + 4.0f, 60.0f, r.getHeight() - 8.0f);
            };
            const auto markBox = [&](int index) {
                const juce::Rectangle<float> r = rowRect(index);
                return juce::Rectangle<float>(static_cast<float>(panel.getX() + MenuLook::kInsetPx) + 8.0f,
                                              r.getCentreY() - 5.0f, 12.0f, 10.0f);
            };
            const auto rightBox = [&](int index, float w) {
                const juce::Rectangle<float> r = rowRect(index);
                return juce::Rectangle<float>(static_cast<float>(panel.getRight() - MenuLook::kInsetPx) - 10.0f - w,
                                              r.getY() + 4.0f, w, r.getHeight() - 8.0f);
            };
            const auto heightOf = [&](int index) {
                juce::Component* r = rowAt(s.window, index);
                return r == nullptr ? -1 : r->getHeight();
            };
            const juce::Colour hover = toJuce(pal.hover);
            const std::string kind_ = kind.name;
            if (kind_ == "plain")
            {
                P.eq(k + "rows", s.window->getNumChildComponents(), 6);
                P.eq(k + "row_heights", heightOf(0) == MenuLook::kRowPx && heightOf(3) == MenuLook::kSeparatorPx, 1);
                P.eq(k + "highlight_pill", channelDiff(pillAt(1), hover) <= 3, 1);
                P.eq(k + "plain_row_no_pill", channelDiff(pillAt(0), surface) <= 2, 1);
                const juce::Rectangle<float> sep = rowRect(3);
                const juce::Colour rule = img.getPixelAt(static_cast<int>(sep.getCentreX() * kSnap),
                                                         static_cast<int>(std::floor(sep.getCentreY() * kSnap)));
                P.eq(k + "separator_rule", channelDiff(rule, toJuce(pal.rule)) <= 3, 1);
                P.eq(k + "shortcut_drawn", contrastIn(img, rightBox(0, 1.0f), surface) < 8
                                               && contrastIn(img, rightBox(2, 40.0f), surface) > 40, 1);
                P.eq(k + "disabled_less_contrast",
                     contrastIn(img, labelBox(4), surface) + 20 < contrastIn(img, labelBox(5), surface), 1);
            }
            else if (kind_ == "marks")
            {
                P.eq(k + "tick", contrastIn(img, markBox(0), surface) > 60, 1);
                P.eq(k + "no_tick", contrastIn(img, markBox(1), surface) < 8, 1);
                P.eq(k + "radio_dot", channelDiff(at(img, static_cast<float>(panel.getX() + MenuLook::kInsetPx) + 14.0f,
                                                      rowRect(5).getCentreY()), toJuce(pal.mark)) <= 40, 1);
                P.eq(k + "radio_unchosen_empty", contrastIn(img, markBox(4), hover) < 8
                                                     || contrastIn(img, markBox(4), surface) < 8, 1);
                P.eq(k + "disabled_tick_dim", contrastIn(img, markBox(2), surface) < contrastIn(img, markBox(0), surface),
                     1);
            }
            else if (kind_ == "sections")
            {
                P.eq(k + "rows", s.window->getNumChildComponents(), 6);
                P.eq(k + "header_heights", heightOf(0) == MenuLook::kHeaderPx && heightOf(3) == MenuLook::kHeaderPx, 1);
                P.eq(k + "header_title_drawn", contrastIn(img, labelBox(0), surface) > 20, 1);
                P.eq(k + "header_dimmer_than_item", contrastIn(img, labelBox(0), surface)
                                                        < contrastIn(img, labelBox(1), surface), 1);
                P.eq(k + "highlight_pill", channelDiff(pillAt(4), hover) <= 3, 1);
            }
            else if (kind_ == "submenus")
            {
                P.eq(k + "chevron", contrastIn(img, rightBox(0, 8.0f), surface) > 30, 1);
                P.eq(k + "chevron_highlighted", contrastIn(img, rightBox(1, 8.0f), hover) > 30, 1);
                P.eq(k + "no_chevron_on_item", contrastIn(img, rightBox(4, 8.0f), surface) < 8, 1);
                P.eq(k + "highlight_pill", channelDiff(pillAt(1), hover) <= 3, 1);
            }
            else if (kind_ == "request")
            {
                P.eq(k + "rows", s.window->getNumChildComponents(), 6);
                P.eq(k + "header_height", heightOf(0), MenuLook::kHeaderPx);
                P.eq(k + "radio_dot", channelDiff(at(img, static_cast<float>(panel.getX() + MenuLook::kInsetPx) + 14.0f,
                                                      rowRect(2).getCentreY()), toJuce(pal.mark)) <= 40, 1);
                P.eq(k + "submenu_chevron", contrastIn(img, rightBox(5, 8.0f), surface) > 30, 1);
            }
            else if (kind_ == "scroll")
            {
                int hidden = 0, outside = 0;
                for (juce::Component* row : s.window->getChildren())
                {
                    if (!row->isVisible())
                        ++hidden;
                    else if (row->getY() < m || row->getBottom() > win.getHeight() - m)
                        ++outside;
                }
                P.eq(k + "scrolls", win.getHeight() < 40 * MenuLook::kRowPx && win.getHeight() <= kind.canvasH, 1);
                P.eq(k + "rows_in_margin_hidden", (m == 0 || hidden > 0) && outside == 0, 1);
                juce::Component* first = rowAt(s.window, 0);
                P.eq(k + "wheel_scrolled", first != nullptr && first->getY() < 0 && !first->isVisible(), 1);
                const float topStrip = static_cast<float>(win.getY() + (m + 24) / 2);
                const float bottomStrip = static_cast<float>(win.getBottom() - (m + 24) / 2);
                const juce::Rectangle<float> mid(static_cast<float>(panel.getCentreX()) - 6.0f, 0.0f, 12.0f, 0.0f);
                P.eq(k + "strip_chevrons", contrastIn(img, mid.withY(topStrip - 3.0f).withHeight(6.0f), surface) > 20
                                               && contrastIn(img, mid.withY(bottomStrip - 3.0f).withHeight(6.0f), surface) > 20,
                     1);
            }
            P.eq(k + "png", writePng(img, juce::String("menulook-") + kind.name + "-" + themeNames[t] + ".png"), 1);
            sheetRow[t].push_back(img);
            shown.push_back(std::move(s));
        }
    }

    // One sheet: each theme's pictures side by side, graphite above paper.
    {
        constexpr int kGap = 16;
        int w = 0, h[2] = { 0, 0 };
        for (int t = 0; t < 2; ++t)
        {
            int rowW = 0;
            for (const juce::Image& img : sheetRow[t])
            {
                rowW += img.getWidth() + kGap;
                h[t] = juce::jmax(h[t], img.getHeight());
            }
            w = juce::jmax(w, rowW);
        }
        if (w > 0)
        {
            juce::Image sheet(juce::Image::ARGB, w, h[0] + h[1] + kGap, true);
            juce::Graphics g(sheet);
            g.fillAll(juce::Colour(0xff808080));
            int y = 0;
            for (int t = 0; t < 2; ++t)
            {
                int x = 0;
                for (const juce::Image& img : sheetRow[t])
                {
                    g.drawImageAt(img, x, y);
                    x += img.getWidth() + kGap;
                }
                y += h[t] + kGap;
            }
            P.eq("png.sheet", writePng(sheet, "menulook-sheet.png"), 1);
        }
    }
    return P.finish();
}

// FUNKGUI_TEST name=fg.host.services.editor timeout=300 gpu=1
//
// fg.host.services.editor: EditorHost's side of the services of panel/HostServices.h (Web Sprint B, v0.12.0) that
// needs neither a window nor the user's clipboard. An EditorHost that was never put on screen:
// - reports all three services, and its commandKeyIsMeta() is JUCE's own rule (ModifierKeys::commandModifier is Cmd
//   on macOS and Ctrl elsewhere);
// - refuses a menu with no items, one whose item has no id and one of separators alone (JUCE would open no window for
//   it and call nothing back): false, and the callback destroyed unrun;
// - leaves JUCE's other menus alone while it shows no menu of its own: a refused request, dismissMenus() and its
//   destructor each leave a juce::PopupMenu that is not the host's in its modal state (JUCE can only dismiss every menu
//   of the process, and HardwareReverb opens its own on ownerComponent()). That menu is shown inside a component that
//   is on no desktop, so it needs a display and no window; the last row dismisses it as the host must not, to show
//   that the rows before it would have seen it;
// - still answers ownerComponent() with itself (HardwareReverb anchors its own menus there);
// - options.*: gives JUCE every field of a request it takes (HostServicesJuce::menuOptions, which showMenu uses), over
//   an owner under a transform of 1 and of 1.5 (a host's content scale, which JUCE's scale for the menu follows) at a
//   UI zoom of 1 and 1.25: the owner as the target, the target area round the anchor on screen, one column, selectedId
//   initially selected and kept visible, minWidth under the zoom plus the look's margins (in the window's px, which are
//   the owner's), and none of those last two for a request without them. The menu is then shown with those options
//   inside the owner, which stands for the window JUCE scales by the same factor (on no desktop, so with no window):
//   a long one is one column that scrolls, and its panel is as wide on screen as the anchor and starts where it does.
// A menu that shows, its dismissal, replacement and the editor leaving its window are fg.host.services.live (a window
// server), and so are the selected row and the width on a real window; the chooser and the clipboard are tried by
// hand in the gallery app's "services" section. Spec rows only.

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Theme.h>
#include <funkgui/gpu/EditorHost.h>
#include <funkgui/juce/MenuLook.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Panel.h>
#include <funkgui/test/Harness.h>

#include "../../src/gpu/HostServicesJuce.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace T = funkgui::test;

namespace
{
    // The processor an AudioProcessorEditor needs: no audio, no state.
    class NoOpProcessor final : public juce::AudioProcessor
    {
    public:
        NoOpProcessor()
            : juce::AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
        {
        }

        const juce::String getName() const override { return "fg.host.services.editor"; }
        void prepareToPlay(double, int) override {}
        void releaseResources() override {}
        void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override { buffer.clear(); }
        double getTailLengthSeconds() const override { return 0.0; }
        bool acceptsMidi() const override { return false; }
        bool producesMidi() const override { return false; }
        bool hasEditor() const override { return false; }
        juce::AudioProcessorEditor* createEditor() override { return nullptr; }
        int getNumPrograms() override { return 1; }
        int getCurrentProgram() override { return 0; }
        void setCurrentProgram(int) override {}
        const juce::String getProgramName(int) override { return {}; }
        void changeProgramName(int, const juce::String&) override {}
        void getStateInformation(juce::MemoryBlock&) override {}
        void setStateInformation(const void*, int) override {}
    };

    class PlainPanel final : public funkgui::Panel
    {
    public:
        funkgui::HostServices* host = nullptr;

        void attach(funkgui::HostServices& h) override { host = &h; }
        int  width() const override { return 200; }
        int  height() const override { return 120; }
        void tick(float) override {}
        void draw(funkgui::Canvas&, const funkgui::Theme&) override {}
        bool wantsFullRate() const override { return false; }
        void accessibility(std::vector<funkgui::A11yItem>&) const override {}
        uint32_t a11yRevision() const override { return 0; }
        void a11yAction(uint32_t, funkgui::A11yAction, double) override {}
        void closeGestures() override {}
    };

    // A menu callback that counts its runs; `alive` expires when the callback has been destroyed.
    struct Call
    {
        int runs = 0;
        std::weak_ptr<int> alive;

        funkgui::MenuCallback menu()
        {
            auto token = std::make_shared<int>(0);
            alive = token;
            return [this, token](int) { ++runs; };
        }

        bool dropped() const { return runs == 0 && alive.expired(); }
    };

    // A popup menu of JUCE's that is not the host's. Its window is a child of `parent`, which is on no desktop: JUCE
    // makes it the modal component inside showMenuAsync, and PopupMenu::dismissAllActiveMenus() ends that at once. The
    // run loop never turns in this test, so the menu's own end is never delivered: its window goes when JUCE shuts
    // down.
    struct JuceMenu
    {
        juce::Component parent;

        JuceMenu()
        {
            parent.setSize(240, 160);
            juce::PopupMenu m;
            m.addItem(1, "Not the host's");
            m.showMenuAsync(juce::PopupMenu::Options().withParentComponent(&parent), [](int) {});
        }

        bool open() const
        {
            const juce::Component* modal = juce::Component::getCurrentlyModalComponent();
            return modal != nullptr && modal->getParentComponent() == &parent;
        }
    };

    // A dropdown's list: 40 rows, the 30th the current choice, as wide as its 160 px trigger at least. The trigger is
    // in from the owner's edges, which JUCE keeps a menu shown inside it within (less the menu's border).
    funkgui::MenuRequest longMenu()
    {
        funkgui::MenuRequest r;
        for (int i = 1; i <= 40; ++i)
            r.items.push_back({ i, "Item " + std::to_string(i) });
        r.anchor = { 40.0f, 30.0f, 160.0f, 20.0f };
        r.selectedId = 30;
        r.minWidth = 160.0f;
        return r;
    }
}

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;          // NSApplication and the message thread, as in a host
    T::Probe P("fg.host.services.editor", "", argc, argv);
    namespace hs = funkgui::hostservice;
    NoOpProcessor processor;
    std::unique_ptr<JuceMenu> juceMenu;

    {
        auto owned = std::make_unique<PlainPanel>();
        PlainPanel& panel = *owned;
        funkgui::EditorHost editor(processor, funkgui::EditorConfig{}, std::move(owned));
        P.eq("attach.host_given", panel.host != nullptr, 1);
        P.eq("attach.no_surface", editor.surfaceAttached(), 0);   // never on screen: the calls below need no window
        if (panel.host == nullptr)
            return P.finish();
        funkgui::HostServices& host = *panel.host;

        P.eq("services.all_three", host.services(), hs::menus | hs::fileChooser | hs::clipboard);
        P.eq("owner.still_the_editor", host.ownerComponent() == static_cast<juce::Component*>(&editor), 1);
        // JUCE's command key: the command modifier is the Ctrl bit exactly where Ctrl is the command key.
        const bool juceMeta = juce::ModifierKeys::commandModifier != juce::ModifierKeys::ctrlModifier;
        P.eq("command_key.is_juces_rule", host.commandKeyIsMeta(), juceMeta);

        // JUCE places a menu on a display, with or without a window.
        const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
        if (P.eq("juce_menu.display_known", display != nullptr, 1))
        {
            juceMenu = std::make_unique<JuceMenu>();
            P.eq("juce_menu.open", juceMenu->open(), 1);
        }

        Call empty, noId, separators;
        const bool emptyRefused = !host.showMenu(funkgui::MenuRequest{}, empty.menu());
        P.eq("menu_refused.no_items", emptyRefused && empty.dropped(), 1);
        funkgui::MenuRequest bad;
        bad.items.push_back({ 1, "First" });
        bad.items.push_back({ 0, "No id" });
        const bool noIdRefused = !host.showMenu(bad, noId.menu());
        P.eq("menu_refused.item_without_id", noIdRefused && noId.dropped(), 1);
        funkgui::MenuRequest onlySeparators;
        onlySeparators.items.push_back({ .separator = true });
        onlySeparators.items.push_back({ .separator = true });
        const bool separatorsRefused = !host.showMenu(onlySeparators, separators.menu());
        P.eq("menu_refused.separators_alone", separatorsRefused && separators.dropped(), 1);
        if (juceMenu != nullptr)
        {
            P.eq("menu_refused.leaves_juces_menu", juceMenu->open(), 1);
            // Nothing of the host's is showing (a request taken with no window to show would count as one).
            host.dismissMenus();
            P.eq("dismiss.nothing_showing_leaves_juces_menu", juceMenu->open(), 1);
        }
    }
    if (juceMenu != nullptr)
    {
        // The editor went with no menu and no chooser pending.
        P.eq("destructor.nothing_pending_leaves_juces_menu", juceMenu->open(), 1);
        const bool dismissed = juce::PopupMenu::dismissAllActiveMenus();
        P.eq("juce_menu.a_dismissal_is_seen", dismissed && !juceMenu->open(), 1);
    }

    // ---- the options of a request that is taken ---------------------------------------------------------------------
    using funkgui::MenuLook;
    for (const int scalePct : { 100, 150 })
        for (const int zoomPct : { 100, 125 })
        {
            const float menuScale = static_cast<float>(scalePct) / 100.0f, zoom = static_cast<float>(zoomPct) / 100.0f;
            const std::string k = "options.scale" + std::to_string(scalePct) + ".zoom" + std::to_string(zoomPct) + ".";
            juce::Component owner;                       // wide enough that JUCE would take more columns if it could
            owner.setBounds(0, 0, 600, 300);
            owner.setTransform(juce::AffineTransform::scale(menuScale));
            MenuLook look(funkgui::Theme::graphite());
            const int m = look.shadowPx();
            const funkgui::MenuRequest request = longMenu();
            const juce::PopupMenu::Options o = funkgui::HostServicesJuce::menuOptions(request, look, owner, zoom);
            const funkgui::Rect& a = request.anchor;
            const juce::Rectangle<int> anchor = owner.localAreaToGlobal(
                juce::Rectangle<float>(a.x * zoom, a.y * zoom, a.w * zoom, a.h * zoom).toNearestInt());
            P.eq(k + "menu_scale_is_the_owners",
                 juce::Component::getApproximateScaleFactorForComponent(&owner) == menuScale, 1);
            P.eq(k + "target_component", o.getTargetComponent() == &owner, 1);
            P.eq(k + "target_area", o.getTargetScreenArea() == look.targetAreaFor(anchor, menuScale), 1);
            P.eq(k + "one_column", o.getMaximumNumColumns(), 1);
            P.eq(k + "selected_and_visible", o.getInitiallySelectedItemId() == 30 && o.getItemThatMustBeVisible() == 30,
                 1);
            P.eq(k + "min_width", o.getMinimumWidth(), juce::roundToInt(160.0f * zoom) + 2 * m);
            funkgui::MenuRequest plain = request;
            plain.selectedId = 0;
            plain.minWidth = 0.0f;
            const juce::PopupMenu::Options none = funkgui::HostServicesJuce::menuOptions(plain, look, owner, zoom);
            P.eq(k + "unset", none.getMinimumWidth() == 0 && none.getInitiallySelectedItemId() == 0
                                  && none.getItemThatMustBeVisible() == 0, 1);

            if (juce::Desktop::getInstance().getDisplays().getPrimaryDisplay() == nullptr)
                continue;                                // JUCE places a menu on a display
            juce::PopupMenu menu = MenuLook::popupMenuFor(request);
            menu.setLookAndFeel(&look);
            menu.showMenuAsync(o.withParentComponent(&owner), [](int) {});
            juce::Component* window = nullptr;
            for (juce::Component* c : owner.getChildren())
                if (c->getName() == "menu")
                    window = c;
            if (P.eq(k + "shown", window != nullptr, 1))
            {
                bool oneColumn = window->getNumChildComponents() == 40;
                for (const juce::Component* row : window->getChildren())
                    oneColumn = oneColumn && row->getX() == 0 && row->getWidth() == window->getWidth();
                P.eq(k + "shown_one_column", oneColumn, 1);
                P.eq(k + "shown_scrolls", window->getHeight() < 40 * MenuLook::kRowPx, 1);
                // The panel (the window less the look's margins) on screen, against the anchor on screen.
                const juce::Rectangle<int> panel = owner.localAreaToGlobal(window->getBounds().reduced(m));
                std::printf("%spanel on screen %d %d %d %d, anchor %d %d %d %d\n", k.c_str(), panel.getX(),
                            panel.getY(), panel.getWidth(), panel.getHeight(), anchor.getX(), anchor.getY(),
                            anchor.getWidth(), anchor.getHeight());
                P.eq(k + "shown_panel_as_wide_as_anchor", std::abs(panel.getWidth() - anchor.getWidth()) <= 1, 1);
                P.eq(k + "shown_panel_left_on_anchor", std::abs(panel.getX() - anchor.getX()) <= 1, 1);
            }
            juce::PopupMenu::dismissAllActiveMenus();   // its window goes at JUCE's shutdown, after the look
        }
    return P.finish();
}

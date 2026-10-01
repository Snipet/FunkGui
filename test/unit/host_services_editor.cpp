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
// - still answers ownerComponent() with itself (HardwareReverb anchors its own menus there).
// A menu that shows, its dismissal, replacement and the editor leaving its window are fg.host.services.live (a window
// server); the chooser and the clipboard are tried by hand in the gallery app's "services" section. Spec rows only.

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Theme.h>
#include <funkgui/gpu/EditorHost.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Panel.h>
#include <funkgui/test/Harness.h>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <memory>
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
    return P.finish();
}

// FUNKGUI_TEST name=fg.host.services.live timeout=300 gpu=1 labels=live
//
// fg.host.services.live: EditorHost's menu over JUCE against the rules of panel/HostServices.h (Web Sprint B, v0.12.0;
// src/gpu/HostServicesJuce.cpp). Labels fg;gpu;live: it opens real popup menus and a borderless window, so it needs a
// window server and is never in `verify`. Run it serialised:
//   lockf -k -t 900 /tmp/fcmp-gui.lock ctest --test-dir <build-agent-gui> -L live
//
// A menu ends here by JUCE's own dismissal (PopupMenu::dismissAllActiveMenus(), which is what a click outside it
// comes to; JUCE also closes a menu by itself when no component of the process has the focus, which the rows allow
// for), by the keys a menu takes (Down, Down, Return, given to the menu's window before the run loop turns), by
// HostServices::dismissMenus(), by a second menu, by the editor leaving its window or by the editor's destructor. The
// anchor under a zoom, the look, the chooser's dialogs and the clipboard are tried by hand in the gallery app's
// "services" section.
//   show.*       taken; not run inside the call; a user's dismissal runs it once, with 0, and destroys it
//   choose.*     the second item chosen with the keys: the callback runs once, with that item's id, from the run loop
//   dismiss.*    dismissMenus(): the menu is closed and its callback destroyed unrun, both at once
//   replace.*    a second showMenu after the user chose from the first, before JUCE delivered the choice: the first
//                menu's result arrives while the second is open and must not reach the second's callback (the serial
//                of HostServicesJuce.cpp); a refused second closes the first too
//   again.*      a callback that asks for the next menu: taken, and not run inside that call either
//   window.*     the editor is taken out of its window with a menu and a chooser pending: both are closed, neither runs
//   destroy.*    the editor is destroyed with a menu and a chooser pending: both are closed, neither runs
//   chooser.*    a second chooseFiles replaces the first (asked back to back, so no dialog gets to open)
// A callback that was dropped is checked where it is dropped (its token has expired, so it can never run after that);
// the run loop still turns afterwards, so JUCE's own late callback of each closed menu arrives and must find nothing.
// "Open" and "closed" are JUCE's count of modal components, read with no run-loop turn: a menu's window and a chooser
// are each modal from the call that opens them until they are dismissed. Spec rows only.

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Theme.h>
#include <funkgui/gpu/EditorHost.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Panel.h>
#include <funkgui/test/Harness.h>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <CoreFoundation/CoreFoundation.h>

#include <cstdint>
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

        const juce::String getName() const override { return "fg.host.services.live"; }
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
        int  width() const override { return 320; }
        int  height() const override { return 200; }
        void tick(float) override {}
        void draw(funkgui::Canvas& c, const funkgui::Theme& th) override
        {
            c.rrect(16.0f, 16.0f, 64.0f, 20.0f, 3.0f, th.ink52);
        }
        bool wantsFullRate() const override { return false; }
        void accessibility(std::vector<funkgui::A11yItem>&) const override {}
        uint32_t a11yRevision() const override { return 0; }
        void a11yAction(uint32_t, funkgui::A11yAction, double) override {}
        void closeGestures() override {}
    };

    // What a callback did; `alive` expires when the callback has been destroyed.
    struct Call
    {
        int runs = 0, id = -1;
        std::weak_ptr<int> alive;

        funkgui::MenuCallback menu()
        {
            auto token = std::make_shared<int>(0);
            alive = token;
            return [this, token](int chosen) {
                ++runs;
                id = chosen;
            };
        }

        funkgui::FilesCallback files()
        {
            auto token = std::make_shared<int>(0);
            alive = token;
            return [this, token](const std::vector<std::string>&) { ++runs; };
        }

        bool dropped() const { return runs == 0 && alive.expired(); }      // destroyed without being called
        bool pending() const { return runs == 0 && !alive.expired(); }     // held, not called yet
        bool ranOnceWithZero() const { return runs == 1 && id == 0 && alive.expired(); }
    };

    // How many menus and choosers are open, as JUCE sees it.
    int modalCount() { return juce::ModalComponentManager::getInstance()->getNumModalComponents(); }

    funkgui::MenuRequest sampleMenu()
    {
        funkgui::MenuRequest m;
        m.items.push_back({ 1, "First" });
        m.items.push_back({ 2, "Second", true, true });
        m.items.push_back({ .separator = true });
        m.items.push_back({ 3, "Disabled", false });
        m.anchor = { 16.0f, 16.0f, 64.0f, 20.0f };
        return m;
    }

    funkgui::FileRequest sampleChooser()
    {
        funkgui::FileRequest r;
        r.title = "fg.host.services.live";
        r.pattern = "*.txt";
        return r;
    }

    // Turn the run loop for a moment: JUCE's message queue (the menus' async callbacks) and window ordering.
    void spin(double seconds)
    {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
    }

    // What a click outside a menu comes to.
    void userDismisses()
    {
        juce::PopupMenu::dismissAllActiveMenus();
        spin(0.25);
    }

    // What a user's Down, Down, Return comes to: the second item that can be chosen is highlighted, then chosen. The
    // menu's window is the modal component from the moment showMenu returns. False when there is no menu.
    bool userChoosesSecondItem()
    {
        juce::Component* menu = juce::Component::getCurrentlyModalComponent();
        if (menu == nullptr)
            return false;
        menu->keyPressed(juce::KeyPress(juce::KeyPress::downKey));
        menu->keyPressed(juce::KeyPress(juce::KeyPress::downKey));
        menu->keyPressed(juce::KeyPress(juce::KeyPress::returnKey));
        return true;
    }

    struct Rig
    {
        PlainPanel* panel = nullptr;
        std::unique_ptr<funkgui::EditorHost> editor;

        explicit Rig(juce::AudioProcessor& processor)
        {
            auto owned = std::make_unique<PlainPanel>();
            panel = owned.get();
            editor = std::make_unique<funkgui::EditorHost>(processor, funkgui::EditorConfig{}, std::move(owned));
        }

        funkgui::HostServices& host() const { return *panel->host; }
    };
}

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;          // NSApplication and the message thread, as in a host
    T::Probe P("fg.host.services.live", "", argc, argv);
    NoOpProcessor processor;

    // ---- an editor with no window: a menu anchors on the component all the same ----------------------------------
    {
        Rig r(processor);
        funkgui::HostServices& h = r.host();

        Call a;
        const bool taken = h.showMenu(sampleMenu(), a.menu());
        P.eq("show.taken", taken, 1);
        P.eq("show.not_run_inside_the_call", a.pending(), 1);
        spin(0.25);
        // Still open, or closed by JUCE itself (not the frontmost application): never more than one run, and only 0.
        P.eq("show.at_most_once_while_open", a.pending() || a.ranOnceWithZero(), 1);
        userDismisses();
        P.eq("show.dismissal_runs_once_with_zero", a.ranOnceWithZero(), 1);
        userDismisses();                             // nothing left to dismiss: no second run
        P.eq("show.at_most_once", a.runs, 1);

        Call k;
        h.showMenu(sampleMenu(), k.menu());
        P.eq("choose.keys_reached_the_menu", userChoosesSecondItem(), 1);
        P.eq("choose.not_run_before_the_loop_turns", k.pending(), 1);
        spin(0.25);
        P.eq("choose.ran_once_with_the_chosen_id", k.runs == 1 && k.id == 2 && k.alive.expired(), 1);
        userDismisses();
        P.eq("choose.at_most_once", k.runs, 1);

        Call b;
        h.showMenu(sampleMenu(), b.menu());
        P.eq("dismiss.menu_open", modalCount(), 1);
        h.dismissMenus();
        P.eq("dismiss.dropped_at_once", b.dropped(), 1);
        P.eq("dismiss.menu_closed_at_once", modalCount(), 0);
        spin(0.25);                                  // JUCE's own callback of the closed menu arrives: nothing to run
        h.dismissMenus();                            // nothing showing

        // The user chooses the first menu's second item; before the run loop turns, a second menu replaces the first.
        // JUCE delivers the first menu's 2 while the second is open: it is not the second's to receive.
        Call c, d;
        h.showMenu(sampleMenu(), c.menu());
        P.eq("replace.keys_reached_the_first", userChoosesSecondItem(), 1);
        const bool second = h.showMenu(sampleMenu(), d.menu());
        P.eq("replace.second_taken", second, 1);
        P.eq("replace.first_dropped_at_once", c.dropped() && d.pending(), 1);
        spin(0.25);                                  // the first menu's result arrives
        // The second is still open, or was closed by JUCE itself (with 0): it never saw the first one's 2.
        P.eq("replace.stale_choice_not_delivered", d.pending() || d.ranOnceWithZero(), 1);
        userDismisses();
        P.eq("replace.second_ran_once_with_zero", d.ranOnceWithZero(), 1);

        Call e, f;
        h.showMenu(sampleMenu(), e.menu());
        const bool refused = !h.showMenu(funkgui::MenuRequest{}, f.menu());
        P.eq("replace.refused_second_closes_the_first", refused && e.dropped() && f.dropped() && modalCount() == 0, 1);
        spin(0.25);

        // A callback that asks for the next menu.
        Call follow;
        int outerRuns = 0;
        bool followTaken = false, followPendingInside = false;
        h.showMenu(sampleMenu(), [&](int) {
            ++outerRuns;
            followTaken = h.showMenu(sampleMenu(), follow.menu());
            followPendingInside = follow.pending();
        });
        spin(0.1);
        userDismisses();                             // closes the first; its callback opens the second
        P.eq("again.outer_ran_once", outerRuns, 1);
        P.eq("again.follow_up_taken_not_run_inside", followTaken && followPendingInside, 1);
        userDismisses();
        P.eq("again.follow_up_ran_once_with_zero", follow.ranOnceWithZero() && outerRuns == 1, 1);

        // Two choosers asked back to back: the run loop does not turn between them, so JUCE opens no dialog.
        Call g, i;
        const bool first = h.chooseFiles(sampleChooser(), g.files());
        const bool next = h.chooseFiles(sampleChooser(), i.files());
        P.eq("chooser.both_taken", first && next, 1);
        P.eq("chooser.first_dropped_unrun", g.dropped() && i.pending(), 1);

        // ---- destruction with a menu and that chooser pending ----------------------------------------------------
        Call m;
        h.showMenu(sampleMenu(), m.menu());
        P.eq("destroy.both_open", modalCount(), 2);
        r.editor.reset();
        P.eq("destroy.menu_dropped", m.dropped(), 1);
        P.eq("destroy.chooser_dropped", i.dropped(), 1);
        P.eq("destroy.both_closed", modalCount(), 0);
        spin(0.3);                                   // whatever JUCE still had queued finds nothing to call
        userDismisses();
    }

    // ---- an editor in a window, taken out of it ---------------------------------------------------------------------
    {
        Rig r(processor);
        funkgui::HostServices& h = r.host();
        juce::Component top;
        top.setOpaque(true);
        top.setBounds(200, 200, 360, 240);           // screen coordinates: a borderless window
        r.editor->setTopLeftPosition(20, 20);
        top.addAndMakeVisible(*r.editor);
        top.setVisible(true);
        top.addToDesktop(0);
        spin(0.2);
        P.eq("window.editor_has_a_peer", r.editor->getPeer() != nullptr, 1);

        Call menu, files;
        h.showMenu(sampleMenu(), menu.menu());
        h.chooseFiles(sampleChooser(), files.files());
        P.eq("window.both_pending", menu.pending() && files.pending(), 1);
        P.eq("window.both_open", modalCount(), 2);
        top.removeChildComponent(r.editor.get());    // parentHierarchyChanged() with no peer: the host lets go
        P.eq("window.dropped_on_leaving", menu.dropped() && files.dropped(), 1);
        P.eq("window.closed_on_leaving", modalCount(), 0);
        spin(0.3);
        userDismisses();

        // Back in a window, the services serve again.
        top.addAndMakeVisible(*r.editor);
        spin(0.1);
        Call back;
        const bool taken = h.showMenu(sampleMenu(), back.menu());
        P.eq("window.serves_again", taken && back.pending(), 1);
        userDismisses();
        P.eq("window.again_ran_once_with_zero", back.ranOnceWithZero(), 1);
        top.removeFromDesktop();
    }
    return P.finish();
}

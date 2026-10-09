// FUNKGUI_TEST name=fg.editorhost.cursor timeout=300 gpu=1
//
// fg.editorhost.cursor on macOS (FSynth G1; the Linux test is editorhost_cursor_linux.cpp, under the same name):
// EditorHost::updateCursor() for the nine values of funkgui::Cursor (panel/Input.h). No window: an EditorHost that is
// never put on screen, a mouse event made as fg.editorhost.zoom makes one, then Component::getMouseCursor() compared
// with juce::MouseCursor's operator==. Spec rows only.
//
//   v0140.*      the five cursors of v0.14.0 are the standard types they were
//   standard.*   grab is DraggingHandCursor and copy is CopyingCursor (the system's open hand and arrow-with-plus)
//   system.*     grabbing and notAllowed equal no standard type (JUCE 8.0.4 has none for them: they are made from the
//                system's closed hand and not-allowed sign, src/gpu/Cursors.mm), differ from each other and from grab
//   once.*       a second request while the editor lives shows an equal cursor, and so does a second editor: each is
//                made once and shared, not made per request or per editor
//   events.*     every kind of pointer event is followed by a cursor update
//   again.*      after every editor has gone, a new editor shows the cursors again
// A juce::MouseCursor has no public null state, and NormalCursor itself holds a null handle, so no row asks whether a
// cursor "exists". The images and their hot spots are looked at live, in the gallery app.

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Env.h>
#include <funkgui/core/Theme.h>
#include <funkgui/gpu/EditorHost.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Input.h>
#include <funkgui/panel/Panel.h>
#include <funkgui/test/Harness.h>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace T = funkgui::test;
using funkgui::Cursor;
using MC = juce::MouseCursor;

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

        const juce::String getName() const override { return "fg.editorhost.cursor"; }
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

    // A Panel whose cursor is whatever the test sets.
    class CursorPanel final : public funkgui::Panel
    {
    public:
        Cursor shown = Cursor::normal;

        void attach(funkgui::HostServices&) override {}
        int  width() const override { return 200; }
        int  height() const override { return 120; }
        void tick(float) override {}
        void draw(funkgui::Canvas&, const funkgui::Theme&) override {}
        bool wantsFullRate() const override { return false; }
        Cursor cursor() const override { return shown; }
        void accessibility(std::vector<funkgui::A11yItem>&) const override {}
        uint32_t a11yRevision() const override { return 0; }
        void a11yAction(uint32_t, funkgui::A11yAction, double) override {}
        void closeGestures() override {}
    };

    struct Rig
    {
        CursorPanel* panel = nullptr;
        std::unique_ptr<funkgui::EditorHost> editor;

        explicit Rig(juce::AudioProcessor& p)
        {
            auto owned = std::make_unique<CursorPanel>();
            panel = owned.get();
            editor = std::make_unique<funkgui::EditorHost>(p, funkgui::EditorConfig{}, std::move(owned));
        }

        juce::MouseEvent mouseAt(float x, float y) const
        {
            const auto now = juce::Time::getCurrentTime();
            return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), { x, y }, juce::ModifierKeys(),
                                    0.0f, 0.0f, 0.0f, 0.0f, 0.0f, editor.get(), editor.get(), now, { x, y }, now, 1,
                                    false);
        }

        // The cursor the editor shows once the Panel answers `c` and the pointer moves.
        MC show(Cursor c) const
        {
            panel->shown = c;
            editor->mouseMove(mouseAt(20.0f, 20.0f));
            return editor->getMouseCursor();
        }
    };

    bool equalsNoStandardType(const MC& c)
    {
        for (int t = 0; t < MC::NumStandardCursorTypes; ++t)
            if (c == static_cast<MC::StandardCursorType>(t))
                return false;
        return true;
    }

    void setEnvVar(const char* suffix, const char* value)
    {
        const std::string name = std::string(FUNKGUI_ENV_PREFIX) + suffix;
        if (value != nullptr)
            T::setEnv(name.c_str(), value);
        else
            T::unsetEnv(name.c_str());
        funkgui::envReload();
    }
}

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;          // NSApplication and the message thread, as in a host
    T::Probe P("fg.editorhost.cursor", "", argc, argv);

    // This test owns its capture environment: a capture takes no pointer input at all.
    setEnvVar("CANVAS_DUMP", nullptr);
    setEnvVar("UI_ZOOM", nullptr);
    NoOpProcessor processor;

    {
        Rig r(processor);

        // ---- the five of v0.14.0 --------------------------------------------------------------------------------
        P.eq("v0140.normal", r.show(Cursor::normal) == MC::NormalCursor, 1);
        P.eq("v0140.left_right", r.show(Cursor::leftRight) == MC::LeftRightResizeCursor, 1);
        P.eq("v0140.up_down", r.show(Cursor::upDown) == MC::UpDownResizeCursor, 1);
        P.eq("v0140.pointing_hand", r.show(Cursor::pointingHand) == MC::PointingHandCursor, 1);
        P.eq("v0140.crosshair", r.show(Cursor::crosshair) == MC::CrosshairCursor, 1);

        // ---- the two JUCE has a standard type for ---------------------------------------------------------------
        P.eq("standard.grab", r.show(Cursor::grab) == MC::DraggingHandCursor, 1);
        P.eq("standard.copy", r.show(Cursor::copy) == MC::CopyingCursor, 1);

        // ---- the two it has none for: the system's, made from their images ----------------------------------------
        const MC grabbing = r.show(Cursor::grabbing);
        const MC notAllowed = r.show(Cursor::notAllowed);
        P.eq("system.grabbing_is_no_standard_type", equalsNoStandardType(grabbing), 1);
        P.eq("system.not_allowed_is_no_standard_type", equalsNoStandardType(notAllowed), 1);
        P.eq("system.they_differ", grabbing != notAllowed, 1);
        P.eq("system.grabbing_is_not_grab", grabbing != r.show(Cursor::grab), 1);
        P.eq("system.not_allowed_is_not_normal", notAllowed != r.show(Cursor::normal), 1);

        // ---- made once ------------------------------------------------------------------------------------------
        P.eq("once.grabbing_again", r.show(Cursor::grabbing) == grabbing, 1);
        P.eq("once.not_allowed_again", r.show(Cursor::notAllowed) == notAllowed, 1);
        r.show(Cursor::normal);
        P.eq("once.grabbing_after_normal", r.show(Cursor::grabbing) == grabbing, 1);
        {
            Rig second(processor);                           // while the first editor lives: the same cursors
            P.eq("once.second_editor_grabbing", second.show(Cursor::grabbing) == grabbing, 1);
            P.eq("once.second_editor_not_allowed", second.show(Cursor::notAllowed) == notAllowed, 1);
        }
        P.eq("once.first_editor_keeps_them", r.show(Cursor::grabbing) == grabbing
                                                 && r.show(Cursor::notAllowed) == notAllowed, 1);

        // ---- every pointer event updates the cursor ---------------------------------------------------------------
        r.show(Cursor::normal);
        r.panel->shown = Cursor::grabbing;
        r.editor->mouseDown(r.mouseAt(20.0f, 20.0f));
        P.eq("events.down", r.editor->getMouseCursor() == grabbing, 1);
        r.panel->shown = Cursor::notAllowed;
        r.editor->mouseDrag(r.mouseAt(60.0f, 30.0f));
        P.eq("events.drag", r.editor->getMouseCursor() == notAllowed, 1);
        r.panel->shown = Cursor::copy;
        r.editor->mouseDrag(r.mouseAt(80.0f, 30.0f));
        P.eq("events.drag_copy", r.editor->getMouseCursor() == MC::CopyingCursor, 1);
        r.panel->shown = Cursor::grab;
        r.editor->mouseUp(r.mouseAt(80.0f, 30.0f));
        P.eq("events.up", r.editor->getMouseCursor() == MC::DraggingHandCursor, 1);
        r.panel->shown = Cursor::normal;
        r.editor->mouseExit(r.mouseAt(-5.0f, 30.0f));
        P.eq("events.exit", r.editor->getMouseCursor() == MC::NormalCursor, 1);
    }

    // ---- after the last editor has gone, a new one shows them again -------------------------------------------------
    {
        Rig r(processor);
        const MC grabbing = r.show(Cursor::grabbing);
        const MC notAllowed = r.show(Cursor::notAllowed);
        P.eq("again.grabbing", equalsNoStandardType(grabbing), 1);
        P.eq("again.not_allowed", equalsNoStandardType(notAllowed) && grabbing != notAllowed, 1);
        // An editor that never asks for them makes none: opening and closing it is all that happens.
        Rig idle(processor);
        P.eq("again.idle_editor_is_normal", idle.show(Cursor::normal) == MC::NormalCursor, 1);
    }

    return P.finish();
}

// FUNKGUI_TEST name=fg.editorhost.cursor timeout=300 gpu=1
//
// fg.editorhost.cursor on Linux (FSynth G1; the macOS test is editorhost_cursor.mm, under the same name):
// EditorHost::updateCursor() for the nine values of funkgui::Cursor (panel/Input.h). No window and no X display
// needed: an EditorHost that is never put on screen, a mouse event made as fg.editorhost.zoom makes one, then
// Component::getMouseCursor() compared with juce::MouseCursor's operator==(StandardCursorType), which reads the
// cursor's type and never its X handle. Spec rows only.
//
//   v0140.*      the five cursors of v0.14.0 are the standard types they were
//   standard.*   grab is DraggingHandCursor and copy is CopyingCursor (JUCE's own hand and arrow-with-plus on X11)
//   fallback.*   JUCE 8.0.4 reaches no native closed-hand or not-allowed cursor on X11, so src/gpu/linux/Cursors.cpp
//                returns standard cursors: grabbing is DraggingHandCursor, the hand grab already shows, and
//                notAllowed is NormalCursor, the arrow. A refusal therefore shows only in what a Panel draws.
//   once.*       a second request and a second editor show equal cursors
//   events.*     every kind of pointer event is followed by a cursor update
//
// Written on macOS, where no Linux build runs: this file has not been compiled or run by its author.

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
    const juce::ScopedJuceInitialiser_GUI juceInit;          // the message thread, as in a host
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

        // ---- the two it has none for: the stated fallback ---------------------------------------------------------
        const MC grabbing = r.show(Cursor::grabbing);
        const MC notAllowed = r.show(Cursor::notAllowed);
        P.eq("fallback.grabbing_is_dragging_hand", grabbing == MC::DraggingHandCursor, 1);
        P.eq("fallback.not_allowed_is_normal", notAllowed == MC::NormalCursor, 1);
        P.eq("fallback.grabbing_keeps_the_hand_of_grab", grabbing == r.show(Cursor::grab), 1);

        // ---- the same cursor again, here and in a second editor ---------------------------------------------------
        P.eq("once.grabbing_again", r.show(Cursor::grabbing) == grabbing, 1);
        P.eq("once.not_allowed_again", r.show(Cursor::notAllowed) == notAllowed, 1);
        {
            Rig second(processor);
            P.eq("once.second_editor_grabbing", second.show(Cursor::grabbing) == MC::DraggingHandCursor, 1);
            P.eq("once.second_editor_not_allowed", second.show(Cursor::notAllowed) == MC::NormalCursor, 1);
        }

        // ---- every pointer event updates the cursor ---------------------------------------------------------------
        r.show(Cursor::normal);
        r.panel->shown = Cursor::grabbing;
        r.editor->mouseDown(r.mouseAt(20.0f, 20.0f));
        P.eq("events.down", r.editor->getMouseCursor() == MC::DraggingHandCursor, 1);
        r.panel->shown = Cursor::copy;
        r.editor->mouseDrag(r.mouseAt(60.0f, 30.0f));
        P.eq("events.drag_copy", r.editor->getMouseCursor() == MC::CopyingCursor, 1);
        r.panel->shown = Cursor::notAllowed;
        r.editor->mouseDrag(r.mouseAt(80.0f, 30.0f));
        P.eq("events.drag_not_allowed", r.editor->getMouseCursor() == MC::NormalCursor, 1);
        r.panel->shown = Cursor::grab;
        r.editor->mouseUp(r.mouseAt(80.0f, 30.0f));
        P.eq("events.up", r.editor->getMouseCursor() == MC::DraggingHandCursor, 1);
        r.panel->shown = Cursor::normal;
        r.editor->mouseExit(r.mouseAt(-5.0f, 30.0f));
        P.eq("events.exit", r.editor->getMouseCursor() == MC::NormalCursor, 1);
    }

    return P.finish();
}

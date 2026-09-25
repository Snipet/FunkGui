// FUNKGUI_TEST name=fg.editorhost.api timeout=300 gpu=1
//
// fg.editorhost.api: EditorHost's side of the G7b additions (v0.7.1) that need no window (gpu/EditorHost.h,
// panel/HostServices.h, panel/Panel.h). An EditorHost that was never put on screen (no peer, no surface):
// - HostServices::ownerComponent() is the editor itself;
// - HostServices::themeIndex() is the preference (or the <PREFIX>UI_THEME override when one is set) and follows a
//   preference write at once, before any frame, which is what lets a theme cell's click draw in the next frame;
// - the JUCE file-drag calls reach the Panel: isInterestedInFileDrag -> filesInterest, fileDragEnter -> filesDragEnter
//   (the paths as UTF-8, the position as float), fileDragMove -> filesDragMove, fileDragExit -> filesDragExit, and a
//   drop -> filesDropped with no exit after it;
// - a Panel written before v0.7.1 (no drag overrides) takes the same calls as no-ops.
// The placement of the render view and the frame a theme write lands in need a window: fg.editorhost.follow (live).
// Every write to the preferences goes to this test's sandbox (FUNKGUI_PREFS_DIR). Spec rows only.

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Theme.h>
#include <funkgui/gpu/EditorHost.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Panel.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/test/Harness.h>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
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

        const juce::String getName() const override { return "fg.editorhost.api"; }
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

    // A Panel as every Panel was before v0.7.1: it overrides none of the file-drag calls.
    class PlainPanel : public funkgui::Panel
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

    // A Panel that accepts drags of ".preset" files and records every call in order.
    class DropPanel final : public PlainPanel
    {
    public:
        mutable std::vector<std::string> interestPaths;
        std::vector<std::string> enterPaths, dropPaths, calls;
        float x = -1.0f, y = -1.0f;

        bool filesInterest(const std::vector<std::string>& p) const override
        {
            interestPaths = p;
            return std::any_of(p.begin(), p.end(), [](const std::string& s) {
                return s.size() >= 7 && s.compare(s.size() - 7, 7, ".preset") == 0;
            });
        }
        void filesDragEnter(const std::vector<std::string>& p, float px, float py) override
        {
            calls.push_back("enter");
            enterPaths = p;
            x = px;
            y = py;
        }
        void filesDragMove(float px, float py) override
        {
            calls.push_back("move");
            x = px;
            y = py;
        }
        void filesDragExit() override { calls.push_back("exit"); }
        void filesDropped(const std::vector<std::string>& p) override
        {
            calls.push_back("drop");
            dropPaths = p;
        }
    };

    std::string joined(const std::vector<std::string>& v)
    {
        std::string out;
        for (const std::string& s : v)
            out += (out.empty() ? "" : ",") + s;
        return out;
    }
}

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;          // NSApplication and the message thread, as in a host
    T::Probe P("fg.editorhost.api", "", argc, argv);

    auto& prefs = funkgui::UiPreferences::get();
    prefs.setTheme(0);                               // the sandbox may hold the last run's choice
    NoOpProcessor processor;

    {
        auto owned = std::make_unique<DropPanel>();
        DropPanel& panel = *owned;
        funkgui::EditorHost editor(processor, funkgui::EditorConfig{}, std::move(owned));
        P.eq("attach.host_given", panel.host != nullptr, 1);
        P.eq("attach.no_surface", editor.surfaceAttached(), 0);   // never on screen: the calls below need no window
        if (panel.host == nullptr)
            return P.finish();
        funkgui::HostServices& host = *panel.host;

        // ---- ownerComponent(): the editor -------------------------------------------------------------------------
        P.eq("owner.is_editor", host.ownerComponent() == static_cast<juce::Component*>(&editor), 1);
        P.eq("owner.stable", host.ownerComponent() == host.ownerComponent(), 1);

        // ---- themeIndex(): the preference, followed at once; or the capture override ------------------------------
        const int forced = editor.capture().uiTheme;
        std::printf("INFO     UI_THEME override %d (-1: none)\n", forced);
        const auto want = [forced](int pref) {
            return forced >= 0 ? std::clamp(forced, 0, funkgui::Theme::kCount - 1) : pref;
        };
        P.eq("theme.initial", host.themeIndex(), want(0));
        prefs.setTheme(1);                           // what a theme cell's click does
        P.eq("theme.after_write", host.themeIndex(), want(1));
        prefs.setTheme(0);
        P.eq("theme.after_second_write", host.themeIndex(), want(0));
        prefs.setTheme(funkgui::Theme::kCount + 3);  // UiPreferences clamps; the index is always a theme
        P.in("theme.in_range", host.themeIndex(), 0, funkgui::Theme::kCount - 1);
        prefs.setTheme(0);

        // ---- file drags: JUCE's calls reach the Panel ------------------------------------------------------------
        juce::StringArray files;
        files.add("/tmp/a.preset");
        files.add(juce::String::fromUTF8("/tmp/\xC3\xBC\xC3\xB1.preset"));
        const std::vector<std::string> want8 = { "/tmp/a.preset", "/tmp/\xC3\xBC\xC3\xB1.preset" };
        P.eq("drag.interest_accepts", editor.isInterestedInFileDrag(files), 1);
        P.eq("drag.interest_paths", panel.interestPaths == want8, 1);
        P.eq("drag.interest_refuses", editor.isInterestedInFileDrag(juce::StringArray("/tmp/a.wav")), 0);

        editor.fileDragEnter(files, 12, 34);
        P.eq("drag.enter_paths", panel.enterPaths == want8, 1);
        P.near("drag.enter_x", panel.x, 12.0, 0.0);
        P.near("drag.enter_y", panel.y, 34.0, 0.0);
        editor.fileDragMove(files, 56, 78);
        P.near("drag.move_x", panel.x, 56.0, 0.0);
        P.near("drag.move_y", panel.y, 78.0, 0.0);
        editor.fileDragExit(files);
        // A second drag, dropped: JUCE sends no exit after a drop.
        editor.fileDragEnter(files, 1, 2);
        editor.fileDragMove(files, 3, 4);
        editor.filesDropped(files, 3, 4);
        P.eq("drag.drop_paths", panel.dropPaths == want8, 1);
        std::printf("INFO     calls %s\n", joined(panel.calls).c_str());
        P.eq("drag.sequence", joined(panel.calls) == "enter,move,exit,enter,move,drop", 1);
    }

    // ---- a Panel written before v0.7.1 ------------------------------------------------------------------------------
    {
        auto owned = std::make_unique<PlainPanel>();
        PlainPanel& panel = *owned;
        funkgui::EditorHost editor(processor, funkgui::EditorConfig{}, std::move(owned));
        const juce::StringArray files("/tmp/a.preset");
        P.eq("plain_panel.interest_default", editor.isInterestedInFileDrag(files), 0);
        editor.fileDragEnter(files, 5, 6);           // not sent by JUCE after a refusal; harmless all the same
        editor.fileDragMove(files, 7, 8);
        editor.fileDragExit(files);
        editor.filesDropped(files, 7, 8);
        P.eq("plain_panel.survives_drag_calls", 1, 1);
        P.eq("plain_panel.owner_is_editor",
             panel.host != nullptr && panel.host->ownerComponent() == static_cast<juce::Component*>(&editor), 1);
    }

    prefs.setTheme(0);
    return P.finish();
}

// FUNKGUI_TEST name=fg.editorhost.zoom timeout=300 gpu=1
//
// fg.editorhost.zoom: EditorHost's UI zoom (G7c, v0.8.0; FCompressor ADR-68; gpu/EditorHost.h "Zoom",
// panel/HostServices.h zoomPercent / setZoomPercent / zoomSteps, gpu/A11yBridge.h setScale, panel/CaptureConfig.h
// UI_ZOOM). EditorHosts that are never put on screen (no peer, no surface): the zoom is decided and applied without
// a window, and FramePump::tick() runs each editor's frame step 1 (preferences and zoom) before its showing gate.
//   empty.*       no zoom steps (HardwareReverb's config): the editor is exactly the Panel's size, zoomPercent() 100,
//                 no steps, setZoomPercent ignored, and input reaches the Panel bit for bit unchanged
//   config.*      steps cleaned (25..400, sorted, unique); an unlisted default becomes the nearest step
//   size.<z>      per step: zoomPercent() answers at once, the next frame sizes the editor round(W z) x round(H z) and
//                 Diagnostics::zoomPercent reports it
//   pointer.<z>   editor (x z, y z) reaches the Panel at (x, y) exactly: move, down, drag, up, double click, wheel (its
//                 deltas untouched), file-drag enter and move
//   a11y.<z>      A11yBridge children at the item's rectangle times z (smallest integer container), a nested child
//                 relative to its parent's; a scale change re-places them at once; items() stay logical
//   persist.*     a choice is written to the sandbox preferences file (FUNKGUI_PREFS_DIR) as <VALUE name="..."
//                 val="150"/>, and a new editor opens at it
//   invalid.*     an unlisted value, a damaged value and a missing key all open at the default; an unlisted
//                 setZoomPercent writes nothing
//   follow.*      a second editor follows the first's choice on its next frame (the preferences revision)
//   nokey.*       no zoomPrefKey: the choice is this editor's alone, nothing is written
//   fit.*         a window wider than the user area of the display under the pointer (the editor has no peer) is drawn
//                 at the largest step that fits while the preference keeps the choice; none fits: the smallest step
//   pin.*         UI_ZOOM pins the zoom (setZoomPercent still writes the preference); CANVAS_DUMP runs at 100 %
//                 unless UI_ZOOM is set; UI_ZOOM applies without steps too
//   capture.*     CaptureConfig::fromEnv parses UI_ZOOM (25..400, whole percent)
// A window-backed run (the render view, the drawable, the frame's dpi, the host window and the bridge's children on a
// real peer) is fg.editorhost.zoom.live. Spec rows only.

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Env.h>
#include <funkgui/core/Theme.h>
#include <funkgui/gpu/A11yBridge.h>
#include <funkgui/gpu/EditorHost.h>
#include <funkgui/gpu/FramePump.h>
#include <funkgui/panel/CaptureConfig.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Panel.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/test/Harness.h>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace T = funkgui::test;

namespace
{
    constexpr const char* kKey = "fgZoomTest";
    const std::vector<int> kSteps{ 100, 125, 150, 175 };

    // The processor an AudioProcessorEditor needs: no audio, no state.
    class NoOpProcessor final : public juce::AudioProcessor
    {
    public:
        NoOpProcessor()
            : juce::AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
        {
        }

        const juce::String getName() const override { return "fg.editorhost.zoom"; }
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

    // A fixed-size Panel that records the last position of every kind of input it receives.
    class ProbePanel final : public funkgui::Panel
    {
    public:
        ProbePanel(int w, int h) : w_(w), h_(h) {}

        funkgui::HostServices* host = nullptr;
        funkgui::PointerEvent move, down, drag, up, dbl;
        funkgui::WheelEvent wheelEv;
        float dragX = -1.0f, dragY = -1.0f;
        int   events = 0;

        void attach(funkgui::HostServices& h) override { host = &h; }
        int  width() const override { return w_; }
        int  height() const override { return h_; }
        void tick(float) override {}
        void draw(funkgui::Canvas&, const funkgui::Theme&) override {}
        bool wantsFullRate() const override { return false; }
        void pointerMove(const funkgui::PointerEvent& e) override { move = e; ++events; }
        void pointerDown(const funkgui::PointerEvent& e) override { down = e; ++events; }
        void pointerDrag(const funkgui::PointerEvent& e) override { drag = e; ++events; }
        void pointerUp(const funkgui::PointerEvent& e) override { up = e; ++events; }
        void doubleClick(const funkgui::PointerEvent& e) override { dbl = e; ++events; }
        bool wheel(const funkgui::WheelEvent& e) override
        {
            wheelEv = e;
            ++events;
            return true;
        }
        bool filesInterest(const std::vector<std::string>&) const override { return true; }
        void filesDragEnter(const std::vector<std::string>&, float x, float y) override
        {
            dragX = x;
            dragY = y;
        }
        void filesDragMove(float x, float y) override
        {
            dragX = x;
            dragY = y;
        }
        void accessibility(std::vector<funkgui::A11yItem>&) const override {}
        uint32_t a11yRevision() const override { return 0; }
        void a11yAction(uint32_t, funkgui::A11yAction, double) override {}
        void closeGestures() override {}

    private:
        int w_, h_;
    };

    // An editor over a ProbePanel, the Panel kept typed.
    struct Rig
    {
        ProbePanel* panel = nullptr;
        std::unique_ptr<funkgui::EditorHost> editor;

        Rig(juce::AudioProcessor& p, int w, int h, funkgui::EditorConfig config)
        {
            auto owned = std::make_unique<ProbePanel>(w, h);
            panel = owned.get();
            editor = std::make_unique<funkgui::EditorHost>(p, std::move(config), std::move(owned));
        }
        funkgui::HostServices& host() const { return *panel->host; }
    };

    funkgui::EditorConfig zoomConfig(std::vector<int> steps = kSteps, int def = 125, const char* key = kKey)
    {
        funkgui::EditorConfig c;
        c.zoomSteps = std::move(steps);
        c.defaultZoomPercent = def;
        c.zoomPrefKey = key;
        return c;
    }

    double stamp = 0.0;

    // One frame of every open editor, as the display link would give it (step 1 runs even for an editor not showing).
    void pumpFrame()
    {
        stamp += 1.0 / 60.0;
        funkgui::FramePump::get().tick(stamp);
    }

    int zoomed(int logical, int percent) { return static_cast<int>(std::lround(logical * percent / 100.0)); }

    bool sameBits(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }

    juce::MouseEvent mouseAt(juce::Component& c, float x, float y, int clicks = 1)
    {
        const auto now = juce::Time::getCurrentTime();
        return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), { x, y }, juce::ModifierKeys(),
                                0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, { x, y }, now, clicks, false);
    }

    // The preference as the sandbox file holds it on disk (not the in-memory copy): the <VALUE> element's val.
    juce::String fileValue(const char* key)
    {
        const auto xml = juce::XmlDocument::parse(funkgui::UiPreferences::get().file());
        if (xml == nullptr)
            return "<no file>";
        for (auto* e : xml->getChildIterator())
            if (e->hasTagName("VALUE") && e->getStringAttribute("name") == key)
                return e->getStringAttribute("val");
        return "<missing>";
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

    // The editor's size and zoom rows for one expected percent.
    void sizeRows(T::Probe& P, const std::string& k, const Rig& r, int w, int h, int percent)
    {
        P.eq(k + ".width", r.editor->getWidth(), zoomed(w, percent));
        P.eq(k + ".height", r.editor->getHeight(), zoomed(h, percent));
        P.eq(k + ".diag_zoom", r.editor->diagnostics().zoomPercent, percent);
        P.eq(k + ".zoom_percent", r.host().zoomPercent(), percent);
    }
}

int main(int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;          // NSApplication and the message thread, as in a host
    T::Probe P("fg.editorhost.zoom", "", argc, argv);

    // This test owns its capture environment: no pin unless a row sets one.
    setEnvVar("UI_ZOOM", nullptr);
    setEnvVar("CANVAS_DUMP", nullptr);

    auto& prefs = funkgui::UiPreferences::get();
    const juce::String prefsPath = prefs.file().getFullPathName();
    std::printf("INFO     preferences file %s\n", prefsPath.toRawUTF8());
    const char* sandbox = funkgui::env("PREFS_DIR");
    P.eq("persist.sandboxed", sandbox != nullptr && prefsPath.startsWith(juce::String::fromUTF8(sandbox)), 1);
    prefs.setInt(kKey, 125);                         // the sandbox may hold the last run's choice
    stamp = juce::Time::getMillisecondCounterHiRes() * 0.001;
    NoOpProcessor processor;

    // ---- empty: no zoom steps, exactly v0.7 ------------------------------------------------------------------------
    {
        Rig r(processor, 200, 120, funkgui::EditorConfig{});
        P.eq("empty.width", r.editor->getWidth(), 200);
        P.eq("empty.height", r.editor->getHeight(), 120);
        P.eq("empty.zoom_percent", r.host().zoomPercent(), 100);
        P.eq("empty.steps", static_cast<int64_t>(r.host().zoomSteps().size()), 0);
        P.eq("empty.diag_zoom", r.editor->diagnostics().zoomPercent, 100);
        r.host().setZoomPercent(150);
        pumpFrame();
        P.eq("empty.set_ignored", r.host().zoomPercent(), 100);
        P.eq("empty.width_after_set", r.editor->getWidth(), 200);
        P.eq("empty.height_after_set", r.editor->getHeight(), 120);
        const float x = 37.3f, y = 11.7f;            // not representable exactly: the bits must pass through untouched
        r.editor->mouseDown(mouseAt(*r.editor, x, y));
        P.eq("empty.pointer_x_bits", sameBits(r.panel->down.x, x), 1);
        P.eq("empty.pointer_y_bits", sameBits(r.panel->down.y, y), 1);
        r.editor->mouseUp(mouseAt(*r.editor, x, y));
        r.editor->mouseWheelMove(mouseAt(*r.editor, x, y), juce::MouseWheelDetails{ 0.0f, 0.25f, false, true, false });
        P.eq("empty.wheel_x_bits", sameBits(r.panel->wheelEv.x, x), 1);
        P.eq("empty.wheel_y_bits", sameBits(r.panel->wheelEv.y, y), 1);
        const juce::StringArray files("/tmp/a.preset");
        r.editor->fileDragEnter(files, 57, 33);
        P.eq("empty.drag_x", sameBits(r.panel->dragX, 57.0f), 1);
        P.eq("empty.drag_y", sameBits(r.panel->dragY, 33.0f), 1);
    }

    // ---- config: steps cleaned, default made a step -----------------------------------------------------------------
    {
        Rig r(processor, 200, 120, zoomConfig({ 150, 100, 100, 500, 20, 125 }, 130, nullptr));
        const auto s = r.host().zoomSteps();
        const std::vector<int> got(s.begin(), s.end());
        P.eq("config.steps_cleaned", got == std::vector<int>{ 100, 125, 150 }, 1);
        P.eq("config.default_nearest", r.host().zoomPercent(), 125);
        Rig tie(processor, 200, 120, zoomConfig({ 100, 120 }, 110, nullptr));
        P.eq("config.default_tie_smaller", tie.host().zoomPercent(), 100);
    }

    // ---- size and pointer per step ----------------------------------------------------------------------------------
    {
        Rig r(processor, 240, 160, zoomConfig());
        sizeRows(P, "size.open", r, 240, 160, 125);   // the preference holds 125
        for (const int z : kSteps)
        {
            const std::string k = std::to_string(z);
            r.host().setZoomPercent(z);
            P.eq("size." + k + ".answered_at_once", r.host().zoomPercent(), z);
            pumpFrame();
            sizeRows(P, "size." + k, r, 240, 160, z);

            // (x, y) in the Panel's px, sent at (x z, y z) in the editor's: every product is exact at these steps.
            const float s = static_cast<float>(z) / 100.0f;
            const float x = 37.5f, y = 12.25f, x2 = 239.0f, y2 = 0.5f;
            const std::string p = "pointer." + k;
            r.editor->mouseMove(mouseAt(*r.editor, x * s, y * s));
            P.eq(p + ".move", sameBits(r.panel->move.x, x) && sameBits(r.panel->move.y, y), 1);
            r.editor->mouseDown(mouseAt(*r.editor, x * s, y * s));
            P.eq(p + ".down", sameBits(r.panel->down.x, x) && sameBits(r.panel->down.y, y), 1);
            r.editor->mouseDrag(mouseAt(*r.editor, x2 * s, y2 * s));
            P.eq(p + ".drag", sameBits(r.panel->drag.x, x2) && sameBits(r.panel->drag.y, y2), 1);
            r.editor->mouseUp(mouseAt(*r.editor, x2 * s, y2 * s));
            P.eq(p + ".up", sameBits(r.panel->up.x, x2) && sameBits(r.panel->up.y, y2), 1);
            r.editor->mouseDoubleClick(mouseAt(*r.editor, x * s, y * s, 2));
            P.eq(p + ".double_click", sameBits(r.panel->dbl.x, x) && sameBits(r.panel->dbl.y, y), 1);
            P.eq(p + ".double_click_clicks", r.panel->dbl.clicks, 2);
            r.editor->mouseWheelMove(mouseAt(*r.editor, x * s, y * s),
                                     juce::MouseWheelDetails{ 0.125f, -0.5f, false, true, false });
            P.eq(p + ".wheel", sameBits(r.panel->wheelEv.x, x) && sameBits(r.panel->wheelEv.y, y), 1);
            P.eq(p + ".wheel_deltas", sameBits(r.panel->wheelEv.dx, 0.125f) && sameBits(r.panel->wheelEv.dy, -0.5f),
                 1);
            // File drags arrive as whole editor px: 40 x 24 logical is whole at every step.
            const juce::StringArray files("/tmp/a.preset");
            r.editor->fileDragEnter(files, zoomed(40, z), zoomed(24, z));
            P.eq(p + ".file_drag_enter", sameBits(r.panel->dragX, 40.0f) && sameBits(r.panel->dragY, 24.0f), 1);
            r.editor->fileDragMove(files, zoomed(200, z), zoomed(8, z));
            P.eq(p + ".file_drag_move", sameBits(r.panel->dragX, 200.0f) && sameBits(r.panel->dragY, 8.0f), 1);
            r.editor->fileDragExit(files);
        }
    }

    // ---- a11y: the bridge's children in editor px --------------------------------------------------------------------
    {
        ProbePanel panel(240, 160);
        juce::Component editor;
        editor.setSize(420, 280);
        std::vector<funkgui::A11yItem> items(3);
        items[0].id = 1;
        items[0].role = funkgui::A11yRole::slider;
        items[0].title = "Threshold";
        items[0].bounds = { 10.5f, 20.25f, 100.0f, 16.0f };
        items[1].id = 2;
        items[1].role = funkgui::A11yRole::radioGroup;
        items[1].title = "Zoom";
        items[1].bounds = { 60.0f, 100.0f, 120.0f, 16.0f };
        items[2].id = 3;
        items[2].parent = 2;
        items[2].role = funkgui::A11yRole::radioButton;
        items[2].title = "150 percent";
        items[2].bounds = { 90.0f, 100.0f, 30.0f, 16.0f };
        const auto want = [](const funkgui::Rect& r, float s) {
            return juce::Rectangle<float>(r.x * s, r.y * s, r.w * s, r.h * s).getSmallestIntegerContainer();
        };
        const auto rectText = [](juce::Rectangle<int> b) { return b.toString().toStdString(); };

        funkgui::A11yBridge bridge;
        P.near("a11y.default_scale", bridge.scale(), 1.0, 0.0);
        bridge.sync(editor, items, panel);
        for (const int z : kSteps)
        {
            const float s = static_cast<float>(z) / 100.0f;
            const std::string k = "a11y." + std::to_string(z);
            bridge.setScale(s);                      // re-places the children at once
            auto* slider = bridge.componentFor(1);
            auto* group = bridge.componentFor(2);
            auto* radio = bridge.componentFor(3);
            if (!P.eq(k + ".children", slider != nullptr && group != nullptr && radio != nullptr, 1))
                break;
            std::printf("INFO     %s: slider %s, group %s, radio %s (in the group)\n", k.c_str(),
                        rectText(slider->getBounds()).c_str(), rectText(group->getBounds()).c_str(),
                        rectText(radio->getBounds()).c_str());
            P.eq(k + ".slider", slider->getBounds() == want(items[0].bounds, s), 1);
            P.eq(k + ".group", group->getBounds() == want(items[1].bounds, s), 1);
            P.eq(k + ".radio_nested", radio->getParentComponent() == group, 1);
            P.eq(k + ".radio_relative",
                 radio->getBounds() == want(items[2].bounds, s) - want(items[1].bounds, s).getPosition(), 1);
            bridge.sync(editor, items, panel);       // an unchanged sync keeps the scale
            P.eq(k + ".sync_keeps_scale", slider->getBounds() == want(items[0].bounds, s), 1);
            P.eq(k + ".items_logical", bridge.items().size() == 3 && bridge.items()[0].bounds.x == 10.5f, 1);
        }
        bridge.setScale(0.0f);                       // not a scale: 1
        P.near("a11y.invalid_scale_is_one", bridge.scale(), 1.0, 0.0);
        bridge.clear();
    }

    // ---- persist, invalid, follow -----------------------------------------------------------------------------------
    {
        prefs.setInt(kKey, 125);
        {
            Rig a(processor, 240, 160, zoomConfig());
            a.host().setZoomPercent(150);
            P.eq("persist.memory", prefs.getInt(kKey, 0, 0, 1000), 150);
            P.eq("persist.file", fileValue(kKey) == "150", 1);
            std::printf("INFO     %s holds %s = %s\n", prefsPath.toRawUTF8(), kKey, fileValue(kKey).toRawUTF8());
        }
        {
            Rig b(processor, 240, 160, zoomConfig());
            sizeRows(P, "persist.reopened", b, 240, 160, 150);
        }

        prefs.setInt(kKey, 130);                     // not a step
        {
            Rig r(processor, 240, 160, zoomConfig());
            sizeRows(P, "invalid.unlisted", r, 240, 160, 125);
            r.host().setZoomPercent(130);            // not offered: nothing written, nothing moves
            r.host().setZoomPercent(0);
            pumpFrame();
            P.eq("invalid.set_unlisted_ignored", r.host().zoomPercent(), 125);
            P.eq("invalid.set_unlisted_no_write", fileValue(kKey) == "130", 1);
        }
        {
            // A damaged file: the value is not an integer (written on disk; the editor re-reads the file when opened).
            const juce::File f = prefs.file();
            const juce::String xml = f.loadFileAsString().replace("name=\"" + juce::String(kKey) + "\" val=\"130\"",
                                                                  "name=\"" + juce::String(kKey) + "\" val=\"1x5\"");
            const bool written = f.replaceWithText(xml);
            P.eq("invalid.damaged_written", written && fileValue(kKey) == "1x5", 1);
            Rig r(processor, 240, 160, zoomConfig());
            sizeRows(P, "invalid.damaged", r, 240, 160, 125);
        }
        {
            Rig r(processor, 240, 160, zoomConfig({ 100, 125, 150, 175 }, 150, "fgZoomNeverWritten"));
            sizeRows(P, "invalid.missing_key", r, 240, 160, 150);
        }

        prefs.setInt(kKey, 100);
        {
            Rig a(processor, 240, 160, zoomConfig());
            Rig b(processor, 240, 160, zoomConfig());
            sizeRows(P, "follow.a_open", a, 240, 160, 100);
            sizeRows(P, "follow.b_open", b, 240, 160, 100);
            a.host().setZoomPercent(175);
            P.eq("follow.a_answers", a.host().zoomPercent(), 175);
            P.eq("follow.b_before_frame", b.editor->getWidth(), 240);
            pumpFrame();
            sizeRows(P, "follow.a", a, 240, 160, 175);
            sizeRows(P, "follow.b", b, 240, 160, 175);
            b.host().setZoomPercent(125);
            pumpFrame();
            sizeRows(P, "follow.back_a", a, 240, 160, 125);
            sizeRows(P, "follow.back_b", b, 240, 160, 125);
        }
    }

    // ---- nokey: not persisted, this editor only ---------------------------------------------------------------------
    {
        prefs.setInt(kKey, 100);
        Rig a(processor, 240, 160, zoomConfig(kSteps, 125, nullptr));
        Rig b(processor, 240, 160, zoomConfig(kSteps, 125, nullptr));
        sizeRows(P, "nokey.open", a, 240, 160, 125);
        a.host().setZoomPercent(175);
        pumpFrame();
        sizeRows(P, "nokey.a", a, 240, 160, 175);
        sizeRows(P, "nokey.b_unchanged", b, 240, 160, 125);
        P.eq("nokey.pref_untouched", fileValue(kKey) == "100", 1);
    }

    // ---- fit: the display under the pointer (no peer) ---------------------------------------------------------------
    {
        const auto& displays = juce::Desktop::getInstance().getDisplays();
        const auto* d = displays.getDisplayForPoint(juce::Desktop::getMousePosition());
        if (d == nullptr)
            d = displays.getPrimaryDisplay();
        if (P.eq("fit.display_known", d != nullptr && !d->userArea.isEmpty(), 1))
        {
            const auto area = d->userArea;
            std::printf("INFO     fit: user area %d x %d\n", area.getWidth(), area.getHeight());
            // 125 % fits (0.89 of the width), 150 % does not (1.07).
            const int w = static_cast<int>(std::floor(area.getWidth() / 1.4));
            const int h = 100;
            prefs.setInt(kKey, 175);
            {
                Rig r(processor, w, h, zoomConfig());
                sizeRows(P, "fit.open", r, w, h, 125);
                P.eq("fit.pref_kept", fileValue(kKey) == "175", 1);
                r.host().setZoomPercent(150);        // chosen, kept, and still fitted
                pumpFrame();
                sizeRows(P, "fit.set_150", r, w, h, 125);
                P.eq("fit.set_150_pref", fileValue(kKey) == "150", 1);
                r.host().setZoomPercent(100);        // a step below the fit is taken as chosen
                pumpFrame();
                sizeRows(P, "fit.set_100", r, w, h, 100);
            }
            // Height-bound: 150 % fits the height (0.94 of it), 175 % does not (1.09).
            const int h2 = static_cast<int>(std::floor(area.getHeight() / 1.6));
            prefs.setInt(kKey, 175);
            {
                Rig r(processor, 200, h2, zoomConfig());
                sizeRows(P, "fit.height", r, 200, h2, 150);
            }
            // Nothing fits: the smallest step.
            {
                Rig r(processor, area.getWidth() + 10, 100, zoomConfig());
                sizeRows(P, "fit.none_fits", r, area.getWidth() + 10, 100, 100);
            }
            // Without steps nothing is fitted: v0.7 sizes a window larger than the display as asked.
            {
                Rig r(processor, area.getWidth() + 10, 100, funkgui::EditorConfig{});
                P.eq("fit.no_steps_unfitted", r.editor->getWidth(), area.getWidth() + 10);
            }
        }
    }

    // ---- pins: UI_ZOOM and CANVAS_DUMP ------------------------------------------------------------------------------
    {
        prefs.setInt(kKey, 175);
        setEnvVar("UI_ZOOM", "150");
        {
            Rig r(processor, 240, 160, zoomConfig());
            P.eq("pin.ui_zoom_capture", r.editor->capture().uiZoom, 150);
            sizeRows(P, "pin.ui_zoom", r, 240, 160, 150);
            r.host().setZoomPercent(125);            // written, as a theme cell writes under UI_THEME; the pin holds
            pumpFrame();
            sizeRows(P, "pin.ui_zoom_after_set", r, 240, 160, 150);
            P.eq("pin.ui_zoom_set_written", fileValue(kKey) == "125", 1);
        }
        {
            Rig r(processor, 240, 160, funkgui::EditorConfig{});
            sizeRows(P, "pin.ui_zoom_no_steps", r, 240, 160, 150);
        }
        {
            // Pins are not fitted: a capture is the same on every display.
            const auto* d = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
            const int w = d != nullptr ? d->userArea.getWidth() : 2000;
            Rig r(processor, w, 100, zoomConfig());
            sizeRows(P, "pin.ui_zoom_unfitted", r, w, 100, 150);
        }
        const std::string dump = (juce::File::getCurrentWorkingDirectory().getChildFile("zoom-never.dump"))
                                     .getFullPathName()
                                     .toStdString();
        setEnvVar("CANVAS_DUMP", dump.c_str());
        {
            Rig r(processor, 240, 160, zoomConfig());
            sizeRows(P, "pin.canvas_dump_ui_zoom", r, 240, 160, 150);
        }
        setEnvVar("UI_ZOOM", nullptr);
        prefs.setInt(kKey, 175);
        {
            Rig r(processor, 240, 160, zoomConfig());
            P.eq("pin.canvas_dump_capture", r.editor->capture().canvasDump == dump, 1);
            sizeRows(P, "pin.canvas_dump", r, 240, 160, 100);
        }
        setEnvVar("CANVAS_DUMP", nullptr);
    }

    // ---- capture: UI_ZOOM parsing -----------------------------------------------------------------------------------
    {
        const struct { const char* v; int want; } cases[] = {
            { "150", 150 }, { "25", 25 }, { "400", 400 }, { "24", 0 }, { "401", 0 }, { "1x5", 0 }, { " 150", 0 },
            { "", 0 },      { "-100", 0 },
        };
        int i = 0;
        for (const auto& c : cases)
        {
            setEnvVar("UI_ZOOM", c.v);
            P.eq("capture.ui_zoom." + std::to_string(i++), funkgui::CaptureConfig::fromEnv().uiZoom, c.want);
        }
        setEnvVar("UI_ZOOM", nullptr);
        P.eq("capture.ui_zoom.unset", funkgui::CaptureConfig::fromEnv().uiZoom, 0);
    }

    prefs.setInt(kKey, 125);
    return P.finish();
}

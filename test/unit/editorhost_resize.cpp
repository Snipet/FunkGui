// FUNKGUI_TEST name=fg.editorhost.resize timeout=120 gpu=1
//
// fg.editorhost.resize (0.18.0): EditorHost with a resizable Panel (gpu/EditorHost.h "Resizing", panel/Panel.h
// "Resizing"). EditorHosts that are never put on screen (no peer, no surface), as in fg.editorhost.zoom: a window
// resize reaches the editor as setSize(), as an AU or VST3 wrapper or JUCE's corner resizer would deliver it.
//   fixed.*    a fixed Panel: the editor is not resizable, has no corner resizer, and a setSize() reaches no Panel
//   open.*     a resizable Panel: the editor opens at the Panel's size, is resizable with the corner resizer, and its
//              limits are the Panel's minimum up to EditorConfig::maxWidth x maxHeight (no maximum: 16384)
//   set.*      setSize(w, h) gives the Panel resize(w, h) at once and EditorConfig::sizeChanged(w, h); a size below
//              the minimum (setSize does not apply the limits) reaches the Panel raised to it; an equal size does
//              nothing
//   zoom.*     a zoom change resizes the editor round(W z) x round(H z) and keeps the logical size (no resize(), no
//              sizeChanged), with the limits times z; a window resize at that zoom gives the Panel the size / z, and
//              input reaches it divided by z
//   max.*      EditorConfig::maxWidth x maxHeight times the zoom are the maximum

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Env.h>
#include <funkgui/core/Theme.h>
#include <funkgui/gpu/EditorHost.h>
#include <funkgui/gpu/FramePump.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Panel.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/test/Harness.h>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace T = funkgui::test;

namespace
{
    class NoOpProcessor final : public juce::AudioProcessor
    {
    public:
        NoOpProcessor()
            : juce::AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
        {
        }

        const juce::String getName() const override { return "fg.editorhost.resize"; }
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

    class SizedPanel final : public funkgui::Panel
    {
    public:
        SizedPanel(int w, int h, bool resizable, int minW, int minH)
            : w_(w), h_(h), resizable_(resizable), minW_(minW), minH_(minH)
        {
        }

        funkgui::HostServices* host = nullptr;
        funkgui::PointerEvent down;
        int resizes = 0;

        void attach(funkgui::HostServices& h) override { host = &h; }
        int  width() const override { return w_; }
        int  height() const override { return h_; }
        bool resizable() const override { return resizable_; }
        int  minWidth() const override { return minW_; }
        int  minHeight() const override { return minH_; }
        void resize(int w, int h) override
        {
            w_ = w;
            h_ = h;
            ++resizes;
        }
        void tick(float) override {}
        void draw(funkgui::Canvas&, const funkgui::Theme&) override {}
        bool wantsFullRate() const override { return false; }
        void pointerDown(const funkgui::PointerEvent& e) override { down = e; }
        void accessibility(std::vector<funkgui::A11yItem>&) const override {}
        uint32_t a11yRevision() const override { return 0; }
        void a11yAction(uint32_t, funkgui::A11yAction, double) override {}
        void closeGestures() override {}

    private:
        int  w_, h_;
        bool resizable_;
        int  minW_, minH_;
    };

    struct Rig
    {
        SizedPanel* panel = nullptr;
        std::unique_ptr<funkgui::EditorHost> editor;
        int reports = 0, lastW = 0, lastH = 0;

        Rig(juce::AudioProcessor& p, bool resizable, funkgui::EditorConfig config)
        {
            config.sizeChanged = [this](int w, int h) {
                ++reports;
                lastW = w;
                lastH = h;
            };
            auto owned = std::make_unique<SizedPanel>(300, 200, resizable, 240, 150);
            panel = owned.get();
            editor = std::make_unique<funkgui::EditorHost>(p, std::move(config), std::move(owned));
        }
        funkgui::HostServices& host() const { return *panel->host; }
    };

    double stamp = 0.0;

    void pumpFrame()
    {
        stamp += 1.0 / 60.0;
        funkgui::FramePump::get().tick(stamp);
    }

    juce::MouseEvent mouseAt(juce::Component& c, float x, float y)
    {
        const auto now = juce::Time::getCurrentTime();
        return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), { x, y }, juce::ModifierKeys(),
                                0.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, { x, y }, now, 1, false);
    }

    bool sameBits(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }

    void limitRows(T::Probe& P, const std::string& k, const Rig& r, int minW, int minH, int maxW, int maxH)
    {
        auto* c = r.editor->getConstrainer();
        P.eq(k + ".has_limits", c != nullptr ? 1 : 0, 1);
        if (c == nullptr)
            return;
        P.eq(k + ".min_width", c->getMinimumWidth(), minW);
        P.eq(k + ".min_height", c->getMinimumHeight(), minH);
        P.eq(k + ".max_width", c->getMaximumWidth(), maxW);
        P.eq(k + ".max_height", c->getMaximumHeight(), maxH);
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
    const juce::ScopedJuceInitialiser_GUI juceInit;
    T::Probe P("fg.editorhost.resize", "", argc, argv);
    setEnvVar("UI_ZOOM", nullptr);
    setEnvVar("CANVAS_DUMP", nullptr);
    stamp = juce::Time::getMillisecondCounterHiRes() * 0.001;
    NoOpProcessor processor;

    // ---- fixed ----------------------------------------------------------------------------------------------------
    {
        Rig r(processor, false, funkgui::EditorConfig{});
        P.eq("fixed.not_resizable", r.editor->isResizable() ? 1 : 0, 0);
        P.eq("fixed.no_corner", r.editor->resizableCorner != nullptr ? 1 : 0, 0);
        r.editor->setSize(500, 400);
        P.eq("fixed.no_resize", r.panel->resizes, 0);
        P.eq("fixed.no_report", r.reports, 0);
        P.eq("fixed.panel_width", r.panel->width(), 300);
    }

    // ---- open, set ------------------------------------------------------------------------------------------------
    {
        Rig r(processor, true, funkgui::EditorConfig{});
        P.eq("open.width", r.editor->getWidth(), 300);
        P.eq("open.height", r.editor->getHeight(), 200);
        P.eq("open.resizable", r.editor->isResizable() ? 1 : 0, 1);
        P.eq("open.corner", r.editor->resizableCorner != nullptr ? 1 : 0, 1);
        P.eq("open.no_resize", r.panel->resizes, 0);
        limitRows(P, "open", r, 240, 150, 16384, 16384);

        r.editor->setSize(520, 330);
        P.eq("set.panel_width", r.panel->width(), 520);
        P.eq("set.panel_height", r.panel->height(), 330);
        P.eq("set.once", r.panel->resizes, 1);
        P.eq("set.reported", r.reports == 1 && r.lastW == 520 && r.lastH == 330 ? 1 : 0, 1);
        r.editor->setSize(100, 90);                  // setSize applies no limits: the Panel gets its minimum
        P.eq("set.min_width", r.panel->width(), 240);
        P.eq("set.min_height", r.panel->height(), 150);
        r.editor->setSize(520, 330);
        const int before = r.panel->resizes;
        r.editor->setSize(520, 330);
        P.eq("set.equal_nothing", r.panel->resizes, before);
    }

    // ---- zoom, max -------------------------------------------------------------------------------------------------
    {
        funkgui::EditorConfig config;
        config.zoomSteps = { 100, 150 };
        config.defaultZoomPercent = 100;
        config.maxWidth = 800;
        config.maxHeight = 500;
        Rig r(processor, true, std::move(config));
        limitRows(P, "max.at_100", r, 240, 150, 800, 500);
        r.editor->setSize(420, 260);
        P.eq("zoom.start", r.panel->width() == 420 && r.panel->height() == 260 ? 1 : 0, 1);
        const int resizes = r.panel->resizes, reports = r.reports;
        r.host().setZoomPercent(150);
        pumpFrame();
        P.eq("zoom.editor_width", r.editor->getWidth(), 630);
        P.eq("zoom.editor_height", r.editor->getHeight(), 390);
        P.eq("zoom.logical_kept", r.panel->width() == 420 && r.panel->height() == 260 ? 1 : 0, 1);
        P.eq("zoom.no_resize", r.panel->resizes, resizes);
        P.eq("zoom.no_report", r.reports, reports);
        limitRows(P, "zoom.limits", r, 360, 225, 1200, 750);
        r.editor->setSize(750, 450);                 // the user's resize at 150 %
        P.eq("zoom.resize_width", r.panel->width(), 500);
        P.eq("zoom.resize_height", r.panel->height(), 300);
        P.eq("zoom.reported", r.lastW == 500 && r.lastH == 300 ? 1 : 0, 1);
        const float x = 437.5f, y = 212.25f;          // exact at 150 %
        r.editor->mouseDown(mouseAt(*r.editor, x * 1.5f, y * 1.5f));
        P.eq("zoom.pointer", sameBits(r.panel->down.x, x) && sameBits(r.panel->down.y, y), 1);
        r.editor->mouseUp(mouseAt(*r.editor, x * 1.5f, y * 1.5f));
        r.host().setZoomPercent(100);
        pumpFrame();
        P.eq("zoom.back_width", r.editor->getWidth(), 500);
        P.eq("zoom.back_logical", r.panel->width() == 500 && r.panel->height() == 300 ? 1 : 0, 1);
    }
    return P.finish();
}

// FunkGuiGalleryApp: FunkGui's gallery Standalone (G7; FCompressor docs/design/02-funkgui-and-ui.md §1.1, §3.10, §5.1;
// SPRINTS.md S8.3; K3 #19). A no-op AudioProcessor whose editor is an EditorHost running one GalleryPanel section live
// on the GPU: the same Panel GalleryProbe runs headless, so tools/capture-frame.sh can capture a frame of the app and
// fg.gallery.live can compare it with HeadlessHost's frame of the same section and state.
//
//   FunkGuiGalleryApp.app/Contents/MacOS/FunkGuiGalleryApp [--section <name>]
//
// - The section: --section, else FUNKGUI_GALLERY_SECTION, else "primitives" (else the first registered one). An
//   unknown name lists the sections on stderr and exits 2. The Section menu reopens the window on another section
//   (the editor's size is fixed per section, EditorHost's rule).
// - FUNKGUI_GALLERY_TRANSIENT_VB_BYTES=<n> (>= 1024) configures BgfxContext's transient vertex buffer before the first
//   editor opens, so a test can force BgfxSink's overflow path (02 §4.5) on a real GPU.
// - Everything else is EditorHost's capture environment (FUNKGUI_CANVAS_DUMP, _UI_KEYS, _UI_FIXED_DT, ...; 02 §5.1).
// - "Standalone" here is a plain app bundle, not a JUCE plug-in Standalone: the processor is a no-op, so no audio
//   device is opened, no microphone permission is asked for and no Standalone settings file is written (C §6.7).

#include "../../test/gallery/GalleryPanel.h"

#include <funkgui/core/Env.h>
#include <funkgui/gpu/BgfxContext.h>
#include <funkgui/gpu/EditorHost.h>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace
{
    namespace G = funkgui::gallery;

    constexpr const char* kDefaultSection = "primitives";

    // The processor an AudioProcessorEditor needs: no audio, no state, one editor per window.
    class NoOpProcessor final : public juce::AudioProcessor
    {
    public:
        NoOpProcessor()
            : juce::AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
        {
        }

        void show(const G::SectionInfo& s) { section_ = &s; }

        const juce::String getName() const override { return "FunkGuiGallery"; }
        void prepareToPlay(double, int) override {}
        void releaseResources() override {}
        void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override { buffer.clear(); }
        double getTailLengthSeconds() const override { return 0.0; }
        bool acceptsMidi() const override { return false; }
        bool producesMidi() const override { return false; }
        bool hasEditor() const override { return true; }
        int getNumPrograms() override { return 1; }
        int getCurrentProgram() override { return 0; }
        void setCurrentProgram(int) override {}
        const juce::String getProgramName(int) override { return {}; }
        void changeProgramName(int, const juce::String&) override {}
        void getStateInformation(juce::MemoryBlock&) override {}
        void setStateInformation(const void*, int) override {}

        // An EditorHost on the current section's GalleryPanel.
        juce::AudioProcessorEditor* createEditor() override
        {
            if (section_ == nullptr)
                return nullptr;
            funkgui::EditorConfig config;
            config.fallbackTitle = "FUNKGUI GALLERY";
            config.zoomSteps = { 100, 125, 150, 175 };   // G7c: the "zoom" section's demo (FCompressor's steps)
            config.zoomPrefKey = "uiZoom";               // default 100: the gallery opens as before until a click
            return new funkgui::EditorHost(*this, std::move(config), std::make_unique<G::GalleryPanel>(*section_));
        }

    private:
        const G::SectionInfo* section_ = nullptr;
    };

    class GalleryWindow final : public juce::DocumentWindow
    {
    public:
        GalleryWindow(NoOpProcessor& processor, const G::SectionInfo& info)
            : juce::DocumentWindow("FunkGui Gallery: " + juce::String(info.name), juce::Colours::black,
                                   juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton)
        {
            processor.show(info);
            setUsingNativeTitleBar(true);
            setContentOwned(processor.createEditor(), true);
            setResizable(false, false);
            centreWithSize(getWidth(), getHeight());
            setVisible(true);
        }

        void closeButtonPressed() override
        {
            if (auto* app = juce::JUCEApplication::getInstance())
                app->systemRequestedQuit();
        }
    };

    const G::SectionInfo* sectionNamed(const juce::String& name)
    {
        return G::findSection(name.toStdString());
    }

    class GalleryApplication final : public juce::JUCEApplication, private juce::MenuBarModel
    {
    public:
        const juce::String getApplicationName() override { return "FunkGuiGallery"; }
        const juce::String getApplicationVersion() override { return "1"; }
        bool moreThanOneInstanceAllowed() override { return true; }

        void initialise(const juce::String& commandLine) override
        {
            juce::String wanted;
            const auto args = juce::StringArray::fromTokens(commandLine, true);
            for (int i = 0; i + 1 < args.size(); ++i)
                if (args[i] == "--section")
                    wanted = args[i + 1].unquoted();
            if (wanted.isEmpty())
                if (const char* e = funkgui::env("GALLERY_SECTION"))
                    wanted = juce::String::fromUTF8(e);
            if (wanted.isEmpty())
                wanted = G::findSection(kDefaultSection) != nullptr || G::sections().empty()
                           ? juce::String(kDefaultSection)
                           : juce::String(G::sections().front().name);

            const G::SectionInfo* info = sectionNamed(wanted);
            if (info == nullptr)
            {
                std::fprintf(stderr, "FunkGuiGalleryApp: no gallery section '%s'; registered:", wanted.toRawUTF8());
                for (const G::SectionInfo& s : G::sections())
                    std::fprintf(stderr, " %s", s.name.c_str());
                std::fprintf(stderr, "\n");
                setApplicationReturnValue(2);
                quit();
                return;
            }

            if (const char* e = funkgui::env("GALLERY_TRANSIENT_VB_BYTES"))
            {
                char* end = nullptr;
                const unsigned long long bytes = std::strtoull(e, &end, 10);
                if (end != e && *end == '\0' && bytes >= 1024u && bytes <= 0xffffffffu)
                {
                    funkgui::BgfxContext::Config c = funkgui::BgfxContext::get().config();
                    c.transientVbBytes = static_cast<uint32_t>(bytes);
                    funkgui::BgfxContext::get().configure(c);
                }
            }

            processor_ = std::make_unique<NoOpProcessor>();
            open(*info);
            juce::MenuBarModel::setMacMainMenu(this);
        }

        void shutdown() override
        {
            juce::MenuBarModel::setMacMainMenu(nullptr);
            window_.reset();                         // the editor goes before its processor
            processor_.reset();
        }

        void systemRequestedQuit() override { quit(); }
        void anotherInstanceStarted(const juce::String&) override {}

    private:
        void open(const G::SectionInfo& info)
        {
            window_.reset();
            current_ = &info;
            window_ = std::make_unique<GalleryWindow>(*processor_, info);
            menuItemsChanged();
        }

        // juce::MenuBarModel: one menu listing the sections; the current one is ticked.
        juce::StringArray getMenuBarNames() override { return { "Section" }; }

        juce::PopupMenu getMenuForIndex(int, const juce::String&) override
        {
            juce::PopupMenu menu;
            int id = 1;
            for (const G::SectionInfo& s : G::sections())
                menu.addItem(id++, juce::String(s.name), true, &s == current_);
            return menu;
        }

        void menuItemSelected(int id, int) override
        {
            const auto& all = G::sections();
            if (id < 1 || id > static_cast<int>(all.size()) || processor_ == nullptr)
                return;
            const G::SectionInfo* next = &all[static_cast<size_t>(id - 1)];
            // Not from inside the menu's own callback: the window (and its editor) is replaced.
            juce::MessageManager::callAsync([this, next] {
                if (processor_ != nullptr)
                    open(*next);
            });
        }

        std::unique_ptr<NoOpProcessor> processor_;
        std::unique_ptr<GalleryWindow> window_;
        const G::SectionInfo* current_ = nullptr;
    };
}

START_JUCE_APPLICATION(GalleryApplication)

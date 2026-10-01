#include <funkgui/panel/HeadlessGuiScope.h>

#include <juce_events/juce_events.h>

// HeadlessGuiScope with JUCE (v0.12.0): the juce::ScopedJuceInitialiser_GUI a headless process needs before the atlas
// bakes or the preferences file opens. Compiled only with FUNKGUI_WITH_JUCE (src/juce/**);
// src/nojuce/HeadlessGuiScope.cpp is its counterpart.

namespace funkgui
{
    HeadlessGuiScope::HeadlessGuiScope()
        : juce_(new juce::ScopedJuceInitialiser_GUI())
    {
    }

    HeadlessGuiScope::~HeadlessGuiScope()
    {
        delete static_cast<juce::ScopedJuceInitialiser_GUI*>(juce_);
    }
}

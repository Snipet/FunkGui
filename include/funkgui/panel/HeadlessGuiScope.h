#pragma once

// What a headless process holds before it draws (v0.12.0): a test, a probe or a command-line tool that uses
// FontService, HeadlessHost or UiPreferences with no plug-in host around it.
//
// With JUCE (FUNKGUI_HAS_JUCE) it holds a juce::ScopedJuceInitialiser_GUI for its lifetime, which is what such a
// process declared itself until now: the atlas bakes through JUCE's font stack and the preferences are a
// juce::PropertiesFile, and both need JUCE's GUI side initialised (a plug-in host has always done that already).
// Without JUCE there is nothing to initialise and it holds nothing. Either way a program writes
//
//   int main(int argc, char** argv)
//   {
//       const funkgui::HeadlessGuiScope gui;
//       ...
//   }
//
// and includes no JUCE header for it, so the same source builds in both configurations. Scopes nest, as JUCE's
// initialiser does. The thread that makes the first one is the message thread.

namespace funkgui
{
    class HeadlessGuiScope
    {
    public:
        HeadlessGuiScope();
        ~HeadlessGuiScope();

        HeadlessGuiScope(const HeadlessGuiScope&) = delete;
        HeadlessGuiScope& operator=(const HeadlessGuiScope&) = delete;

    private:
        void* juce_ = nullptr;                           // the juce::ScopedJuceInitialiser_GUI, when there is JUCE
    };
}

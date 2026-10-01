#include <funkgui/panel/HeadlessGuiScope.h>

// HeadlessGuiScope without JUCE (v0.12.0): nothing to initialise. Compiled only with FUNKGUI_WITH_JUCE=OFF
// (src/nojuce/**); src/juce/HeadlessGuiScope.cpp holds JUCE's GUI initialiser.

namespace funkgui
{
    HeadlessGuiScope::HeadlessGuiScope()
    {
        static_cast<void>(juce_);                        // the member exists for the JUCE build's layout
    }

    HeadlessGuiScope::~HeadlessGuiScope() = default;
}

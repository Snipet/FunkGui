#include <funkgui/canvas/SoftRaster.h>

// funkgui::writePng without JUCE (v0.12.0): the PNG encoder is JUCE's (src/juce/WritePng.cpp), so this build cannot
// write one and reports it the way writePng reports any file it could not write: false. HeadlessHost::writePng then
// returns false too. rasterise() is unaffected. Compiled only with FUNKGUI_WITH_JUCE=OFF (src/nojuce/**).

namespace funkgui
{
    bool writePng(const Image&, const char*)
    {
        return false;
    }
}

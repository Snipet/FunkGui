#include <funkgui/text/LineEdit.h>

#include <juce_core/juce_core.h>

// text::printable's juce::String overload (v0.12.0: moved here from src/text/LineEdit.cpp, unchanged). Compiled only
// with FUNKGUI_WITH_JUCE (src/juce/**); the std::string overloads and LineEdit itself are JUCE-free.

namespace funkgui::text
{
    juce::String printable(const juce::String& s)
    {
        juce::String out;
        out.preallocateBytes(s.getNumBytesAsUTF8() + 1);
        for (auto p = s.getCharPointer(); !p.isEmpty(); ++p)
        {
            const juce::juce_wchar c = *p;
            out += c >= 32 && c <= 126 ? juce::String::charToString(c) : juce::String("?");
        }
        return out;
    }
}

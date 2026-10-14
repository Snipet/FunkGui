// The closed hand and the not-allowed sign on Linux (../Cursors.h; FSynth G1). JUCE 8.0.4 reaches no native
// closed-hand or not-allowed cursor on X11: its standard types are the whole list (mouse/juce_MouseCursor.h:56-79),
// and the ones it draws itself there are the hand and the arrow with a plus
// (native/juce_XWindowSystem_linux.cpp:2406-2458). So both are standard cursors, the stated fallback:
//
// - grabbing: DraggingHandCursor, the hand Cursor::grab already shows, so the pointer keeps its shape on press;
// - notAllowed: NormalCursor, the arrow. A refusal is therefore not shown by the pointer on Linux: the Panel shows
//   it in what it draws (panel/Input.h, Cursor).
//
// JUCE only, no X11 header. A standard juce::MouseCursor is shared by JUCE itself, so making one costs nothing.

#include "../Cursors.h"

namespace funkgui
{
    juce::MouseCursor makeGrabbingCursor()
    {
        return juce::MouseCursor(juce::MouseCursor::DraggingHandCursor);
    }

    juce::MouseCursor makeNotAllowedCursor()
    {
        return juce::MouseCursor(juce::MouseCursor::NormalCursor);
    }
}

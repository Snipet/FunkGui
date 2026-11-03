// The closed hand and the not-allowed sign on Windows (../Cursors.h; v0.24.0), as on Linux: JUCE 8.0.4's standard
// cursor types are the whole list it offers (mouse/juce_MouseCursor.h:56-79), so both are standard cursors, the stated
// fallback:
//
// - grabbing: DraggingHandCursor, the hand Cursor::grab already shows, so the pointer keeps its shape on press;
// - notAllowed: NormalCursor, the arrow. A refusal is therefore not shown by the pointer on Windows: the Panel shows it
//   in what it draws (panel/Input.h, Cursor).
//
// JUCE only, no Windows header. A standard juce::MouseCursor is shared by JUCE itself, so making one costs nothing.

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

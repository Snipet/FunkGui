#pragma once

// The two Panel cursors JUCE 8.0.4 has no standard type for (FSynth G1): Cursor::grabbing, the closed hand of a drag
// in progress, and Cursor::notAllowed, a drop that is refused (juce_gui_basics/mouse/juce_MouseCursor.h:56-79 lists
// neither). Cursor::grab and Cursor::copy are JUCE's DraggingHandCursor and CopyingCursor on every platform and need
// nothing from here.
//
// One definition of the two functions per platform, so EditorHost.cpp keeps no platform #if:
// - macOS (Cursors.mm): the system's own [NSCursor closedHandCursor] and [NSCursor operationNotAllowedCursor], as
//   juce::MouseCursors made from their images at 2x and their hot spots.
// - Linux (linux/Cursors.cpp): JUCE reaches no native closed-hand or not-allowed cursor there, so they are standard
//   cursors: DraggingHandCursor for grabbing (the pointer keeps the open hand of grab on press) and NormalCursor for
//   notAllowed. A product must therefore show a refusal in what it draws too; the cursor is never the only cue.
//
// Each call MAKES a cursor. EditorHost asks through EditorCursors below, which makes each one once and is shared by
// every editor of the process: the first editor to need a cursor makes it, and the last editor to go destroys both,
// so no cursor is made per frame or per editor, and none outlives JUCE's GUI in a static.
//
// Private to src/gpu. Message thread only.

#include <juce_gui_basics/juce_gui_basics.h>

#include <optional>

namespace funkgui
{
    juce::MouseCursor makeGrabbingCursor();
    juce::MouseCursor makeNotAllowedCursor();

    // The cursors, made on first use. Held by every EditorHost through a juce::SharedResourcePointer, so one instance
    // serves the process for as long as any editor lives.
    struct SharedCursors
    {
        const juce::MouseCursor& grabbing()
        {
            if (!grabbing_)
                grabbing_ = makeGrabbingCursor();
            return *grabbing_;
        }

        const juce::MouseCursor& notAllowed()
        {
            if (!notAllowed_)
                notAllowed_ = makeNotAllowedCursor();
            return *notAllowed_;
        }

    private:
        std::optional<juce::MouseCursor> grabbing_, notAllowed_;
    };

    // What one EditorHost holds: its share of the process's cursors.
    struct EditorCursors
    {
        juce::SharedResourcePointer<SharedCursors> shared;
    };
}

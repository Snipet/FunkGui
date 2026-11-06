#pragma once

// The GPU-free UI unit (02 §3.5): layout, input, drawing and accessibility for one window, fixed-size unless the Panel
// is resizable (below). Not a
// juce::Component and no GPU: EditorHost runs it live, HeadlessHost runs it in a console probe with a fixed dt. A Panel
// follows the determinism rules of 02 §3.7: no wall clock (time comes only from tick(dt) and
// HostServices::nowSeconds()), every ease through funkgui::ease, draw() reads state and never advances it, no getenv
// and no preferences file access in tick() or draw().

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/panel/Input.h>

#include <cstdint>
#include <string>
#include <vector>

namespace funkgui
{
    class Canvas;
    class HostServices;
    struct Theme;

    class Panel
    {
    public:
        virtual ~Panel() = default;

        virtual void  attach(HostServices&) = 0;         // called once by the host before the first tick
        virtual int   width() const = 0;                 // the logical size (fixed unless resizable(), below)
        virtual int   height() const = 0;
        virtual void  tick(float dt) = 0;                // every eased value; seconds, never frames
        virtual void  idle(double /*nowSec*/) {}         // >= 10 Hz even while the display link is stopped: closes
                                                         // idle wheel gestures (HR BgfxEditor.h:119-124 lesson)
        virtual void  draw(Canvas&, const Theme&) = 0;
        virtual bool  wantsFullRate() const = 0;
        virtual void  pointerMove(const PointerEvent&) {}
        virtual void  pointerExit() {}
        virtual void  pointerDown(const PointerEvent&) {}
        virtual void  pointerDrag(const PointerEvent&) {}
        virtual void  pointerUp(const PointerEvent&) {}
        virtual void  doubleClick(const PointerEvent&) {}
        virtual bool  wheel(const WheelEvent&) { return false; }
        virtual bool  key(const KeyEvent&) { return false; }       // true = consumed (Logic/Live must not swallow it)
        virtual Cursor cursor() const { return Cursor::normal; }
        virtual void  accessibility(std::vector<A11yItem>&) const = 0;
        virtual uint32_t a11yRevision() const = 0;       // bumps when items appear/disappear/move
        virtual void  a11yAction(uint32_t id, A11yAction, double value = 0) = 0;
        virtual void  closeGestures() = 0;               // host closes the editor mid-gesture (HR dtor rule)
        virtual bool  filesInterest(const std::vector<std::string>&) const { return false; }
        virtual void  filesDropped(const std::vector<std::string>&) {}

        // A file drag over the window (G7b additions, v0.7.1; defaults ignore it). Only a drag whose files
        // filesInterest() accepted arrives: filesDragEnter once, filesDragMove while it moves, then either
        // filesDragExit (it left, or was cancelled) or filesDropped (no exit follows a drop; the last move's position
        // is the drop position). Positions are the Panel's logical px. EditorHost nudges the frame pump to full rate
        // after each, so a drop-zone highlight eases at once.
        virtual void  filesDragEnter(const std::vector<std::string>&, float /*x*/, float /*y*/) {}
        virtual void  filesDragMove(float /*x*/, float /*y*/) {}
        virtual void  filesDragExit() {}

        // Resizing (0.18.0; the defaults keep a Panel fixed). A Panel whose resizable() is true lays out in any
        // logical size from minWidth() x minHeight() up: the host calls resize(w, h) before the first frame drawn at a
        // new size, and width() and height() answer w and h from then on. resize() runs on the host's thread (the
        // message thread under EditorHost) between frames, never during tick() or draw(), and is called only with a
        // size at or above the minimum and different from the current one. EditorHost lets the user drag the window
        // (gpu/EditorHost.h "Resizing"); HeadlessHost::resize drives it in a probe. WebHost does not resize: there a
        // resizable Panel keeps the size it opened at.
        virtual bool  resizable() const { return false; }
        virtual int   minWidth() const { return width(); }
        virtual int   minHeight() const { return height(); }
        virtual void  resize(int /*w*/, int /*h*/) {}

        // Unchanged frames (0.25.0; the default draws every frame, as before). EditorHost asks needsDraw() after each
        // tick(): false says that draw() would record what it recorded the last time it was called, because nothing
        // it draws has changed since, and the host then neither records, nor uploads, nor submits that frame, so the
        // last picture stays up and an idle editor costs no GPU time. The answer covers the Panel's own state only:
        // the host still draws when it has a reason of its own (the first frame, a new size, backing scale, zoom or
        // theme, a surface made again, a window shown again or uncovered, a picture or shader replaced in its
        // registry, a capture; gpu/EditorHost.h "Unchanged frames"), and may call draw() whatever the answer, so
        // draw() stays correct at any time. A Panel typically sets a flag wherever its picture changes (input, a
        // value, an ease still moving, data from the processor) and clears it in draw(); a custom shader that
        // animates with u_viewSize.w (time) needs true while it should move. EditorHost also skips the submit of a
        // frame whose recording equals the last one drawn (canvas/FrameCompare.h), whatever needsDraw() answered.
        // HeadlessHost and WebHost draw whenever they are asked to, as before. Called on the host's thread, after
        // tick() and never during draw().
        virtual bool  needsDraw() const { return true; }
    };
}

#pragma once

// The GPU-free UI unit (02 §3.5): layout, input, drawing and accessibility for one fixed-size window. Not a
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
        virtual int   width() const = 0;                 // fixed logical size
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
    };
}

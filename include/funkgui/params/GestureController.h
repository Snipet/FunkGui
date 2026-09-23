#pragma once

// The begin/set/end discipline of A §3.5 in one place per Panel (02 §5.2). Every write a widget makes goes through it:
// a drag is one gesture from pointer down to up, a discrete write (click, key, a11y, double-click) is one
// begin/set/end triple and nothing at all when the value would not change, a wheel burst is one gesture closed after
// kWheelIdle seconds without a notch, and a composite write is one bracket per parameter inside one host batch
// (K2 #23). Gestures only: there is no undo manager (K2 #7). Ports are references owned elsewhere (K2 #27): the
// controller stores pointers only while a gesture is open, and closeAll() (also run by the destructor) ends them.
//
// Rules beyond 02 §5.2 (G2's choices; fg.gesture pins them):
// - A port never has two gestures open (JUCE asserts on a nested beginChangeGesture): a tap or wheel write to the port
//   being dragged is written inside the drag's gesture; a drag or tap on the port of an open wheel burst ends the burst
//   first; a new beginDrag ends the previous drag.
// - Values are clamped to 0..1 (-0 counts as 0). A NaN value writes nothing.
// - "Unchanged" is a bit compare (ease::sameBits): for tap and wheel against the port's value01(), for dragTo against
//   the last value this drag wrote (initially the port's value at beginDrag), so a parameter that quantises what it
//   stores is not rewritten on every pointer event.
// - A wheel notch that would not change the value writes nothing and does not extend the burst (HR's wheel handler
//   returns before restarting its idle timer).
// - poll(now) ends the burst once now - (time of its last write) >= kWheelIdle.
//
// Message thread only.

#include <funkgui/panel/HostServices.h>
#include <funkgui/params/ParamPort.h>

#include <span>
#include <utility>

namespace funkgui
{
    class GestureController
    {
    public:
        explicit GestureController(HostServices&);
        ~GestureController();                            // closeAll()

        GestureController(const GestureController&) = delete;
        GestureController& operator=(const GestureController&) = delete;

        void beginDrag(ParamPort&);                      // beginGesture + host.setUnboundedDrag(true)
        void dragTo(float host01);                       // clamp; write only when the bits change (ease::sameBits)
        void endDrag();                                  // endGesture + setUnboundedDrag(false); no-op if not dragging
        bool dragging() const noexcept;
        ParamPort* dragParam() const noexcept;           // nullptr when not dragging

        void tap(ParamPort&, float host01);              // begin/set/end; nothing at all if unchanged (HR :1712-1720)

        // Composite writes (a Mode switch with its defaults, 02 §8.4.4): one begin/set/end per changed parameter, all
        // inside one host.beginBatch()/endBatch() bracket (K2 #23), so the audio thread never resolves a half-written
        // set. Pairs with a null port are skipped. When no pair would change its parameter, nothing happens at all
        // (no batch either: endBatch raises an engine snap).
        void tapMany(std::span<const std::pair<ParamPort*, float>>);

        void wheelTo(ParamPort&, float host01, double nowSec);         // one gesture per burst (HR :1805-1848)
        void poll(double nowSec);                        // ends the wheel gesture kWheelIdle after its last write
        bool wheeling() const noexcept;                  // a wheel burst is open
        ParamPort* wheelParam() const noexcept;          // nullptr when no burst is open

        void closeAll();                                 // drag + wheel; the host closing the editor (HR :163-175)

        // The host this controller writes through, for a widget that opens its parameter's host menu on a popup click
        // (G2 addition: widgets whose pointerDown takes only a GestureController reach showParamMenu through it).
        HostServices& host() const noexcept;

        static constexpr double kWheelIdle = 0.5;        // seconds; HR kWheelIdleMs

    private:
        void endWheel();

        HostServices& host_;
        ParamPort*    drag_ = nullptr;
        float         dragLast_ = 0.0f;                  // the last value this drag wrote (or found at beginDrag)
        ParamPort*    wheel_ = nullptr;
        double        wheelLast_ = 0.0;                  // nowSec of the burst's last write
    };
}

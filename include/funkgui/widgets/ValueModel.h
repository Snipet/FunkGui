#pragma once

// The five-state value contract a product implements per slot (02 §5.3, §8.1–§8.2) and RuleSlider renders.
// FCompressor's SlotModel builds a ValueView from its resolver (02 §9.4). The view is plain data in fixed buffers, so
// building it allocates nothing (A §3.1), and a widget re-reads it only when key() changes, so formatting runs only on
// change.

#include <funkgui/params/GestureController.h>
#include <funkgui/params/ParamPort.h>

#include <cstdint>

namespace funkgui
{
    enum class ValueState : uint8_t { continuous, stepped, locked, derived, na };

    struct Detent
    {
        float       host01 = 0.0f;         // canonical host-normalised value this detent writes (exact)
        const char* label = "";            // drawn: "4", "ALL", ".3", "COMP"
        const char* spoken = "";           // a11y: "4 to 1", "all buttons", "0.3 milliseconds"
    };

    struct ValueText                        // no heap per frame (A §3.1)
    {
        char value[24] = {};               // never contains the slot label (K1 #15)
        char unit[8] = {};
        char sub[40] = {};
        char spoken[64] = {};
    };

    struct ValueView
    {
        ValueState  state = ValueState::continuous;
        const char* label  = "";           // Mode term ("PEAK RED.")
        const char* aka    = nullptr;      // universal label when remapped ("THRESHOLD")
        const char* tag    = nullptr;      // "FIXED", "= RATIO", "AUTO", "+" (extension)
        const char* reason = nullptr;      // REQUIRED for locked/derived/na: footer + a11y help
        float track = 0, trackDefault = 0;                         // 0..1 on the drawn track
        bool  bipolar = false, atDefault = false, clamped = false, live = false;
        int   nDetents = 0, detent = -1;  const Detent* detents = nullptr;   // stepped, index space
        int   nNotches = 0;  const float* notches = nullptr;       // soft notches, track 0..1
        int   nEndLo = 0, nEndHi = 0;                      // hybrid: steps below lo / above hi (16 px end cells)
        const Detent* endLo = nullptr;  const Detent* endHi = nullptr;
        int   activeEnd = 0;                               // 0 = inside [lo,hi]; -k = k-th low cell; +k = k-th high
        ValueText text{};
    };

    class ValueModel
    {
    public:
        virtual ~ValueModel() = default;

        virtual uint64_t   key() const = 0;                     // changes whenever view() would; widgets re-read then
        virtual void       view(ValueView&) const = 0;
        virtual ParamPort* port() = 0;                          // nullptr: display-only
        virtual float      host01FromTrack(float t) const = 0;  // continuous: track -> host01 (Mode sub-range + skew)
        virtual float      defaultHost01() const = 0;           // the Mode default: double-click, Delete

        // Writes detent i: tap(port, detents[i].host01). Nothing when i is out of range, the model is display-only, or
        // the view's state is locked, derived or n/a (those never write, from any input path: 02 §8.4.5).
        virtual void writeDetent(int i, GestureController& g)
        {
            ValueView v;
            view(v);
            ParamPort* p = port();
            const bool writable = v.state == ValueState::continuous || v.state == ValueState::stepped;
            if (p != nullptr && writable && v.detents != nullptr && i >= 0 && i < v.nDetents)
                g.tap(*p, v.detents[i].host01);
        }

        // Absolute handle drags: plot value → host01. A product returns false when the plot value is not the
        // parameter's own value (then the handle drags relatively). FCompressor: threshold via T_in; knee/range only
        // with kFlagPlotIsPlain (02 §6.5, K1 #9).
        virtual bool plotToHost01(float /*plotValue*/, float& /*host01*/) const { return false; }
    };
}

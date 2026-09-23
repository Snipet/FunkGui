#include <funkgui/params/GestureController.h>

#include <funkgui/core/Ease.h>

#include <cmath>

namespace funkgui
{
    namespace
    {
        // The value a write stores: clamped to 0..1, with -0 as +0. NaN has no place on a parameter and writes nothing
        // (false).
        bool clamped01(float v, float& out) noexcept
        {
            if (std::isnan(v))
                return false;
            out = v <= 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            return true;
        }
    }

    GestureController::GestureController(HostServices& host) : host_(host) {}

    GestureController::~GestureController()
    {
        closeAll();
    }

    // ---- drag -------------------------------------------------------------------------------------------------------

    void GestureController::beginDrag(ParamPort& p)
    {
        endDrag();                                       // a new drag replaces the previous one
        if (wheel_ == &p)
            endWheel();                                  // one open gesture per port
        drag_ = &p;
        dragLast_ = p.value01();
        p.beginGesture();
        host_.setUnboundedDrag(true);
    }

    void GestureController::dragTo(float host01)
    {
        float v = 0.0f;
        if (drag_ == nullptr || !clamped01(host01, v) || ease::sameBits(v, dragLast_))
            return;
        dragLast_ = v;
        drag_->setValue01(v);
    }

    void GestureController::endDrag()
    {
        if (drag_ == nullptr)
            return;
        ParamPort* p = drag_;
        drag_ = nullptr;
        p->endGesture();
        host_.setUnboundedDrag(false);
    }

    bool GestureController::dragging() const noexcept { return drag_ != nullptr; }

    ParamPort* GestureController::dragParam() const noexcept { return drag_; }

    // ---- discrete writes --------------------------------------------------------------------------------------------

    void GestureController::tap(ParamPort& p, float host01)
    {
        float v = 0.0f;
        if (!clamped01(host01, v))
            return;
        if (&p == drag_)
        {
            // Inside the open drag gesture: the host already has a begin for this parameter.
            if (!ease::sameBits(v, p.value01()))
            {
                dragLast_ = v;
                p.setValue01(v);
            }
            return;
        }
        if (ease::sameBits(v, p.value01()))
            return;                                      // nothing at all if unchanged
        if (&p == wheel_)
            endWheel();
        p.beginGesture();
        p.setValue01(v);
        p.endGesture();
    }

    void GestureController::tapMany(std::span<const std::pair<ParamPort*, float>> writes)
    {
        bool anyChange = false;
        for (const auto& [port, value] : writes)
        {
            float v = 0.0f;
            if (port != nullptr && clamped01(value, v) && !ease::sameBits(v, port->value01()))
            {
                anyChange = true;
                break;
            }
        }
        if (!anyChange)
            return;

        host_.beginBatch();
        for (const auto& [port, value] : writes)
            if (port != nullptr)
                tap(*port, value);
        host_.endBatch();
    }

    // ---- wheel ------------------------------------------------------------------------------------------------------

    void GestureController::wheelTo(ParamPort& p, float host01, double nowSec)
    {
        float v = 0.0f;
        if (!clamped01(host01, v) || ease::sameBits(v, p.value01()))
            return;                                      // no write, and the burst is not extended
        if (&p == drag_)
        {
            dragLast_ = v;
            p.setValue01(v);                             // inside the open drag gesture
            return;
        }
        if (wheel_ != &p)
        {
            endWheel();
            wheel_ = &p;
            p.beginGesture();
        }
        wheelLast_ = nowSec;
        p.setValue01(v);
    }

    void GestureController::poll(double nowSec)
    {
        if (wheel_ != nullptr && nowSec - wheelLast_ >= kWheelIdle)
            endWheel();
    }

    bool GestureController::wheeling() const noexcept { return wheel_ != nullptr; }

    ParamPort* GestureController::wheelParam() const noexcept { return wheel_; }

    void GestureController::endWheel()
    {
        if (wheel_ == nullptr)
            return;
        ParamPort* p = wheel_;
        wheel_ = nullptr;
        p->endGesture();
    }

    // ---- lifetime ---------------------------------------------------------------------------------------------------

    void GestureController::closeAll()
    {
        endDrag();
        endWheel();
    }

    HostServices& GestureController::host() const noexcept { return host_; }
}

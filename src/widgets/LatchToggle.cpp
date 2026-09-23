#include <funkgui/widgets/LatchToggle.h>

#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Col.h>
#include <funkgui/core/Ease.h>
#include <funkgui/core/Theme.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/params/GestureController.h>
#include <funkgui/params/ParamPort.h>
#include <funkgui/widgets/FocusRing.h>

// LatchToggle (02 §5.5, §8.9, §8.10): HR's FREEZE latch (BgfxEditor.cpp:1209-1215 draw, :1803 arm, :1827 drag,
// :1854-1866 commit) over a ToggleModel, plus ParamToggle, the ToggleModel products use for a boolean parameter.
// Choices where 02 is silent are listed in the header.

namespace funkgui
{
    LatchToggle::LatchToggle(ToggleModel& model, Rect rect, const char* label, uint32_t a11yId)
        : model_(model), rect_(rect), label_(label != nullptr ? label : ""), a11yId_(a11yId)
    {
    }

    const char* LatchToggle::reason() const
    {
        return model_.enabled() ? nullptr : model_.reason();
    }

    void LatchToggle::tick(float dt, Point pointer)
    {
        hoverOn_ = contains(pointer) && model_.enabled();
        hover_ = ease::hover(hover_, hoverOn_, dt);
    }

    bool LatchToggle::settled() const
    {
        return ease::sameBits(hover_, hoverOn_ ? 1.0f : 0.0f);
    }

    bool LatchToggle::contains(Point p) const
    {
        return rect_.contains(p);
    }

    Cursor LatchToggle::cursorAt(Point p) const
    {
        return model_.enabled() && contains(p) ? Cursor::pointingHand : Cursor::normal;
    }

    void LatchToggle::draw(Canvas& c, const Theme& th, bool focusRing) const
    {
        const bool on = model_.on();
        Col fill, ink;
        if (armed_)
        {
            fill = th.ink32;                             // pressed: armed fill, the thing under the hand in accent
            ink = th.accent;
        }
        else if (!model_.enabled())
        {
            fill = on ? th.ink32 : th.ink16;             // refused: no hover, the footer gives the reason
            ink = on ? th.ground : th.ink32;
        }
        else if (on)
        {
            fill = th.ink70;
            ink = th.ground;
        }
        else
        {
            fill = th.ink16;
            ink = mix(th.ink52, th.ink100, hover_);
        }
        {
            const Canvas::Scope s(c, tags::latch, false);
            c.rrect(rect_.x, rect_.y, rect_.w, rect_.h, 0.0f, fill);
            c.text(label_, rect_.centreX(), c.capCentreTop(rect_.centreY(), type::kLatch), type::kLatch, ink,
                   Align::centre);
        }
        if (focusRing)
            drawFocusRing(c, rect_, th.accent);
    }

    void LatchToggle::pointerDown(const PointerEvent& e, GestureController& g)
    {
        armed_ = false;
        if (e.popup)
        {
            if (ParamPort* p = model_.port())
                g.host().showParamMenu(*p, e.x, e.y);    // the host menu, never a write
            return;
        }
        if (!model_.enabled())
            return;                                      // refused
        armed_ = contains({ e.x, e.y });
    }

    void LatchToggle::pointerDrag(const PointerEvent& e)
    {
        if (armed_ && !contains({ e.x, e.y }))
            armed_ = false;                              // leaving the latch disarms: drag-off cancels
    }

    void LatchToggle::pointerUp(const PointerEvent& e, GestureController& g)
    {
        const bool commit = armed_ && contains({ e.x, e.y }) && model_.enabled();
        armed_ = false;
        if (commit)
            model_.set(!model_.on(), g);
    }

    bool LatchToggle::key(const KeyEvent& e, GestureController& g)
    {
        if (e.key != Key::enter && e.key != Key::space)
            return false;
        if (model_.enabled())
            model_.set(!model_.on(), g);
        return true;                                     // disabled: refused, the footer shows the reason
    }

    bool LatchToggle::a11yAction(uint32_t id, A11yAction a, GestureController& g)
    {
        if (id != a11yId_)
            return false;
        switch (a)
        {
            case A11yAction::press:
            case A11yAction::toggle:
                if (model_.enabled())
                    model_.set(!model_.on(), g);
                break;
            case A11yAction::showMenu:
                if (ParamPort* p = model_.port())
                    g.host().showParamMenu(*p, rect_.centreX(), rect_.centreY());
                break;
            case A11yAction::setValue:
            case A11yAction::increment:
            case A11yAction::decrement:
            case A11yAction::focus:
                break;
        }
        return true;
    }

    void LatchToggle::accessibility(std::vector<A11yItem>& out) const
    {
        A11yItem item;
        item.id = a11yId_;
        item.role = A11yRole::toggleButton;
        item.bounds = rect_;
        item.title = label_;
        item.checkable = true;
        item.checked = model_.on();
        item.enabled = model_.enabled();
        if (const char* r = reason())
            item.help = r;
        out.push_back(item);
    }

    // ---- ParamToggle ------------------------------------------------------------------------------------------------

    ParamToggle::ParamToggle(ParamPort& port) : port_(port) {}

    bool ParamToggle::on() const
    {
        return port_.value01() >= 0.5f;
    }

    void ParamToggle::set(bool v, GestureController& g)
    {
        g.tap(port_, v ? 1.0f : 0.0f);                   // nothing at all when the port already holds it
    }

    ParamPort* ParamToggle::port()
    {
        return &port_;
    }
}

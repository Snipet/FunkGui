#include <funkgui/widgets/AttachedWord.h>

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

// AttachedWord (02 §5.5, §6.4; F §3.5): a kMicro word right-aligned in a slot's label row, toggled like HR's Freeze
// latch (arm on down, commit on up inside, leaving the word disarms; BgfxEditor.cpp:1724, 1774-1788). Every write is
// the model's set(), which a product implements with a GestureController tap (one begin/set/end triple).

namespace funkgui
{
    AttachedWord::AttachedWord(ToggleModel& model, const SlotGeom& geom, const char* word, uint32_t a11yId)
        : model_(model), geom_(geom), word_(word != nullptr ? word : ""), a11yId_(a11yId)
    {
    }

    AttachedWord::AttachedWord(WordModel& model, const SlotGeom& geom, const char* word, uint32_t a11yId)
        : AttachedWord(static_cast<ToggleModel&>(model), geom, word, a11yId)
    {
        wordModel_ = &model;
    }

    bool AttachedWord::visible() const
    {
        return wordModel_ == nullptr || wordModel_->visible();
    }

    const char* AttachedWord::reason() const
    {
        return model_.enabled() ? nullptr : model_.reason();
    }

    void AttachedWord::tick(float dt, bool hovered)
    {
        const bool shown = visible();
        if (!shown)
            armed_ = false;                              // a word that left the Mode cannot commit
        hoverOn_ = hovered && shown;
        hover_ = ease::hover(hover_, hoverOn_, dt);
    }

    bool AttachedWord::settled() const
    {
        return ease::sameBits(hover_, hoverOn_ ? 1.0f : 0.0f);
    }

    void AttachedWord::draw(Canvas& c, const Theme& th, bool focusRing) const
    {
        if (!visible())
            return;
        Col col = th.ink16;                              // disabled: refused, the footer gives the reason
        if (model_.enabled())
        {
            if (armed_)
                col = th.accent;                         // pressed: the thing under the hand
            else if (model_.on())
                col = th.ink100;
            else
                col = mix(th.ink32, th.ink70, hover_);
        }
        {
            const Canvas::Scope s(c, tags::word, false);
            c.text(word_, geom_.x + geom_.w, c.sharedBaselineTop(geom_.top, type::kLabel, type::kMicro), type::kMicro,
                   col, Align::right);
        }
        if (focusRing)
            drawFocusRing(c, hit(), th.accent);
    }

    Cursor AttachedWord::cursorAt(Point p) const
    {
        return visible() && model_.enabled() && contains(p) ? Cursor::pointingHand : Cursor::normal;
    }

    void AttachedWord::pointerDown(const PointerEvent& e, GestureController& g)
    {
        armed_ = false;
        if (!visible())
            return;
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

    void AttachedWord::pointerDrag(const PointerEvent& e)
    {
        if (armed_ && !contains({ e.x, e.y }))
            armed_ = false;                              // leaving the word disarms: drag-off cancels
    }

    void AttachedWord::pointerUp(const PointerEvent& e, GestureController& g)
    {
        const bool commit = armed_ && contains({ e.x, e.y }) && visible() && model_.enabled();
        armed_ = false;
        if (commit)
            model_.set(!model_.on(), g);
    }

    bool AttachedWord::key(const KeyEvent& e, GestureController& g)
    {
        if (!visible() || (e.key != Key::enter && e.key != Key::space))
            return false;
        if (model_.enabled())
            model_.set(!model_.on(), g);
        return true;                                     // disabled: refused, the footer shows the reason
    }

    bool AttachedWord::a11yAction(uint32_t id, A11yAction a, GestureController& g)
    {
        if (id != a11yId_)
            return false;
        if (!visible())
            return true;
        if (a == A11yAction::showMenu)
        {
            if (ParamPort* p = model_.port())
                g.host().showParamMenu(*p, hit().centreX(), hit().centreY());
        }
        else if ((a == A11yAction::press || a == A11yAction::toggle) && model_.enabled())
            model_.set(!model_.on(), g);
        return true;
    }

    void AttachedWord::accessibility(std::vector<A11yItem>& out) const
    {
        if (!visible())
            return;                                      // hidden: not in the tree
        A11yItem item;
        item.id = a11yId_;
        item.role = A11yRole::toggleButton;
        item.bounds = hit();
        item.title = word_;
        item.checkable = true;
        item.checked = model_.on();
        item.enabled = model_.enabled();
        if (const char* r = reason())
            item.help = r;
        out.push_back(item);
    }
}

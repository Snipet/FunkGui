#include <funkgui/widgets/SegmentedSelector.h>

#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Col.h>
#include <funkgui/core/Ease.h>
#include <funkgui/core/Theme.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/params/GestureController.h>
#include <funkgui/params/ParamPort.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/widgets/FocusRing.h>

#include <climits>
#include <cmath>
#include <utility>

// SegmentedSelector (02 §5.5, §8.9, §8.10): HR's Order and theme cells (BgfxEditor.cpp:1100-1131 draw, :1596-1601 hit,
// :1703-1722 click) as one widget over a CellModel, plus the CellModel implementations products share: ParamCells over
// a choice parameter and PrefCells over an integer preference. Choices where 02 is silent are listed in the header.

namespace funkgui
{
    namespace
    {
        constexpr float kRingGrow = 2.0f;                // the focus ring's margin round the union of the cells

        bool nonEmpty(const char* s) noexcept { return s != nullptr && s[0] != '\0'; }

        const char* orEmpty(const char* s) noexcept { return s != nullptr ? s : ""; }

        Rect unite(const Rect& a, const Rect& b) noexcept
        {
            const float x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
            const float x1 = a.right() > b.right() ? a.right() : b.right();
            const float y1 = a.bottom() > b.bottom() ? a.bottom() : b.bottom();
            return { x0, y0, x1 - x0, y1 - y0 };
        }
    }

    SegmentedSelector::SegmentedSelector(CellModel& model, std::vector<Rect> cells, CellStyle style,
                                         const char* caption, Point captionAt, uint32_t a11yId)
        : model_(model), cells_(std::move(cells)), style_(style), caption_(caption), captionAt_(captionAt),
          a11yId_(a11yId), hoverAmt_(cells_.size(), 0.0f)
    {
    }

    int SegmentedSelector::cells() const
    {
        const int n = model_.count();
        const int r = static_cast<int>(cells_.size());
        return n < 0 ? 0 : (n < r ? n : r);
    }

    Rect SegmentedSelector::bounds() const noexcept
    {
        if (cells_.empty())
            return {};
        Rect u = cells_.front();
        for (const Rect& r : cells_)
            u = unite(u, r);
        return u;
    }

    void SegmentedSelector::setSpokenTitle(const char* title)
    {
        spokenTitle_ = title;
    }

    // ---- state ------------------------------------------------------------------------------------------------------

    void SegmentedSelector::tick(float dt, Point pointer)
    {
        const int at = cellAt(pointer);
        hovered_ = at >= 0 && model_.enabled(at) ? at : -1;
        for (size_t i = 0; i < hoverAmt_.size(); ++i)
            hoverAmt_[i] = ease::hover(hoverAmt_[i], static_cast<int>(i) == hovered_, dt);
    }

    bool SegmentedSelector::settled() const
    {
        for (size_t i = 0; i < hoverAmt_.size(); ++i)
            if (!ease::sameBits(hoverAmt_[i], static_cast<int>(i) == hovered_ ? 1.0f : 0.0f))
                return false;
        return true;
    }

    int SegmentedSelector::cellAt(Point p) const
    {
        const int n = cells();
        for (int i = 0; i < n; ++i)
            if (cells_[static_cast<size_t>(i)].contains(p))
                return i;
        return -1;
    }

    bool SegmentedSelector::contains(Point p) const
    {
        return cellAt(p) >= 0;
    }

    Cursor SegmentedSelector::cursorAt(Point p) const
    {
        const int i = cellAt(p);
        return i >= 0 && model_.enabled(i) ? Cursor::pointingHand : Cursor::normal;
    }

    // ---- drawing ----------------------------------------------------------------------------------------------------

    void SegmentedSelector::draw(Canvas& c, const Theme& th, bool focusRing) const
    {
        const int n = cells();
        const int act = model_.active();
        {
            const Canvas::Scope s(c, tags::cell, false);
            if (nonEmpty(caption_))
                c.text(caption_, captionAt_.x, captionAt_.y, type::kCaption, th.ink52);
            for (int i = 0; i < n; ++i)
            {
                const Rect& r = cells_[static_cast<size_t>(i)];
                const bool on = i == act;
                const bool enabled = model_.enabled(i);
                const float h = hoverAmt_[static_cast<size_t>(i)];
                Col ink;
                if (style_ == CellStyle::boxed)
                {
                    Col fill = th.ink16;
                    if (on)
                    {
                        fill = th.ink70;
                        ink = th.ground;
                    }
                    else
                        ink = enabled ? mix(th.ink52, th.ink100, h) : th.ink32;
                    c.rrect(r.x, r.y, r.w, r.h, 0.0f, fill);
                }
                else
                    ink = on ? th.ink70 : (enabled ? mix(th.ink32, th.ink100, h) : th.ink16);
                c.text(orEmpty(model_.label(i)), r.centreX(), c.capCentreTop(r.centreY(), type::kCaption),
                       type::kCaption, ink, Align::centre);
            }
        }
        if (focusRing && n > 0)
            drawFocusRing(c, bounds().expanded(kRingGrow), th.accent);
    }

    // ---- input ------------------------------------------------------------------------------------------------------

    int SegmentedSelector::step(int from, int dir) const
    {
        const int n = cells();
        for (int i = from + dir; i >= 0 && i < n; i += dir)
            if (model_.enabled(i))
                return i;
        return -1;
    }

    void SegmentedSelector::selectCell(int i, GestureController& g)
    {
        if (i >= 0 && i < cells() && model_.enabled(i))
            model_.select(i, g);                         // tap semantics: the model writes nothing when unchanged
    }

    void SegmentedSelector::showMenu(float x, float y, GestureController& g)
    {
        if (ParamPort* p = model_.port())
            g.host().showParamMenu(*p, x, y);            // the host menu, never a write
    }

    void SegmentedSelector::pointerDown(const PointerEvent& e, GestureController& g)
    {
        const int i = cellAt({ e.x, e.y });
        if (i < 0)
            return;
        if (e.popup)
        {
            showMenu(e.x, e.y, g);
            return;
        }
        selectCell(i, g);                                // a disabled cell refuses
    }

    bool SegmentedSelector::key(const KeyEvent& e, GestureController& g)
    {
        const int n = cells();
        int act = model_.active();
        if (act < 0 || act >= n)
            act = -1;
        switch (e.key)
        {
            case Key::right:
            case Key::up:
                selectCell(step(act, 1), g);
                return true;
            case Key::left:
            case Key::down:
                selectCell(step(act < 0 ? n : act, -1), g);
                return true;
            case Key::home:
                selectCell(step(-1, 1), g);
                return true;
            case Key::end:
                selectCell(step(n, -1), g);
                return true;
            case Key::enter:
            case Key::space:
                return true;                             // the arrows already selected
            case Key::character:
            case Key::tab:
            case Key::pageUp:
            case Key::pageDown:
            case Key::escape:
            case Key::backspace:
            case Key::del:
                return false;
        }
        return false;
    }

    bool SegmentedSelector::a11yAction(uint32_t id, A11yAction a, double value, GestureController& g)
    {
        const int n = cells();
        if (id == a11yId_)
        {
            const int act = model_.active();
            switch (a)
            {
                case A11yAction::increment:
                    selectCell(step(act >= 0 && act < n ? act : -1, 1), g);
                    break;
                case A11yAction::decrement:
                    selectCell(step(act >= 0 && act < n ? act : n, -1), g);
                    break;
                case A11yAction::setValue:
                    if (std::isfinite(value))
                        selectCell(static_cast<int>(std::lround(value)), g);
                    break;
                case A11yAction::showMenu:
                {
                    const Rect b = bounds();
                    showMenu(b.centreX(), b.centreY(), g);
                    break;
                }
                case A11yAction::press:
                case A11yAction::toggle:
                case A11yAction::focus:
                    break;
            }
            return true;
        }
        if (id <= a11yId_ || id - a11yId_ - 1 >= static_cast<uint32_t>(n))
            return false;
        const int i = static_cast<int>(id - a11yId_ - 1);
        switch (a)
        {
            case A11yAction::press:
            case A11yAction::toggle:
            case A11yAction::setValue:
                selectCell(i, g);
                break;
            case A11yAction::showMenu:
            {
                const Rect& r = cells_[static_cast<size_t>(i)];
                showMenu(r.centreX(), r.centreY(), g);
                break;
            }
            case A11yAction::increment:
            case A11yAction::decrement:
            case A11yAction::focus:
                break;
        }
        return true;
    }

    void SegmentedSelector::accessibility(std::vector<A11yItem>& out) const
    {
        const int n = cells();
        const int act = model_.active();
        const auto spokenOf = [this](int i) -> const char* {
            const char* s = model_.spoken(i);
            return nonEmpty(s) ? s : orEmpty(model_.label(i));
        };

        A11yItem group;
        group.id = a11yId_;
        group.role = A11yRole::radioGroup;
        group.bounds = bounds();
        group.title = nonEmpty(spokenTitle_) ? spokenTitle_ : orEmpty(caption_);
        if (act >= 0 && act < n)
            group.value = spokenOf(act);
        out.push_back(group);

        for (int i = 0; i < n; ++i)
        {
            A11yItem item;
            item.id = a11yId_ + 1 + static_cast<uint32_t>(i);
            item.parent = a11yId_;
            item.role = A11yRole::radioButton;
            item.bounds = cells_[static_cast<size_t>(i)];
            item.title = spokenOf(i);
            item.help = orEmpty(model_.help(i));
            item.checkable = true;
            item.checked = i == act;
            item.enabled = model_.enabled(i);
            out.push_back(item);
        }
    }

    // ---- ParamCells -------------------------------------------------------------------------------------------------

    ParamCells::ParamCells(ParamPort& port, std::vector<CellText> texts) : port_(port), texts_(std::move(texts)) {}

    int ParamCells::count() const
    {
        return static_cast<int>(texts_.size());
    }

    float ParamCells::host01(int i) const noexcept
    {
        const int n = static_cast<int>(texts_.size());
        if (n < 2 || i <= 0)
            return 0.0f;
        if (i >= n - 1)
            return 1.0f;
        return static_cast<float>(i) / static_cast<float>(n - 1);
    }

    int ParamCells::active() const
    {
        const int n = static_cast<int>(texts_.size());
        if (n < 2)
            return n - 1;                                // 0 for one cell, -1 for none
        const float v = port_.value01();
        if (!(v > 0.0f))
            return 0;                                    // also NaN
        if (v >= 1.0f)
            return n - 1;
        return static_cast<int>(std::lround(v * static_cast<float>(n - 1)));
    }

    const char* ParamCells::label(int i) const
    {
        return i >= 0 && i < count() ? orEmpty(texts_[static_cast<size_t>(i)].label) : "";
    }

    const char* ParamCells::spoken(int i) const
    {
        if (i < 0 || i >= count())
            return "";
        const CellText& t = texts_[static_cast<size_t>(i)];
        return nonEmpty(t.spoken) ? t.spoken : orEmpty(t.label);
    }

    const char* ParamCells::help(int i) const
    {
        return i >= 0 && i < count() ? texts_[static_cast<size_t>(i)].help : nullptr;
    }

    void ParamCells::select(int i, GestureController& g)
    {
        if (i >= 0 && i < count())
            g.tap(port_, host01(i));                     // nothing at all when the port already holds it
    }

    ParamPort* ParamCells::port()
    {
        return &port_;
    }

    // ---- PrefCells --------------------------------------------------------------------------------------------------

    PrefCells::PrefCells(const char* key, std::vector<int> values, std::vector<CellText> texts, int fallback)
        : key_(key), values_(std::move(values)), texts_(std::move(texts)), fallback_(fallback)
    {
        if (texts_.size() > values_.size())
            texts_.resize(values_.size());
        values_.resize(texts_.size());                   // paired by index: the shorter list wins
        UiPreferences::get();                            // open the store now, never first in a tick or draw
    }

    int PrefCells::count() const
    {
        return static_cast<int>(values_.size());
    }

    int PrefCells::value() const
    {
        if (values_.empty())
            return fallback_;
        const int held = UiPreferences::get().getInt(key_, fallback_, INT_MIN, INT_MAX);   // snapped below, not clamped
        for (const int v : values_)
            if (v == held)
                return v;
        for (const int v : values_)
            if (v == fallback_)
                return v;
        return values_.front();
    }

    int PrefCells::active() const
    {
        const int v = value();
        for (size_t i = 0; i < values_.size(); ++i)
            if (values_[i] == v)
                return static_cast<int>(i);
        return -1;
    }

    const char* PrefCells::label(int i) const
    {
        return i >= 0 && i < count() ? orEmpty(texts_[static_cast<size_t>(i)].label) : "";
    }

    const char* PrefCells::spoken(int i) const
    {
        if (i < 0 || i >= count())
            return "";
        const CellText& t = texts_[static_cast<size_t>(i)];
        return nonEmpty(t.spoken) ? t.spoken : orEmpty(t.label);
    }

    const char* PrefCells::help(int i) const
    {
        return i >= 0 && i < count() ? texts_[static_cast<size_t>(i)].help : nullptr;
    }

    void PrefCells::select(int i, GestureController&)
    {
        if (i >= 0 && i < count() && nonEmpty(key_))
            UiPreferences::get().setInt(key_, values_[static_cast<size_t>(i)]);   // a no-op when already held
    }
}

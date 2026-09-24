#include <funkgui/gpu/A11yBridge.h>

#include <funkgui/panel/Panel.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <utility>

// A11yBridge (02 §5.6; G7). HR's AccessibleItem (BgfxEditor.cpp:255-430) generalised: HR's items read the processor
// and the editor directly, these read only the A11yItem the Panel last listed and write only through
// Panel::a11yAction, so a Panel that works headless works under VoiceOver with no product code in the bridge.

namespace funkgui
{
    namespace
    {
        juce::AccessibilityRole juceRole(A11yRole r) noexcept
        {
            using R = juce::AccessibilityRole;
            switch (r)
            {
                case A11yRole::slider:       return R::slider;
                case A11yRole::toggleButton: return R::toggleButton;
                case A11yRole::button:       return R::button;
                case A11yRole::radioGroup:   return R::group;
                case A11yRole::radioButton:  return R::radioButton;
                case A11yRole::comboBox:     return R::comboBox;
                case A11yRole::listItem:     return R::listItem;
                case A11yRole::staticText:   return R::staticText;
                case A11yRole::image:        return R::image;
                case A11yRole::progressBar:  return R::progressBar;
            }
            return R::unspecified;
        }

        bool interactive(const A11yItem& it) noexcept
        {
            return it.role != A11yRole::staticText && it.role != A11yRole::image
                && it.role != A11yRole::progressBar && it.role != A11yRole::radioGroup;
        }

        bool hasRange(const A11yItem& it) noexcept
        {
            return it.role == A11yRole::slider || it.role == A11yRole::progressBar;
        }

        bool sameBits(double a, double b) noexcept
        {
            return std::bit_cast<uint64_t>(a) == std::bit_cast<uint64_t>(b);
        }

        // Logical-px bounds as the smallest integer rectangle that contains them (a child never shrinks its item).
        juce::Rectangle<int> intBounds(const Rect& r) noexcept
        {
            return juce::Rectangle<float>(r.x, r.y, std::max(r.w, 0.0f), std::max(r.h, 0.0f))
                .getSmallestIntegerContainer();
        }
    }

    //==================================================================================================================
    class A11yBridge::Item final : public juce::Component
    {
    public:
        Item(Panel& panel, const A11yItem& item) : panel_(panel), item_(item)
        {
            setOpaque(false);
            setInterceptsMouseClicks(false, false);
            setWantsKeyboardFocus(false);
            setAccessible(true);
            applyFields();
        }

        const A11yItem& item() const noexcept { return item_; }

        // The new state of the same item (same id, role and parent), with the accessibility events it calls for. A
        // change that alters the handler's shape (a value appearing or disappearing, checkable) drops the handler, so
        // JUCE builds a new one on the next query.
        void update(const A11yItem& next)
        {
            const bool valueChanged = !sameBits(next.v, item_.v) || next.value != item_.value
                                   || next.checked != item_.checked;
            const bool titleChanged = next.title != item_.title;
            const bool reshaped = next.checkable != item_.checkable
                               || (!hasRange(next) && next.value.empty() != item_.value.empty());
            item_ = next;
            applyFields();
            if (reshaped)
            {
                invalidateAccessibilityHandler();
                return;
            }
            if (auto* h = getAccessibilityHandler())
            {
                if (titleChanged)
                    h->notifyAccessibilityEvent(juce::AccessibilityEvent::titleChanged);
                if (valueChanged)
                    h->notifyAccessibilityEvent(juce::AccessibilityEvent::valueChanged);
            }
        }

        // The item's bounds in its parent component: the editor's (logical px, the Panel's space) or, when nested,
        // relative to the parent item's.
        void place(const Item* parentItem)
        {
            auto b = intBounds(item_.bounds);
            if (parentItem != nullptr)
                b = b - intBounds(parentItem->item_.bounds).getPosition();
            if (b != getBounds())
                setBounds(b);
        }

        std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

    private:
        void applyFields()
        {
            setTitle(juce::String::fromUTF8(item_.title.c_str()));
            setDescription(juce::String::fromUTF8(item_.description.c_str()));
            setHelpText(juce::String::fromUTF8(item_.help.c_str()));
            if (isEnabled() != item_.enabled)
                setEnabled(item_.enabled);
            if (isVisible() != item_.visible)
                setVisible(item_.visible);
        }

        void act(A11yAction a, double v = 0.0) { panel_.a11yAction(item_.id, a, v); }

        struct RangedValue;
        struct TextValue;
        struct Handler;

        Panel&   panel_;
        A11yItem item_;
    };

    // A slider or progress bar: {v, lo, hi, step} (index space for stepped sliders, F §8.2).
    struct A11yBridge::Item::RangedValue final : juce::AccessibilityValueInterface
    {
        explicit RangedValue(Item& o) : owner(o) {}

        bool isReadOnly() const override
        {
            return owner.item_.readOnly || owner.item_.role == A11yRole::progressBar || !owner.item_.enabled;
        }
        double getCurrentValue() const override { return owner.item_.v; }
        juce::String getCurrentValueAsString() const override
        {
            return owner.item_.value.empty() ? juce::String(owner.item_.v)
                                             : juce::String::fromUTF8(owner.item_.value.c_str());
        }
        void setValue(double v) override
        {
            if (isReadOnly() || !std::isfinite(v))
                return;
            const A11yItem& it = owner.item_;
            owner.act(A11yAction::setValue, it.hi > it.lo ? std::clamp(v, it.lo, it.hi) : v);
        }
        void setValueAsString(const juce::String& s) override { setValue(s.getDoubleValue()); }
        AccessibleValueRange getRange() const override
        {
            const A11yItem& it = owner.item_;
            if (!(it.hi > it.lo))
                return {};
            const double interval = it.step > 0.0 ? it.step : (it.hi - it.lo) / 100.0;
            return { { it.lo, it.hi }, interval };
        }

        Item& owner;
    };

    // Any other item with a spoken value (a readout, a combo box's current entry): read-only text.
    struct A11yBridge::Item::TextValue final : juce::AccessibilityTextValueInterface
    {
        explicit TextValue(Item& o) : owner(o) {}

        bool isReadOnly() const override { return true; }
        juce::String getCurrentValueAsString() const override
        {
            return juce::String::fromUTF8(owner.item_.value.c_str());
        }
        void setValueAsString(const juce::String&) override {}

        Item& owner;
    };

    struct A11yBridge::Item::Handler final : juce::AccessibilityHandler
    {
        Handler(Item& o, juce::AccessibilityRole r, juce::AccessibilityActions a, Interfaces i)
            : juce::AccessibilityHandler(o, r, std::move(a), std::move(i)), owner(o)
        {
        }

        juce::AccessibleState getCurrentState() const override
        {
            auto s = juce::AccessibilityHandler::getCurrentState();
            if (owner.item_.checkable)
            {
                s = s.withCheckable();
                if (owner.item_.checked)
                    s = s.withChecked();
            }
            return s;
        }

        Item& owner;
    };

    std::unique_ptr<juce::AccessibilityHandler> A11yBridge::Item::createAccessibilityHandler()
    {
        using T = juce::AccessibilityActionType;
        const A11yRole role = item_.role;
        juce::AccessibilityActions actions;
        const bool pressable = role == A11yRole::button || role == A11yRole::toggleButton
                            || role == A11yRole::radioButton || role == A11yRole::listItem
                            || role == A11yRole::comboBox;
        if (pressable)
            actions.addAction(T::press, [this] { act(A11yAction::press); });
        if (role == A11yRole::toggleButton || item_.checkable)
            actions.addAction(T::toggle, [this] { act(A11yAction::toggle); });
        if (role == A11yRole::comboBox || role == A11yRole::slider)
            actions.addAction(T::showMenu, [this] { act(A11yAction::showMenu); });
        if (interactive(item_))
            actions.addAction(T::focus, [this] { act(A11yAction::focus); });

        juce::AccessibilityHandler::Interfaces ifaces;
        if (hasRange(item_))
            ifaces = juce::AccessibilityHandler::Interfaces(std::make_unique<RangedValue>(*this));
        else if (!item_.value.empty())
            ifaces = juce::AccessibilityHandler::Interfaces(std::make_unique<TextValue>(*this));
        return std::make_unique<Handler>(*this, juceRole(role), std::move(actions), std::move(ifaces));
    }

    //==================================================================================================================
    A11yBridge::A11yBridge() = default;

    A11yBridge::~A11yBridge()
    {
        clear();
    }

    void A11yBridge::clear()
    {
        // Children first (a nested child's parent is another child), then the parents; each leaves its parent
        // component, so the editor never holds a dangling child.
        for (auto it = children_.rbegin(); it != children_.rend(); ++it)
            if (auto* p = (*it)->getParentComponent())
                p->removeChildComponent(it->get());
        children_.clear();
        items_.clear();
    }

    juce::Component* A11yBridge::componentFor(uint32_t id) const noexcept
    {
        for (const auto& c : children_)
            if (c->item().id == id)
                return c.get();
        return nullptr;
    }

    bool A11yBridge::sameStructure(const std::vector<A11yItem>& items) const
    {
        if (items.size() != items_.size())
            return false;
        for (size_t i = 0; i < items.size(); ++i)
            if (items[i].id != items_[i].id || items[i].role != items_[i].role || items[i].parent != items_[i].parent)
                return false;
        return true;
    }

    void A11yBridge::rebuild(juce::Component& editor, const std::vector<A11yItem>& items, Panel& panel)
    {
        clear();
        children_.reserve(items.size());
        for (const A11yItem& it : items)
        {
            auto child = std::make_unique<Item>(panel, it);
            // Nested under its parent item when that item is already a child (a radio group's buttons); otherwise
            // directly under the editor.
            Item* parentItem = nullptr;
            if (it.parent != 0)
                for (const auto& c : children_)
                    if (c->item().id == it.parent)
                        parentItem = c.get();
            juce::Component& parent = parentItem != nullptr ? static_cast<juce::Component&>(*parentItem) : editor;
            child->place(parentItem);
            parent.addChildComponent(*child);    // visibility is the item's (applyFields)
            children_.push_back(std::move(child));
        }
        items_ = items;
        ++rebuilds_;
    }

    void A11yBridge::sync(juce::Component& editor, const std::vector<A11yItem>& items, Panel& panel)
    {
        if (!sameStructure(items))
        {
            rebuild(editor, items, panel);
            return;
        }
        for (size_t i = 0; i < items.size(); ++i)
        {
            Item& child = *children_[i];
            child.update(items[i]);
            child.place(dynamic_cast<const Item*>(child.getParentComponent()));
        }
        items_ = items;
    }
}

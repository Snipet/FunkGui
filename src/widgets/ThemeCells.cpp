#include <funkgui/widgets/ThemeCells.h>

#include <funkgui/core/Theme.h>
#include <funkgui/prefs/UiPreferences.h>

#include <utility>

// ThemeCells (02 §5.5, §6.3): HR's theme selector (BgfxEditor.cpp:236-243 layout, :1146-1160 draw, :1782-1786 click,
// :448-449 a11y names) as a SegmentedSelector over UiPreferences::theme(). Choices are listed in the header.

namespace funkgui
{
    namespace
    {
        // HR's a11y names, one per Theme index. A theme added to Theme.h must be named here too.
        constexpr const char* kSpoken[] = { "Graphite theme", "Paper theme" };
        static_assert(sizeof(kSpoken) / sizeof(kSpoken[0]) == static_cast<size_t>(Theme::kCount),
                      "ThemeCells: one spoken name per theme");
    }

    ThemeCells::ThemeCells(std::vector<Rect> cells, const char* caption, Point captionAt, uint32_t a11yId)
        : selector_(*this, std::move(cells), CellStyle::text, caption, captionAt, a11yId)
    {
        selector_.setSpokenTitle("Theme");
        UiPreferences::get();                            // open the store now, never first in a tick or draw
    }

    int ThemeCells::count() const
    {
        return Theme::kCount;
    }

    int ThemeCells::active() const
    {
        return UiPreferences::get().theme();
    }

    const char* ThemeCells::label(int i) const
    {
        return i >= 0 && i < Theme::kCount ? Theme::name(i) : "";
    }

    const char* ThemeCells::spoken(int i) const
    {
        return i >= 0 && i < Theme::kCount ? kSpoken[i] : "";
    }

    void ThemeCells::select(int i, GestureController&)
    {
        if (i >= 0 && i < Theme::kCount)
            UiPreferences::get().setTheme(i);            // a no-op when already the theme
    }
}

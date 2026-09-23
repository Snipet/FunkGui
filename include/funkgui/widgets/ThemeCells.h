#pragma once

// The THEME cells (02 §5.5): a SegmentedSelector in CellStyle::text bound to UiPreferences::theme(), HR's behaviour
// verbatim (BgfxEditor.cpp:236-237, 1115-1131, 1703-1707): one cell per Theme (Theme::kCount, labelled Theme::name),
// active ink70, hover ink100, rest ink32; selecting writes the preference (machine-wide, not a host parameter, so no
// gesture), which bumps UiPreferences::revision() and every open editor follows.
//
// Declared in G2 (v0.2.0, frozen at FZ1); implemented by G6 (src/widgets/ThemeCells.cpp).

#include <funkgui/core/Geometry.h>
#include <funkgui/widgets/SegmentedSelector.h>

#include <cstdint>
#include <vector>

namespace funkgui
{
    class ThemeCells final : private CellModel
    {
    public:
        // cells: one rectangle per theme, in Theme index order.
        ThemeCells(std::vector<Rect> cells, const char* caption, Point captionAt, uint32_t a11yId);

        ThemeCells(const ThemeCells&) = delete;
        ThemeCells& operator=(const ThemeCells&) = delete;

        SegmentedSelector&       selector() noexcept       { return selector_; }   // draw, input and a11y
        const SegmentedSelector& selector() const noexcept { return selector_; }

    private:
        int  count() const override;
        int  active() const override;
        const char* label(int) const override;
        const char* spoken(int) const override;
        void select(int, GestureController&) override;

        SegmentedSelector selector_;
    };
}

// FUNKGUI_TEST name=fg.widgets.relayout timeout=60 gpu=0
//
// fg.widgets.relayout (0.18.0): widgets a resizable Panel moves (panel/Panel.h "Resizing"): Dial::setGeom and
// SegmentedSelector::setCells / setCaptionAt. Spec rows only; no JUCE.
// - dial.*  after setGeom the hit area, the centre and the accessibility bounds are the new geometry's, and the old
//           place hits nothing
// - cells.* after setCells the bounds are the new cells' union, a point in a new cell hits and one in an old cell does
//           not, the radio buttons' a11y bounds are the new cells; fewer cells than before keep working

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/params/GestureController.h>
#include <funkgui/params/ParamPort.h>
#include <funkgui/test/Harness.h>
#include <funkgui/widgets/Dial.h>
#include <funkgui/widgets/SegmentedSelector.h>
#include <funkgui/widgets/ValueModel.h>

#include <cstdint>
#include <vector>

namespace T = funkgui::test;

namespace
{
    class FixedValue final : public funkgui::ValueModel
    {
    public:
        uint64_t key() const override { return 1; }
        void view(funkgui::ValueView& v) const override
        {
            v = funkgui::ValueView{};
            v.state = funkgui::ValueState::continuous;
            v.label = "GAIN";
            v.track = 0.5f;
        }
        funkgui::ParamPort* port() override { return nullptr; }
        float host01FromTrack(float t) const override { return t; }
        float defaultHost01() const override { return 0.5f; }
    };

    class ThreeCells final : public funkgui::CellModel
    {
    public:
        int count() const override { return 3; }
        int active() const override { return 0; }
        const char* label(int i) const override { return i == 0 ? "A" : (i == 1 ? "B" : "C"); }
        const char* spoken(int i) const override { return label(i); }
        void select(int, funkgui::GestureController&) override {}
    };

    bool sameRect(const funkgui::Rect& a, const funkgui::Rect& b)
    {
        return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
    }
}

int main(int argc, char** argv)
{
    const funkgui::HeadlessGuiScope gui;
    T::Probe P("fg.widgets.relayout", "", argc, argv);

    // ---- dial -------------------------------------------------------------------------------------------------------
    {
        FixedValue model;
        funkgui::Dial dial(model, funkgui::DialGeom{ 20.0f, 30.0f }, 100);
        const funkgui::Point oldCentre = dial.geom().centre();
        P.eq("dial.hits_before", dial.contains(oldCentre) ? 1 : 0, 1);
        const funkgui::DialGeom moved{ 400.0f, 250.0f };
        dial.setGeom(moved);
        P.eq("dial.geom", dial.geom().x == 400.0f && dial.geom().top == 250.0f ? 1 : 0, 1);
        P.eq("dial.hits_new", dial.contains(moved.centre()) ? 1 : 0, 1);
        P.eq("dial.old_place_misses", dial.contains(oldCentre) ? 1 : 0, 0);
        funkgui::A11yItem item;
        dial.accessibility(item);
        P.eq("dial.a11y_bounds", sameRect(item.bounds, moved.hit()) ? 1 : 0, 1);
    }

    // ---- cells ------------------------------------------------------------------------------------------------------
    {
        ThreeCells model;
        funkgui::SegmentedSelector sel(model, { { 10, 10, 40, 18 }, { 52, 10, 40, 18 }, { 94, 10, 40, 18 } },
                                       funkgui::CellStyle::boxed, nullptr, funkgui::Point{}, 200);
        P.eq("cells.hits_before", sel.contains({ 60, 15 }) ? 1 : 0, 1);
        sel.setCells({ { 300, 400, 50, 20 }, { 352, 400, 50, 20 }, { 404, 400, 50, 20 } });
        const funkgui::Rect b = sel.bounds();
        P.eq("cells.bounds", sameRect(b, { 300, 400, 154, 20 }) ? 1 : 0, 1);
        P.eq("cells.hits_new", sel.contains({ 360, 410 }) ? 1 : 0, 1);
        P.eq("cells.old_misses", sel.contains({ 60, 15 }) ? 1 : 0, 0);
        std::vector<funkgui::A11yItem> items;
        sel.accessibility(items);
        bool placed = items.size() == 4;
        for (std::size_t i = 1; placed && i < items.size(); ++i)
            placed = items[i].bounds.x == 300.0f + 52.0f * static_cast<float>(i - 1) && items[i].bounds.y == 400.0f;
        P.eq("cells.a11y_bounds", placed ? 1 : 0, 1);
        sel.setCells({ { 0, 0, 30, 10 } });
        P.eq("cells.fewer_bounds", sameRect(sel.bounds(), { 0, 0, 30, 10 }) ? 1 : 0, 1);
        P.eq("cells.fewer_hits", sel.contains({ 5, 5 }) ? 1 : 0, 1);
        P.eq("cells.fewer_settled", sel.settled() ? 1 : 0, 1);
    }
    return P.finish();
}

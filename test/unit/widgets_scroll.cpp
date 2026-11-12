// FUNKGUI_TEST name=fg.widgets.scroll timeout=120 gpu=0
//
// fg.widgets.scroll (F1; widgets/Scroll.h): the TextEditor's wheel rule and scrollbar geometry as public constants and
// functions, which the TextEditor uses itself. Spec rows only; no JUCE, so it runs in every configuration.
//
// - const.*   the constants are the TextEditor's (512 px per smooth unit, a notch of 50/256, 3 rows per notch, a bar
//             4 px wide, 2 px in, its thumb at least 12 px).
// - fn.*      wheelPx and thumb by their rules: the sign of the delta, a notch is 3 rows; a thumb in proportion, at
//             least kBarMin, at most the track, at 0 and at the end of its room, at 0 when nothing can scroll.
// - wheel.*   a TextEditor over HeadlessHost scrolled by one notch, then by a smooth delta, moves by exactly
//             -wheelPx(dy, smooth, rowHeight()) (bit for bit; no clamp reached), down and up, and dx the same way.
// - thumb.*   the vertical thumb the TextEditor draws (its tags::editFrame rrect, kBarW wide) is where thumb() puts
//             it, at the top, after the wheel and at the bottom.

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/Prim.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/core/Theme.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Input.h>
#include <funkgui/panel/Panel.h>
#include <funkgui/test/Harness.h>
#include <funkgui/text/FontService.h>
#include <funkgui/widgets/Scroll.h>
#include <funkgui/widgets/TextEditor.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace T = funkgui::test;
namespace scroll = funkgui::scroll;
using funkgui::HeadlessHost;
using funkgui::Prim;
using funkgui::PrimList;
using funkgui::Rect;
using funkgui::TextEditor;
using funkgui::WheelEvent;

namespace
{
    constexpr Rect kBox{ 10.0f, 10.0f, 300.0f, 120.0f };

    class EditorPanel final : public funkgui::Panel
    {
    public:
        EditorPanel() { ed.setBounds(kBox); }

        void attach(funkgui::HostServices& h) override { ed.attach(&h); }
        int  width() const override { return 320; }
        int  height() const override { return 140; }
        void tick(float dt) override { ed.tick(dt); }
        void draw(funkgui::Canvas& c, const funkgui::Theme& th) override { ed.draw(c, th, false); }
        bool wantsFullRate() const override { return false; }
        bool wheel(const WheelEvent& e) override { return ed.wheel(e); }
        void accessibility(std::vector<funkgui::A11yItem>&) const override {}
        uint32_t a11yRevision() const override { return 0; }
        void a11yAction(uint32_t, funkgui::A11yAction, double) override {}
        void closeGestures() override {}

        TextEditor ed{ 3 };
    };

    bool sameBits(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }

    // The vertical thumb: the editFrame rrect kBarW wide whose centre is in the bar's column.
    bool vThumb(const PrimList& l, float barCentreX, float& top, float& height)
    {
        int found = 0;
        for (const Prim& p : l.prims)
        {
            if (p.tag != funkgui::tags::editFrame || static_cast<int>(p.d2[2] + 0.5f) != 0)
                continue;
            const float cx = 0.5f * (p.x0 + p.x1), cy = 0.5f * (p.y0 + p.y1);
            if (std::fabs(p.d0[2] * 2.0f - scroll::kBarW) > 1e-4f || std::fabs(cx - barCentreX) > 1e-3f)
                continue;
            top = cy - p.d0[3];
            height = 2.0f * p.d0[3];
            ++found;
        }
        return found == 1;
    }

    WheelEvent wheelAt(float x, float y, float dx, float dy, bool smooth)
    {
        WheelEvent w;
        w.x = x;
        w.y = y;
        w.dx = dx;
        w.dy = dy;
        w.smooth = smooth;
        return w;
    }
}

int main(int argc, char** argv)
{
    const funkgui::HeadlessGuiScope gui;
    const T::ScopedFtz ftz;
    T::Probe P("fg.widgets.scroll", "", argc, argv);

    // ---- const, fn --------------------------------------------------------------------------------------------------
    P.eq("const.values", scroll::kWheelPxPerUnit == 512.0f && scroll::kWheelNotch == 50.0f / 256.0f
                             && scroll::kRowsPerNotch == 3.0f && scroll::kBarW == 4.0f && scroll::kBarInset == 2.0f
                             && scroll::kBarMin == 12.0f, 1);
    P.eq("fn.notch_is_three_rows", sameBits(scroll::wheelPx(scroll::kWheelNotch, false, 17.0f), 3.0f * 17.0f), 1);
    P.eq("fn.smooth", scroll::wheelPx(0.25f, true, 17.0f) == 128.0f, 1);
    P.eq("fn.sign", scroll::wheelPx(-0.25f, true, 17.0f) == -128.0f && scroll::wheelPx(0.0f, false, 17.0f) == 0.0f, 1);
    static_assert(scroll::wheelPx(1.0f, true, 1.0f) == 512.0f, "wheelPx is constexpr");
    {
        const scroll::Thumb half = scroll::thumb(100.0f, 400.0f, 100.0f, 0.0f);
        P.eq("fn.thumb_proportion", half.size == 25.0f && half.pos == 0.0f, 1);
        const scroll::Thumb end = scroll::thumb(100.0f, 400.0f, 100.0f, 300.0f);
        P.eq("fn.thumb_end", end.pos == 75.0f && end.size == 25.0f, 1);
        P.eq("fn.thumb_min", scroll::thumb(100.0f, 100000.0f, 100.0f, 0.0f).size == scroll::kBarMin, 1);
        P.eq("fn.thumb_max", scroll::thumb(8.0f, 100000.0f, 100.0f, 0.0f).size == 8.0f, 1);
        const scroll::Thumb none = scroll::thumb(100.0f, 50.0f, 100.0f, 10.0f);
        P.eq("fn.thumb_nothing_to_scroll", none.pos == 0.0f && none.size == 100.0f, 1);
    }

    funkgui::FontService::get().atlas();
    if (!P.eq("font.ok", funkgui::FontService::get().ok(), 1))
        return P.finish();

    // ---- wheel ------------------------------------------------------------------------------------------------------
    EditorPanel panel;
    HeadlessHost host(panel, 0, 2.0f);
    TextEditor& e = panel.ed;
    std::string many;
    for (int i = 0; i < 60; ++i)
        many += "line " + std::to_string(i) + " of a text long enough to overflow the box sideways too, surely\n";
    e.setText(many);
    const float rowH = e.rowHeight();
    const float x = kBox.centreX(), y = kBox.centreY();
    P.eq("wheel.overflows", e.contentHeight() > e.textArea().h && e.contentWidth() > e.textArea().w, 1);

    P.eq("wheel.notch_taken", host.wheel(wheelAt(x, y, 0.0f, -scroll::kWheelNotch, false)), 1);
    const float afterNotch = 0.0f - scroll::wheelPx(-scroll::kWheelNotch, false, rowH);
    P.eq("wheel.notch_exact", sameBits(e.scroll().y, afterNotch), 1);
    P.near("wheel.notch_three_rows", e.scroll().y, 3.0 * rowH, 1e-4);
    host.wheel(wheelAt(x, y, 0.0f, -0.0375f, true));
    const float afterSmooth = afterNotch - scroll::wheelPx(-0.0375f, true, rowH);
    P.eq("wheel.smooth_exact", sameBits(e.scroll().y, afterSmooth), 1);
    host.wheel(wheelAt(x, y, 0.0f, 0.0125f, true));
    P.eq("wheel.smooth_up_exact", sameBits(e.scroll().y, afterSmooth - scroll::wheelPx(0.0125f, true, rowH)), 1);
    host.wheel(wheelAt(x, y, -scroll::kWheelNotch, 0.0f, false));
    P.eq("wheel.dx_exact", sameBits(e.scroll().x, 0.0f - scroll::wheelPx(-scroll::kWheelNotch, false, rowH)), 1);
    e.setScroll(0.0f, e.scroll().y);

    // ---- thumb ------------------------------------------------------------------------------------------------------
    // The vertical bar: kBarInset in from the box's inner right edge (no minimap), as tall as the text area less the
    // horizontal bar and its inset.
    const Rect inner = kBox.reduced(1.0f);
    const float barCentreX = inner.right() - scroll::kBarInset - scroll::kBarW * 0.5f;
    const Rect ta = e.textArea();
    const float track = ta.h - (scroll::kBarW + scroll::kBarInset);
    const auto checkThumb = [&](const std::string& key)
    {
        float top = 0.0f, h = 0.0f;
        const bool one = vThumb(host.draw(), barCentreX, top, h);
        const scroll::Thumb t = scroll::thumb(track, e.contentHeight(), ta.h, e.scroll().y);
        P.eq(key + ".found", one, 1);
        P.near(key + ".pos", top, ta.y + t.pos, 1e-3);
        P.near(key + ".size", h, t.size, 1e-3);
    };
    checkThumb("thumb.wheeled");
    e.setScroll(0.0f, 0.0f);
    checkThumb("thumb.top");
    e.setScroll(0.0f, 1.0e9f);
    checkThumb("thumb.bottom");
    P.eq("thumb.bottom_at_end", e.scroll().y == e.contentHeight() - ta.h, 1);
    return P.finish();
}

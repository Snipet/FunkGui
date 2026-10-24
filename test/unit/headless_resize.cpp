// FUNKGUI_TEST name=fg.headless.resize timeout=60 gpu=0
//
// fg.headless.resize (0.18.0): resizable Panels under HeadlessHost::resize (panel/Panel.h "Resizing",
// panel/HeadlessHost.h). Spec rows only; no JUCE, so it runs in every configuration.
// - fixed.*    a fixed Panel (the default) gets no resize(); resize() returns false and is counted
// - grow.*     a resizable Panel gets resize(w, h) once with the asked size; width() and height() answer it, and the
//              next draw() records a frame of that logical size
// - min.*      a size below the minimum reaches the Panel raised to it, on each axis on its own
// - same.*     a size equal to the current one calls nothing (and one raised to the current one neither)

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/core/Theme.h>
#include <funkgui/panel/HeadlessGuiScope.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/panel/Panel.h>
#include <funkgui/test/Harness.h>

#include <cstdint>
#include <vector>

namespace T = funkgui::test;

namespace
{
    // A Panel of a given size, resizable or not, that counts its resize() calls and draws one rectangle over itself.
    class SizedPanel final : public funkgui::Panel
    {
    public:
        SizedPanel(int w, int h, bool resizable, int minW = 0, int minH = 0)
            : w_(w), h_(h), resizable_(resizable), minW_(minW), minH_(minH)
        {
        }

        int resizes = 0;

        void attach(funkgui::HostServices&) override {}
        int  width() const override { return w_; }
        int  height() const override { return h_; }
        bool resizable() const override { return resizable_; }
        int  minWidth() const override { return minW_ > 0 ? minW_ : w_; }
        int  minHeight() const override { return minH_ > 0 ? minH_ : h_; }
        void resize(int w, int h) override
        {
            w_ = w;
            h_ = h;
            ++resizes;
        }
        void tick(float) override {}
        void draw(funkgui::Canvas& c, const funkgui::Theme& th) override
        {
            c.rrect(0.0f, 0.0f, static_cast<float>(w_), static_cast<float>(h_), 0.0f, th.ink16);
        }
        bool wantsFullRate() const override { return false; }
        void accessibility(std::vector<funkgui::A11yItem>&) const override {}
        uint32_t a11yRevision() const override { return 0; }
        void a11yAction(uint32_t, funkgui::A11yAction, double) override {}
        void closeGestures() override {}

    private:
        int  w_, h_;
        bool resizable_;
        int  minW_, minH_;
    };
}

int main(int argc, char** argv)
{
    const funkgui::HeadlessGuiScope gui;
    T::Probe P("fg.headless.resize", "", argc, argv);

    // ---- fixed: nothing reaches the Panel ---------------------------------------------------------------------------
    {
        SizedPanel panel(300, 200, false);
        funkgui::HeadlessHost host(panel);
        P.eq("fixed.returns", host.resize(500, 400) ? 1 : 0, 0);
        P.eq("fixed.no_resize", panel.resizes, 0);
        P.eq("fixed.width", panel.width(), 300);
        P.eq("fixed.counted", host.log.resizes, 1);
    }

    // ---- grow, min, same -------------------------------------------------------------------------------------------
    {
        SizedPanel panel(300, 200, true, 240, 160);
        funkgui::HeadlessHost host(panel);
        P.eq("grow.returns", host.resize(520, 330) ? 1 : 0, 1);
        P.eq("grow.once", panel.resizes, 1);
        P.eq("grow.width", panel.width(), 520);
        P.eq("grow.height", panel.height(), 330);
        const funkgui::PrimList& frame = host.draw();
        P.eq("grow.frame_width", frame.info.logicalW, 520);
        P.eq("grow.frame_height", frame.info.logicalH, 330);

        P.eq("min.returns", host.resize(100, 400) ? 1 : 0, 1);
        P.eq("min.width_raised", panel.width(), 240);
        P.eq("min.height_kept", panel.height(), 400);
        host.resize(500, 10);
        P.eq("min.height_raised", panel.height(), 160);
        P.eq("min.width_alone", panel.width(), 500);

        const int before = panel.resizes;
        P.eq("same.returns", host.resize(500, 160) ? 1 : 0, 0);
        P.eq("same.raised_to_current", host.resize(500, 5) ? 1 : 0, 0);
        P.eq("same.nothing_called", panel.resizes, before);
        P.eq("same.counted", host.log.resizes, 5);
    }
    return P.finish();
}

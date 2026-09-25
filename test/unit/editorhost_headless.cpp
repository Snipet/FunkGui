// FUNKGUI_TEST name=fg.editorhost.headless timeout=120 gpu=0
//
// fg.editorhost.headless: the G7b additions (v0.7.1) to HostServices and Panel as a build without bgfx sees them
// (panel/HostServices.h, panel/Panel.h, panel/HeadlessHost.h). A host written before v0.7.1, which implements only the
// pure virtuals, still compiles and answers themeIndex() 0 and ownerComponent() nullptr; HeadlessHost::themeIndex() is
// the index of the theme its draw() uses (its constructor's themeIdx, 0 for one that names no theme, as
// Theme::byIndex draws graphite then), confirmed against the frame's clear colour, and its ownerComponent() is nullptr
// (no window); a Panel written before v0.7.1 takes the new file-drag calls as no-ops, and one that overrides them
// receives exactly what it is given. EditorHost's side is fg.editorhost.api (gpu) and fg.editorhost.follow (live).
// Spec rows only.

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/core/Theme.h>
#include <funkgui/panel/HeadlessHost.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Panel.h>
#include <funkgui/test/Harness.h>

#include <cstdint>
#include <string>
#include <vector>

namespace T = funkgui::test;

namespace
{
    // A host as every host was before v0.7.1: the pure virtuals and nothing else.
    class PlainHost final : public funkgui::HostServices
    {
    public:
        void   setUnboundedDrag(bool) override {}
        void   showParamMenu(funkgui::ParamPort&, float, float) override {}
        void   nudgeFullRate() override {}
        double nowSeconds() const override { return 0.0; }
        void   beginBatch() override {}
        void   endBatch() override {}
    };

    // A Panel as every Panel was before v0.7.1: it overrides none of the file-drag calls.
    class PlainPanel : public funkgui::Panel
    {
    public:
        funkgui::HostServices* host = nullptr;
        int ticks = 0;

        void attach(funkgui::HostServices& h) override { host = &h; }
        int  width() const override { return 160; }
        int  height() const override { return 90; }
        void tick(float) override { ++ticks; }
        void draw(funkgui::Canvas& c, const funkgui::Theme& th) override
        {
            c.rrect(8.0f, 8.0f, 40.0f, 20.0f, 4.0f, th.ink52);
        }
        bool wantsFullRate() const override { return false; }
        void accessibility(std::vector<funkgui::A11yItem>&) const override {}
        uint32_t a11yRevision() const override { return 0; }
        void a11yAction(uint32_t, funkgui::A11yAction, double) override {}
        void closeGestures() override {}
    };

    // A Panel that takes file drags and records them.
    class DropPanel final : public PlainPanel
    {
    public:
        int enters = 0, moves = 0, exits = 0;
        std::vector<std::string> paths;
        float x = -1.0f, y = -1.0f;

        void filesDragEnter(const std::vector<std::string>& p, float px, float py) override
        {
            ++enters;
            paths = p;
            x = px;
            y = py;
        }
        void filesDragMove(float px, float py) override
        {
            ++moves;
            x = px;
            y = py;
        }
        void filesDragExit() override { ++exits; }
    };

    bool sameColour(funkgui::Col a, funkgui::Col b) { return a.r == b.r && a.g == b.g && a.b == b.b; }
}

int main(int argc, char** argv)
{
    T::Probe P("fg.editorhost.headless", "", argc, argv);

    // ---- a pre-v0.7.1 host: the defaults ----------------------------------------------------------------------------
    {
        PlainHost plain;
        funkgui::HostServices& h = plain;
        P.eq("plain_host.theme_index", h.themeIndex(), 0);
        P.eq("plain_host.owner_is_null", h.ownerComponent() == nullptr, 1);
    }

    // ---- HeadlessHost::themeIndex(): the index of the theme draw() uses ---------------------------------------------
    struct ThemeCase
    {
        const char* key;
        int ctorIdx;
        int want;
    };
    constexpr ThemeCase kThemes[] = {
        { "graphite", 0, 0 },
        { "paper", 1, 1 },
        { "past_the_end", funkgui::Theme::kCount, 0 },
        { "negative", -1, 0 },
    };
    for (const ThemeCase& c : kThemes)
    {
        PlainPanel panel;
        funkgui::HeadlessHost host(panel, c.ctorIdx);
        const std::string k = std::string("headless.") + c.key;
        P.eq(k + ".attached", panel.host == &host, 1);
        P.eq(k + ".theme_index", panel.host != nullptr ? panel.host->themeIndex() : -1, c.want);
        P.eq(k + ".owner_is_null", panel.host != nullptr && panel.host->ownerComponent() == nullptr, 1);
        host.tick(1);
        const funkgui::PrimList& frame = host.draw();
        // The index names exactly the theme the frame was drawn in: its ground is the frame's clear colour.
        P.eq(k + ".names_drawn_theme",
             sameColour(frame.info.clear, funkgui::Theme::byIndex(host.themeIndex()).ground), 1);
    }

    // ---- Panel: the file-drag calls -------------------------------------------------------------------------------
    {
        PlainPanel plain;                            // the defaults: accepted and ignored
        funkgui::Panel& p = plain;
        p.filesDragEnter({ "/tmp/x.preset" }, 1.0f, 2.0f);
        p.filesDragMove(3.0f, 4.0f);
        p.filesDragExit();
        P.eq("panel_defaults.no_effect", plain.ticks, 0);

        DropPanel drop;
        funkgui::Panel& d = drop;
        const std::vector<std::string> files = { "/tmp/a.preset", "/tmp/\xC3\xBC.preset" };   // UTF-8 kept as is
        d.filesDragEnter(files, 12.5f, 34.0f);
        d.filesDragMove(56.0f, 78.25f);
        d.filesDragExit();
        P.eq("panel_override.enters", drop.enters, 1);
        P.eq("panel_override.moves", drop.moves, 1);
        P.eq("panel_override.exits", drop.exits, 1);
        P.eq("panel_override.paths", drop.paths == files, 1);
        P.near("panel_override.last_x", drop.x, 56.0, 0.0);
        P.near("panel_override.last_y", drop.y, 78.25, 0.0);
    }

    return P.finish();
}

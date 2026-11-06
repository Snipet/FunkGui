// FUNKGUI_TEST name=fg.editorhost.gate timeout=60 gpu=0 links=harness
//
// fg.editorhost.gate (0.25.0; gpu/EditorHost.h "Unchanged frames", src/gpu/FrameGate.h): EditorHost's own reasons to
// draw a frame whatever the Panel's needsDraw() says, driven with made-up facts (the gate is plain C++, so this runs in
// every configuration). fg.editorhost.skip runs the same rules on real windows. Spec rows only.
//   start.*     nothing drawn yet: a draw, whether or not a frame would reach the screen
//   idle.*      the last frame shown and nothing changed: no draw, however often asked
//   state.*     each part of the state the picture was drawn for (the surface's revision, the drawable's device size,
//               the Panel's size, the theme, the picture and shader registries): a change draws, once
//   occluded.*  a frame drawn while the window read occluded is not on screen: nothing is drawn while it stays so,
//               and the first frame that reaches the screen draws; a change while occluded draws too
//   lost.*      a picture lost (the window hidden or minimised, the surface detached or waiting): drawn again at the
//               first frame that reaches the screen, and not before

#include "../../src/gpu/FrameGate.h"

#include <funkgui/test/Harness.h>

#include <functional>
#include <string>

namespace T = funkgui::test;
using funkgui::detail::FrameGate;
using funkgui::detail::SurfaceState;

namespace
{
    SurfaceState state()
    {
        SurfaceState s;
        s.surface = 3;
        s.physW = 640;
        s.physH = 400;
        s.logicalW = 320;
        s.logicalH = 200;
        s.theme = 0;
        s.images = 1;
        s.shaders = 1;
        return s;
    }
}

int main(int argc, char** argv)
{
    T::Probe P("fg.editorhost.gate", "", argc, argv);
    const SurfaceState s = state();

    // ---- start ---------------------------------------------------------------------------------------------------
    {
        const FrameGate g;
        P.eq("start.draws_visible", g.mustDraw(s, true), 1);
        P.eq("start.draws_occluded", g.mustDraw(s, false), 1);
        P.eq("start.not_shown", g.shown(), 0);
    }

    // ---- idle ----------------------------------------------------------------------------------------------------
    {
        FrameGate g;
        g.drawn(s, true);
        P.eq("idle.shown", g.shown(), 1);
        int draws = 0;
        for (int i = 0; i < 100; ++i)
            draws += g.mustDraw(s, true) ? 1 : 0;
        P.eq("idle.no_draw", draws, 0);
        P.eq("idle.no_draw_occluded", g.mustDraw(s, false), 0);
    }

    // ---- state ---------------------------------------------------------------------------------------------------
    const auto changeRow = [&](const std::string& key, const std::function<void(SurfaceState&)>& change)
    {
        FrameGate g;
        g.drawn(s, true);
        SurfaceState now = s;
        change(now);
        P.eq("state." + key, g.mustDraw(now, true), 1);
        g.drawn(now, true);
        P.eq("state." + key + "_once", g.mustDraw(now, true), 0);
        P.eq("state." + key + "_back", g.mustDraw(s, true), 1);      // the old state is another state again
    };
    changeRow("surface", [](SurfaceState& x) { x.surface += 1; });
    changeRow("device_width", [](SurfaceState& x) { x.physW = 960; });   // a zoom or a backing scale
    changeRow("device_height", [](SurfaceState& x) { x.physH = 600; });
    changeRow("logical_width", [](SurfaceState& x) { x.logicalW = 330; });
    changeRow("logical_height", [](SurfaceState& x) { x.logicalH = 210; });
    changeRow("theme", [](SurfaceState& x) { x.theme = 1; });
    changeRow("images", [](SurfaceState& x) { x.images += 1; });
    changeRow("shaders", [](SurfaceState& x) { x.shaders += 1; });

    // ---- occluded ------------------------------------------------------------------------------------------------
    {
        FrameGate g;
        g.drawn(s, false);                           // drawn while the window read occluded: not presented
        P.eq("occluded.not_shown", g.shown(), 0);
        int draws = 0;
        for (int i = 0; i < 10; ++i)
            draws += g.mustDraw(s, false) ? 1 : 0;
        P.eq("occluded.no_draw_while_occluded", draws, 0);
        P.eq("occluded.draw_when_uncovered", g.mustDraw(s, true), 1);
        g.drawn(s, true);
        P.eq("occluded.then_idle", g.mustDraw(s, true), 0);
        SurfaceState now = s;
        now.theme = 1;
        P.eq("occluded.change_draws", g.mustDraw(now, false), 1);
    }

    // ---- lost ----------------------------------------------------------------------------------------------------
    {
        FrameGate g;
        g.drawn(s, true);
        g.lost();                                    // the window was hidden, or the surface detached
        P.eq("lost.not_shown", g.shown(), 0);
        P.eq("lost.no_draw_while_occluded", g.mustDraw(s, false), 0);
        P.eq("lost.draw_when_shown", g.mustDraw(s, true), 1);
        g.drawn(s, true);
        P.eq("lost.then_idle", g.mustDraw(s, true), 0);
        g.lost();
        g.lost();
        P.eq("lost.twice_draws_once", g.mustDraw(s, true), 1);
    }
    return P.finish();
}

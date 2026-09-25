#pragma once

// Runs a Panel in a plain console process (02 §3.6): a simulated clock, synthesised input, frames recorded into an
// owned Canvas over FontService's atlas, and every HostServices call logged for asserts. With a fixed dt everything is
// deterministic, and settle() ticks until the Panel no longer wants full rate, which the snapping eases make exact.
//
// Declared in G2 (v0.2.0, frozen at FZ1); implemented by G3 (src/panel/HeadlessHost.cpp).

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/core/Theme.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Input.h>
#include <funkgui/panel/Panel.h>

#include <cstdint>
#include <vector>

namespace funkgui
{
    class HeadlessHost final : public HostServices
    {
    public:
        HeadlessHost(Panel&, int themeIdx = 0, float dpi = 2.0f);    // calls panel.attach(*this)
        ~HeadlessHost() override;

        HeadlessHost(const HeadlessHost&) = delete;
        HeadlessHost& operator=(const HeadlessHost&) = delete;

        void  tick(int frames, float dt = 1.0f / 60.0f);             // Panel::tick + the simulated clock
        int   settle(int maxFrames = 600, float dt = 1.0f / 60.0f);  // tick until !wantsFullRate(); returns frames used
        const PrimList& draw();                                      // one frame into the owned Canvas
        void  move(float x, float y);
        void  click(float x, float y, Mods = {});
        void  doubleClick(float x, float y, Mods = {});
        void  drag(float x0, float y0, float x1, float y1, int steps = 8, Mods = {});
        void  wheel(float x, float y, float dy, bool smooth = false, Mods = {});
        void  keys(const char* spec);          // HR replayKeys grammar + home,end,pageup,pagedown
        std::vector<A11yItem> accessibility() const;
        bool  writeDump(const char* path) const;                     // the last draw()'s frame, dump v2
        bool  writePng (const char* path, int supersample = 2) const;   // SoftRaster: agents can look at frames

        // HostServices calls, for asserts. `batches` counts beginBatch calls; `batchDepth` is the open depth.
        struct Log
        {
            int  menus = 0;
            bool unbounded = false;
            int  nudges = 0;
            int  batches = 0;
            int  batchDepth = 0;
        } log;

        // HostServices: records into log; nowSeconds() returns the simulated clock.
        void   setUnboundedDrag(bool on) override;
        void   showParamMenu(ParamPort&, float x, float y) override;
        void   nudgeFullRate() override;
        double nowSeconds() const override;
        void   beginBatch() override;
        void   endBatch() override;
        // G7b (v0.7.1): the index of the theme draw() uses (the constructor's themeIdx, or 0 when that names no theme).
        // ownerComponent() keeps the default nullptr: there is no window to anchor a menu to.
        int    themeIndex() const override;

    private:
        // Private state: completed by the implementing card (G3); not part of the frozen API.
        Panel&   panel_;
        Theme    theme_;
        int      themeIdx_ = 0;
        float    dpi_ = 2.0f;
        Canvas   canvas_;
        double   now_ = 0.0;
        uint32_t frame_ = 0;
        float    lastDt_ = 1.0f / 60.0f;
    };
}

#pragma once

// EditorHost's bookkeeping of the picture on screen (0.25.0; gpu/EditorHost.h "Unchanged frames"): whether a tick must
// draw for a reason of the host's own, whatever the Panel's needsDraw() says and whatever its recording is. Plain C++
// with no bgfx and no JUCE, so fg.editorhost.gate drives it with made-up facts. Message thread, like EditorHost.

#include <cstdint>

namespace funkgui::detail
{
    // What a frame is drawn for besides the Panel's recording. The picture on screen is right only for the values it
    // was drawn for: a frame drawn for others is drawn again.
    struct SurfaceState
    {
        uint32_t surface = 0;                        // BgfxContext::surfaceRevision of the render view
        int      physW = 0, physH = 0;               // the drawable in device px (backing scale and zoom)
        int      logicalW = 0, logicalH = 0;         // the Panel's size
        int      theme = 0;                          // the theme index
        uint64_t images = 0;                         // images::generation(): pictures replaced or removed
        uint64_t shaders = 0;                        // shaders::detail::generation(): programs replaced or removed

        bool operator==(const SurfaceState&) const = default;
    };

    class FrameGate
    {
    public:
        // Whether this tick must draw: nothing drawn yet; the last frame drawn for another state; or a picture that is
        // not on screen (drawn while the window was occluded, dropped, or lost while the window was hidden) when a
        // frame drawn now reaches the screen (`reachesScreen`: renderViewOccluded is false).
        bool mustDraw(const SurfaceState& now, bool reachesScreen) const noexcept
        {
            if (!drawn_ || !(now == drawnFor_))
                return true;
            return !shown_ && reachesScreen;
        }

        // A frame was submitted for `state`. `shown`: it reached the screen (the window was not occluded and the sink
        // drew it). A frame that did not leaves the gate wanting another as soon as one would reach the screen.
        void drawn(const SurfaceState& state, bool shown) noexcept
        {
            drawn_ = true;
            drawnFor_ = state;
            shown_ = shown;
        }

        // The picture may be gone or stale: the window was hidden or minimised, its surface detached or not drawable.
        // The next tick that can draw does.
        void lost() noexcept { shown_ = false; }

        bool shown() const noexcept { return drawn_ && shown_; }

    private:
        SurfaceState drawnFor_{};
        bool drawn_ = false;                         // a frame has been submitted
        bool shown_ = false;                         // and the last one is on screen
    };
}

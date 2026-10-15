// FUNKGUI_TEST name=fg.gallery.curves timeout=600 gpu=0 exe=FunkGuiGalleryProbe args="--section curves"
//
// The "curves" gallery section (FSynth G1): Canvas::arc, arcDotted and cubic (canvas/Canvas.h, "FSynth G1 additions")
// as a product draws them. Everything here is a run of capsule segments flattened by core/Flatten.h at the frame's
// dpi, so the same section records more, shorter pieces at dpi 2 than at dpi 1 and looks the same at both.
//
// - Three rings at r 11, 15 and 20, each a 270 degree track in ink16 from 135 degrees (the gap at the bottom, as a
//   dial has it) with a value arc in ink100 over a part of it, and a round dot on the value's end, all at the rings'
//   default tolerance, geom::kArcDevicePx (0.1 device px).
// - The r 20 ring again at the cables' tolerance, geom::kFlattenDevicePx (0.6 device px), passed as the last
//   argument: the same ring with fewer, longer chords, labelled with its tolerance.
// - A ring at r 20 as a dotted arc (a dot every 4 px of arc length), with a shorter dotted arc inside it that runs
//   the other way.
// - Three cables at 2.5 px between port squares: forwards (left to right, horizontal tangents), backwards (the source
//   to the right of its target, so the cable loops out and back) and vertical. A fourth, in the signal colour at
//   half strength premixed over the ground, is the one translucent colour: a curve must be opaque, so a product mixes
//   the colour it wants over the ground first (core/Col.h premix()).
//
// One state, rest; nothing is live, nothing takes input, and there are no a11y items (the a11y.rest sidecar is
// empty). No JUCE and no platform code: the section also compiles into the gallery app and, under Emscripten, the web
// gallery and the web host's page. The line above registers the test; the tools glob compiles this file into
// FunkGuiGalleryProbe.

#include "GalleryPanel.h"

#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Col.h>
#include <funkgui/core/Flatten.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/core/TypeScale.h>

#include <memory>

namespace funkgui::gallery
{
    namespace
    {
        constexpr float kPi = 3.14159265358979323846f;
        constexpr float kStart = 0.75f * kPi;            // 135 degrees: a dial's track starts at the lower left
        constexpr float kSweep = 1.5f * kPi;             // 270 degrees, clockwise

        struct Ring
        {
            Point       centre;
            float       r;
            float       value;                           // 0..1 of the sweep
            const char* label;
            float       tolDevicePx;                     // the chord error asked for
        };
        constexpr Ring kRings[4] = { { { 36.0f, 52.0f }, 11.0f, 0.25f, "R 11", geom::kArcDevicePx },
                                     { { 96.0f, 52.0f }, 15.0f, 0.5f, "R 15", geom::kArcDevicePx },
                                     { { 160.0f, 52.0f }, 20.0f, 0.8f, "R 20", geom::kArcDevicePx },
                                     { { 224.0f, 52.0f }, 20.0f, 0.8f, "0.6 PX", geom::kFlattenDevicePx } };
        constexpr Point kDotted{ 288.0f, 52.0f };

        struct Cable
        {
            Point       p[4];
            const char* label;
            Point       labelAt;
        };
        // Forwards, backwards and vertical; then the ghost, a cable being carried to no port yet.
        constexpr Cable kCables[4] = {
            { { { 28.0f, 132.0f }, { 116.0f, 132.0f }, { 116.0f, 204.0f }, { 204.0f, 204.0f } }, "FORWARD",
              { 28.0f, 216.0f } },
            { { { 404.0f, 132.0f }, { 484.0f, 132.0f }, { 164.0f, 204.0f }, { 244.0f, 204.0f } }, "BACKWARD",
              { 300.0f, 216.0f } },
            { { { 508.0f, 28.0f }, { 508.0f, 124.0f }, { 532.0f, 124.0f }, { 532.0f, 220.0f } }, "VERTICAL",
              { 440.0f, 28.0f } },
            { { { 348.0f, 28.0f }, { 412.0f, 28.0f }, { 380.0f, 92.0f }, { 452.0f, 84.0f } }, "CARRIED",
              { 348.0f, 96.0f } },
        };

        class CurvesSection final : public Section
        {
        public:
            CurvesSection() : Section(560, 240) {}

            void draw(Canvas& c, const Theme& th) override
            {
                // The rings: track, value arc, the value's end as a dot (an arc of no sweep).
                for (const Ring& ring : kRings)
                {
                    const float end = kStart + kSweep * ring.value;
                    c.arc(ring.centre, ring.r, kStart, kStart + kSweep, 2.0f, th.ink16, ring.tolDevicePx);
                    c.arc(ring.centre, ring.r, kStart, end, 2.0f, th.ink100, ring.tolDevicePx);
                    c.arc(ring.centre, ring.r, end, end, 4.0f, th.accent, ring.tolDevicePx);
                    const Canvas::Scope s(c, tags::slotLabel, false);
                    c.text(ring.label, ring.centre.x, ring.centre.y + ring.r + 10.0f, type::kCaption, th.ink52,
                           Align::centre);
                }

                // The dotted ring, and a dotted arc that runs anticlockwise inside it.
                c.arcDotted(kDotted, 20.0f, kStart, kStart + kSweep, 4.0f, 1.5f, th.ink52);
                c.arcDotted(kDotted, 12.0f, kStart + kSweep, kStart + 0.5f * kSweep, 3.0f, 1.5f, th.signal);
                {
                    const Canvas::Scope s(c, tags::slotLabel, false);
                    c.text("DOTTED", kDotted.x, kDotted.y + 30.0f, type::kCaption, th.ink52, Align::centre);
                }

                // The cables, each between two port squares; the ghost has a source only.
                for (int i = 0; i < 4; ++i)
                {
                    const Cable& cable = kCables[i];
                    const bool ghost = i == 3;
                    const Col ink = ghost ? premix(th.ground, th.signal, 0.5f) : (i == 1 ? th.accent : th.ink70);
                    c.cubic(cable.p, 2.5f, ink);
                    port(c, th, cable.p[0]);
                    if (!ghost)
                        port(c, th, cable.p[3]);
                    const Canvas::Scope s(c, tags::slotLabel, false);
                    c.text(cable.label, cable.labelAt.x, cable.labelAt.y, type::kCaption, th.ink52);
                }
            }

        private:
            // A port: an 8 px square outline, centred on the cable's end.
            static void port(Canvas& c, const Theme& th, Point at)
            {
                c.rrect(at.x - 4.0f, at.y - 4.0f, 8.0f, 8.0f, 1.0f, th.ground, 1.0f, th.ink52);
            }
        };

        const Registration kCurves{ SectionInfo{
            "curves",
            [] { return std::make_unique<CurvesSection>(); },
            {
                State{ "rest", {} },
            } } };
    }
}

// FUNKGUI_TEST name=fg.gallery.area timeout=600 gpu=0 exe=FunkGuiGalleryProbe args="--section area"
//
// The "area" gallery section (FCompressor docs/design/02-funkgui-and-ui.md §3.11 "AREA strips", §4.1–§4.2; G4): the
// recorder's new shapes as a compressor panel uses them. AREA strips: a GR history filled down from its 0 dB line with
// the trace on the bottom edge, a stroke-only OUT trace (the seam-free line), a min/max band stroked on both edges, the
// same history live (left out of the fingerprint), one hand-made column with crossed edges and one with no width;
// then polyline (a transfer curve with its unity diagonal), disc (operating dot and ring), dotted (locked tracks at
// fractional positions) and axis records for both plots. Every data point is integer arithmetic (no libm), so the
// fingerprint is arch-neutral by construction. The line above registers fg.gallery.area; the tools glob compiles this
// file into FunkGuiGalleryProbe.

#include "GalleryPanel.h"

#include <funkgui/canvas/Axis.h>
#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Col.h>
#include <funkgui/core/TypeScale.h>

#include <array>
#include <cstdint>

namespace funkgui::gallery
{
    namespace
    {
        // Plot tags, unnamed (the dump writes them as numbers): gallery-only, inside FunkGui's range.
        constexpr Tag kHistoryPlot = 240;
        constexpr Tag kCurvePlot   = 241;

        constexpr int kCols = 48;                       // history columns -> 47 AREA quads per strip

        // A deterministic GR-like series in dB (0..-18): a slow attack-release envelope over a pseudo-random drive.
        std::array<float, kCols> historyDb()
        {
            std::array<float, kCols> v{};
            uint32_t seed = 0x2545F491u;
            int env = 0;                                   // in 1/16 dB
            for (int k = 0; k < kCols; ++k)
            {
                seed = seed * 1664525u + 1013904223u;      // LCG: exact, the same on every arch
                const int drive = static_cast<int>((seed >> 24) % 288u);   // 0..287 (1/16 dB)
                env = drive > env ? env + (drive - env) / 2 : env - (env - drive) / 6;
                v[static_cast<size_t>(k)] = -static_cast<float>(env) / 16.0f;
            }
            return v;
        }

        class AreaSection final : public Section
        {
        public:
            AreaSection() : Section(520, 360) {}

            void draw(Canvas& c, const Theme& th) override
            {
                drawHistory(c, th);
                drawColumns(c, th);
                drawCurve(c, th);
                drawTracks(c, th);
            }

        private:
            // The history plots, x 16..252 (column centres, the ends clamped to the plot edges), one data series:
            // GR (0 dB at y 24, -18 dB at y 96), OUT, a RANGE band and a LIVE copy, each under its label.
            static void drawHistory(Canvas& c, const Theme& th)
            {
                constexpr float left = 16.0f, right = 252.0f, zeroY = 24.0f, pxPerDb = 4.0f;
                const auto db = historyDb();
                std::array<float, kCols> xs{}, gr{}, zero{}, out{}, lo{}, hi{}, live{};
                for (int k = 0; k < kCols; ++k)
                {
                    const auto i = static_cast<size_t>(k);
                    const float centre = left + (static_cast<float>(k) + 0.5f) * (right - left) / kCols;
                    xs[i] = k == 0 ? left : (k == kCols - 1 ? right : centre);
                    gr[i] = zeroY - db[i] * pxPerDb;                   // GR drawn downward from the 0 dB line
                    zero[i] = zeroY;
                    out[i] = 150.0f + db[i] * 1.8f;                    // an output-level trace, lower with more GR
                    hi[i] = 178.0f - db[i] * 0.5f;                     // a band: min above, max below
                    lo[i] = 196.0f - db[i] * 1.5f;
                    live[i] = 280.0f + db[i] * 1.5f;
                }

                const AxisMap hx{ left, right, -5.0f, 0.0f, false };   // seconds, newest at the right
                const AxisMap hy{ zeroY, zeroY + 18.0f * pxPerDb, 0.0f, -18.0f, false };
                c.axis(kHistoryPlot, &hx, &hy);

                // GR: fill between the 0 dB line and the value, the value stroked (bottom edge).
                c.areaStrip(xs.data(), kCols, zero.data(), gr.data(), 0.0f, th.accentDim, 1.5f, th.accent,
                            AreaEdge::bottom);
                // OUT: stroke only (fill alpha 0) and translucent: columns tile, so the fading line has no beads.
                c.areaStrip(xs.data(), kCols, out.data(), nullptr, 150.0f, th.ink70.withAlpha(0.0f), 1.25f,
                            th.ink100.withAlpha(0.6f), AreaEdge::top);
                // RANGE: a translucent band with both edges stroked.
                c.areaStrip(xs.data(), kCols, hi.data(), lo.data(), 0.0f, th.ink16.withAlpha(0.5f), 1.0f, th.ink52,
                            AreaEdge::both);
                // LIVE: the same shape flagged live, so the fingerprint leaves it out (flags bit 0).
                {
                    const Canvas::Scope scope(c, tags::none, true);
                    c.areaStrip(xs.data(), kCols, live.data(), nullptr, 292.0f, th.signal.withAlpha(0.25f), 1.0f,
                                th.signal, AreaEdge::top);
                }

                const Canvas::Scope labels(c, tags::slotLabel, false);
                c.text("GR", left, 6.0f, type::kMicro, th.ink52);
                c.text("OUT", left, 104.0f, type::kMicro, th.ink52);
                c.text("RANGE", left, 158.0f, type::kMicro, th.ink52);
                c.text("LIVE", left, 236.0f, type::kMicro, th.ink52);
            }

            // Single columns: crossed edges (only the stroke survives), a zero-width column (records nothing) and a
            // filled column with fractional ends and a sloped top.
            static void drawColumns(Canvas& c, const Theme& th)
            {
                c.area(280.0f, 296.0f, 40.0f, 60.0f, 30.0f, 50.0f, th.ink32, 1.0f, th.ink100, AreaEdge::top);
                c.area(302.0f, 302.0f, 40.0f, 60.0f, 70.0f, 80.0f, th.ink32);
                c.area(308.25f, 323.5f, 42.5f, 36.75f, 88.0f, 88.0f, th.ink52);
                const Canvas::Scope labels(c, tags::slotLabel, false);
                c.text("COLUMNS", 280.0f, 6.0f, type::kMicro, th.ink52);
            }

            // Transfer-curve plot: in -48..0 dB on x 344..504, out on y 344..184; 4:1 above -24 dB, 12 dB knee.
            static void drawCurve(Canvas& c, const Theme& th)
            {
                constexpr float x0 = 344.0f, x1 = 504.0f, y0 = 344.0f, y1 = 184.0f;
                const AxisMap ax{ x0, x1, -48.0f, 0.0f, false };
                const AxisMap ay{ y0, y1, -48.0f, 0.0f, false };
                c.axis(kCurvePlot, &ax, &ay);

                c.segment(x0, y0, x1, y1, 1.0f, th.ink16);             // unity diagonal
                constexpr int n = 33;                                  // 1.5 dB steps
                std::array<float, n> xs{}, ys{}, ghost{};
                for (int k = 0; k < n; ++k)
                {
                    const auto i = static_cast<size_t>(k);
                    const float in = -48.0f + 1.5f * static_cast<float>(k);
                    const float over = in + 24.0f;                     // relative to the threshold
                    float gain = 0.0f;                                 // quadratic soft knee, W = 12 dB, R = 4
                    if (over > 6.0f)
                        gain = (0.25f - 1.0f) * over;
                    else if (over > -6.0f)
                        gain = (0.25f - 1.0f) * (over + 6.0f) * (over + 6.0f) / 24.0f;
                    const float outDb = in + gain;
                    xs[i] = x0 + (in + 48.0f) / 48.0f * (x1 - x0);
                    ys[i] = y0 + (outDb + 48.0f) / 48.0f * (y1 - y0);
                    ghost[i] = ys[i] + 12.0f;
                }
                c.polyline(xs.data(), ys.data(), n, 1.5f, premix(th.ground, th.accent, 1.0f));
                c.polyline(xs.data(), ys.data(), 1, 1.5f, th.ink100);   // one point: no segment
                // A faded curve drawn the bead-free way: an opaque colour pre-mixed over the ground.
                c.polyline(xs.data(), ghost.data(), n, 1.0f, premix(th.ground, th.ink100, 0.3f));

                // Operating point: a dot and a ring (fill alpha 0 + ring width), plus an empty disc (r 0: nothing).
                c.disc(xs[20], ys[20], 3.5f, th.ink100);
                c.disc(xs[20], ys[20], 7.0f, th.ink100.withAlpha(0.0f), 1.0f, th.ink52);
                c.disc(xs[4], ys[4], 0.0f, th.ink100);

                const Canvas::Scope labels(c, tags::slotLabel, false);
                c.text("4:1", x1, 346.0f, type::kMicro, th.ink52, Align::right);
            }

            // Locked tracks: 1-device-px dots at fractional starts and steps, degenerate calls that draw nothing, and
            // the unlocked hairline for comparison.
            static void drawTracks(Canvas& c, const Theme& th)
            {
                {
                    const Canvas::Scope track(c, tags::slotTrack, false);
                    c.dotted(16.0f, 310.3f, 120.0f, 4.0f, th.ink32);
                    c.dotted(16.4f, 322.7f, 99.5f, 3.3f, th.ink52);
                    c.dotted(150.0f, 310.0f, 0.0f, 4.0f, th.ink52);      // no width
                    c.dotted(150.0f, 322.0f, 40.0f, 0.0f, th.ink52);     // no step
                    c.hairlineH(16.0f, 334.3f, 120.0f, th.ink16);
                }
                const Canvas::Scope labels(c, tags::slotLabel, false);
                c.text("LOCKED", 150.0f, 310.0f, type::kCaption, th.ink32);
                c.text("UNLOCKED", 150.0f, 330.0f, type::kCaption, th.ink32);
            }
        };

        const Registration kArea{ SectionInfo{
            "area",
            [] { return std::make_unique<AreaSection>(); },
            { State{ "rest", {} } } } };
    }
}

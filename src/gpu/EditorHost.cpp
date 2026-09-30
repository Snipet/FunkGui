#include <funkgui/gpu/EditorHost.h>

#include <funkgui/core/Config.h>
#include <funkgui/gpu/BgfxContext.h>
#include <funkgui/gpu/NativeSurface.h>
#include <funkgui/params/ParamPort.h>
#include <funkgui/prefs/UiPreferences.h>
#include <funkgui/text/FontService.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>

// EditorHost (02 §5.1; G7). The surface lifecycle, fallback, retry, re-parenting, backing scale, frame gating and
// input conversion are HR's BgfxEditor.cpp (:131-182, :677-840, :843-1096, :1741-2047) re-implemented around a Panel:
// every behaviour 02 §5.1 lists as an HR bug fix is kept, with its reason next to it. Choices where 02 §5.1 is silent:
//
// - Frame numbering: diagnostics().frames counts frames drawn and submitted. CANVAS_DUMP_AFTER = n dumps the frame
//   drawn when n frames were already submitted (HR's skip count), once; the Panel has been ticked n + 1 times then.
// - FrameInfo: the Panel's size, dpi = physical / logical height (HR), the theme's ground and gamma, seconds = the sum
//   of the dts the Panel was ticked with, frame = ticks, clock "fixed" under UI_FIXED_DT (fps 0) else the pump's
//   clock and measured rate, rate = the pump's last request, overflows = frames dropped so far.
// - nowSeconds() is the wall clock, or under UI_FIXED_DT the Panel's simulated clock (so nothing a capture draws can
//   depend on when the capture ran). Panel::idle(nowSeconds()) runs from a 10 Hz timer in both cases.
// - Input: every event nudges the pump to full rate and is followed by a cursor update; an unconsumed wheel event goes
//   to the parent component (the host scrolls), an unconsumed key returns false (the host gets it). Under CANVAS_DUMP
//   the editor takes no mouse input at all (HR captureMode_); UI_KEYS still replays through keyPressed().
// - Accessibility: the bridge is synced before the first tick (after UI_KEYS) and again after the first frame, then on
//   every a11yRevision() change and at most every 100 ms for values. A11Y_DUMP is written after the first frame (02
//   §5.1's table), so it holds the replayed keys' state once ticked and drawn (widgets refresh their cached views in
//   tick()): the Panel's visible items as a11yDumpLine lines (02 §5.6), the text HeadlessHost::accessibility() gives
//   for the same state after the same tick.
// - Overflow (02 §4.5): Diagnostics::overflows, one GPU_LOG line, jassertfalse (Debug), and the next dump's
//   "overflow N"; the frame still counts as submitted, since its view was cleared and touched.
//
// G7b (v0.7.1), additive:
// - Ancestor moves: a ParentWatcher (juce::ComponentMovementWatcher on the editor) calls followPlacement(), moved()'s
//   own body, whenever the editor's position in its top-level component changes; a resize still reaches resized(), a
//   new peer still parentHierarchyChanged(). A move of the whole window changes nothing inside its peer and is not
//   reported. The watcher is made last in the constructor and reset first in the destructor.
// - HostServices::themeIndex() is the index the next frame draws with: the UI_THEME override, else the preference
//   (submitFrame step 1 applies it before step 6 ticks the Panel). ownerComponent() is this editor.
// - File drags: enter, move and exit reach the Panel like the drop, as its plain types (paths as UTF-8, positions as
//   float logical px), each followed by a full-rate nudge.
//
// G7c (v0.8.0), additive (the UI zoom, EditorHost.h "Zoom"; FCompressor ADR-68). Choices where the card is silent:
// - Steps outside 25..400 % are dropped, the rest sorted and de-duplicated (a Debug assertion flags a config that
//   needed it). A default that is not listed becomes the nearest listed step (the smaller on a tie).
// - The preference is read with UiPreferences::getInt over the whole int range and taken only when it is a listed
//   step: a missing key, a damaged value or a step the product no longer lists all read as the default.
// - Fit: the window is the editor alone (a host's title bar is not known here), compared with the user area (menu
//   bar and Dock excluded) of the display that holds most of the editor, or of the display under the pointer while
//   the editor has no peer (it is being opened: the host window appears where the user clicked); no display known =
//   no fit. The largest listed step <= the chosen one that fits wins; when none fits, the smallest step. It is
//   evaluated when the zoom is chosen (setZoomPercent), read (construction, a preference revision) or the editor
//   gets a peer (parentHierarchyChanged, which resizes at once, before the surface attaches); a window dragged to
//   another display keeps its zoom until one of those happens.
// - A pin (UI_ZOOM, or 100 under CANVAS_DUMP) is applied whatever the steps and never fitted; setZoomPercent still
//   writes the preference under a pin (as a theme cell writes it under UI_THEME), and the pin keeps the size.
// - Resize timing: the constructor and parentHierarchyChanged size the editor at once; every other change (a click,
//   another editor's preference write) is applied at the start of the next frame (submitFrame step 1), so the resize,
//   the drawable's new size and the first frame at that size happen in one pump tick, and no Panel callback is on the
//   stack when the host resizes its window. Input between the click and that frame is converted at the old zoom (the
//   editor's actual size).
// - The fallback screen draws at the logical size, scaled by the zoom. GPU_LOG logs every zoom change.

namespace funkgui
{
    namespace
    {
        constexpr float  kRetryHopefulSec = 2.0f;        // HR :861-866: two seconds while still hopeful ...
        constexpr float  kRetryFailedSec  = 10.0f;       // ... ten once the fallback is showing
        constexpr int    kFailuresToFallback = 5;        // HR :700-705: five consecutive failures show the fallback
        constexpr double kA11yValuePeriod = 0.1;         // values at <= 10 Hz (02 §5.6)
        constexpr int    kIdleHz = 10;                   // Panel::idle, >= 10 Hz even with the display link stopped
        constexpr int    kMinZoomPercent = 25, kMaxZoomPercent = 400;   // G7c: CaptureConfig's UI_ZOOM range too

        double wallSeconds() { return juce::Time::getMillisecondCounterHiRes() * 0.001; }

        juce::Colour colour(Col c) { return juce::Colour::fromRGB(c.r, c.g, c.b); }

        Mods modsFrom(const juce::ModifierKeys& m)
        {
            Mods out;
            out.shift = m.isShiftDown();
            out.cmd = m.isCommandDown();
            out.alt = m.isAltDown();
            out.ctrl = m.isCtrlDown();
            return out;
        }

        // A file written beside `path` and renamed onto it once closed: a reader that polls for the file (the capture
        // script) never sees half of it (HR SdfCanvas.cpp:290-319).
        template <typename WriteFn>
        bool writeAtomically(const std::string& path, WriteFn&& write)
        {
            const std::string tmp = path + ".partial";
            std::FILE* f = std::fopen(tmp.c_str(), "w");
            if (f == nullptr)
                return false;
            const bool written = write(f);
            const bool closed = std::fclose(f) == 0;
            if (written && closed && std::rename(tmp.c_str(), path.c_str()) == 0)
                return true;
            std::remove(tmp.c_str());
            return false;
        }

        std::vector<std::string> pathsOf(const juce::StringArray& files)
        {
            std::vector<std::string> paths;
            paths.reserve(static_cast<size_t>(files.size()));
            for (const auto& f : files)
                paths.emplace_back(f.toStdString());
            return paths;
        }
    }

    // JUCE reports a component's own moves (moved()) and a new peer (parentHierarchyChanged()), but not an ancestor
    // that moves inside the same peer, which moves the editor's area of that peer all the same. This listens to the
    // editor and to every ancestor (JUCE re-registers it when the hierarchy changes) and reports a change of the
    // editor's position relative to its top-level component, as WebBrowserComponent keeps its native view placed.
    class EditorHost::ParentWatcher final : public juce::ComponentMovementWatcher
    {
    public:
        explicit ParentWatcher(EditorHost& host) : juce::ComponentMovementWatcher(&host), host_(host) {}

        using juce::ComponentMovementWatcher::componentMovedOrResized;      // the ComponentListener overloads stay
        using juce::ComponentMovementWatcher::componentVisibilityChanged;

        void componentMovedOrResized(bool wasMoved, bool /*wasResized*/) override
        {
            if (wasMoved)
                host_.followPlacement();             // a resize of the editor itself reaches resized()
        }
        void componentPeerChanged() override {}      // parentHierarchyChanged() re-attaches to a new peer
        void componentVisibilityChanged() override {}   // submitFrame() gates every frame on isShowing()

    private:
        EditorHost& host_;
    };

    //==================================================================================================================
    // Lifetime
    //==================================================================================================================

    EditorHost::EditorHost(juce::AudioProcessor& owner, EditorConfig config, std::unique_ptr<Panel> panel)
        : juce::AudioProcessorEditor(owner), panel_(std::move(panel)), config_(std::move(config)),
          capture_(CaptureConfig::fromEnv()), canvas_(FontService::get().atlas())
    {
        jassert(panel_ != nullptr);                  // an EditorHost always runs a Panel
        setOpaque(true);
        setWantsKeyboardFocus(true);
        setTitle(juce::String::fromUTF8(config::kProductName));

        if (!capture_.canvasDump.empty())
        {
            // Capture must not depend on the window being frontmost (a display link stops for an occluded window),
            // nor on the real pointer: the window opens wherever the screen puts it, and a pointer that lands on a
            // control hovers it (HR :136-148, captureMode_). Keys still replay.
            captureMode_ = true;
            FramePump::get().forceFallbackClock();
            setInterceptsMouseClicks(false, false);
        }

        // One logical size. The editor is that size times the zoom and changes only with the zoom (G7c): without zoom
        // steps it is set once, here, and nothing calls setSize() again, which also removes the path that let a preset
        // recall invalidate a live drag (HR :149-152).
        const int w = config_.width > 0 ? config_.width : panel_->width();
        const int h = config_.height > 0 ? config_.height : panel_->height();
        jassert(w == panel_->width() && h == panel_->height());   // the Panel lays out in exactly this space
        logicalW_ = w;
        logicalH_ = h;
        setResizable(false, false);

        // The preference is machine-wide and another host may have changed it since this process first read it
        // (HR :153-156). UI_THEME overrides it without persisting.
        auto& prefs = UiPreferences::get();
        prefs.reload();
        prefsRevision_ = prefs.revision();
        applyTheme(capture_.uiTheme >= 0 ? capture_.uiTheme : prefs.theme());

        // The zoom follows the same preference file (G7c): W x H exactly when there are no steps and no pin.
        initZoom();
        applyZoom();

        panel_->attach(*this);
        if (config_.setUiAttached)
            config_.setUiAttached(true);
        FramePump::get().add(this);
        startTimerHz(kIdleHz);
        parentWatcher_ = std::make_unique<ParentWatcher>(*this);
        gpuLog("editor opened (" + juce::String(w) + " x " + juce::String(h) + ")");
    }

    EditorHost::~EditorHost()
    {
        parentWatcher_.reset();                      // no placement callback while the surface goes away
        stopTimer();
        // A host can close the editor with the mouse still down, in which case mouseUp never arrives; leaving the
        // gesture open strands the host's automation write (HR :168-173).
        panel_->closeGestures();
        if (config_.setUiAttached)
            config_.setUiAttached(false);
        // Leave the pump before the render view goes away: the display link is attached to that view and must be
        // rebound before it dangles (HR :174-177).
        FramePump::get().remove(this);
        detachSurface();
        a11y_.clear();                               // the children call into the Panel; they go before it does
        gpuLog("editor closed");
    }

    EditorHost::Diagnostics EditorHost::diagnostics() const
    {
        Diagnostics d = diag_;
        d.displayLinked = FramePump::get().isDisplayLinked();
        d.fps = FramePump::get().measuredFps();
        d.scale = surfaceOk_ ? attachedScale_ : 0.0;
        d.zoomPercent = zoomApplied_;
        return d;
    }

    void EditorHost::applyTheme(int idx)
    {
        if (!surfaceOk_)
            fallbackPainted_ = false;                // the fallback is showing: repaint it in the new ground
        themeIdx_ = std::clamp(idx, 0, Theme::kCount - 1);
        theme_ = Theme::byIndex(themeIdx_);
    }

    void EditorHost::gpuLog(const juce::String& message) const
    {
        if (capture_.gpuLog)
            juce::Logger::writeToLog(juce::String("FunkGui[") + config::kProductName + "] " + message);
    }

    //==================================================================================================================
    // Surface lifecycle (HR :677-812)
    //==================================================================================================================

    void EditorHost::attachSurfaceIfPossible()
    {
        if (surfaceOk_) return;
        auto* peer = getPeer();
        if (peer == nullptr) return;

        // Every view slot taken (sixteen editors already open) is not a failure of anything; it is a wait. Counting
        // it toward the failure state pinned the seventeenth editor to the fallback screen for its whole life, even
        // after six others had been closed (HR :686-690).
        auto& ctx = BgfxContext::get();
        if (!ctx.hasFreeSlot()) return;

        const auto area = peer->getAreaCoveredBy(*this);
        const double scl = backingScaleFor(peer->getNativeHandle());
        const double s = scl > 0.0 ? scl : 1.0;

        renderView_ = createRenderView(peer->getNativeHandle(), area.getX(), area.getY(), area.getWidth(),
                                       area.getHeight());
        if (renderView_ == nullptr)
        {
            ++retryCount_;
            ++diag_.attachFailures;
            gpuLog("attach: no render view (failure " + juce::String(retryCount_) + ")");
            return;
        }
        setRenderViewScale(renderView_, s);          // v0.11.0: on Linux the view's device size is the drawable's

        physW_ = juce::roundToInt(area.getWidth() * s);
        physH_ = juce::roundToInt(area.getHeight() * s);

        if (!ctx.acquire(renderView_, physW_, physH_))
        {
            destroyRenderView(renderView_);
            renderView_ = nullptr;
            ++diag_.attachFailures;
            // Five consecutive failures show the fallback screen; the retry continues underneath it on a slow
            // cadence, and a success clears everything. The count is per streak, not per editor lifetime.
            if (++retryCount_ >= kFailuresToFallback)
                bgfxFailed_ = true;
            gpuLog("attach: BgfxContext::acquire failed (failure " + juce::String(retryCount_)
                   + (bgfxFailed_ ? ", fallback shown)" : ")"));
            return;
        }
        attachedPeer_ = peer->getNativeHandle();
        attachedScale_ = s;
        surfaceOk_ = true;
        retryCount_ = 0;
        bgfxFailed_ = false;
        fallbackPainted_ = false;
        gpuLog("attach: " + juce::String(physW_) + " x " + juce::String(physH_) + " px at scale " + juce::String(s)
               + ", view " + juce::String(static_cast<int>(ctx.viewIdFor(renderView_))) + ", transient VB "
               + juce::String(static_cast<int64_t>(ctx.transientVbBytes())) + " B");
    }

    // A display change is not an event JUCE hands a component: the window is dragged onto a screen with a different
    // backing scale, and a drawable sized for the old scale is either upsampled (blurry) or clipped. Reading the scale
    // is one message to the window; once per frame is nothing (HR :719-741).
    void EditorHost::refreshDrawableScale()
    {
        if (!surfaceOk_) return;
        auto* peer = getPeer();
        if (peer == nullptr) return;
        const double scl = backingScaleFor(renderView_);
        if (!(scl > 0.0) || std::fabs(scl - attachedScale_) < 1.0e-6) return;

        const auto area = peer->getAreaCoveredBy(*this);
        physW_ = juce::roundToInt(area.getWidth() * scl);
        physH_ = juce::roundToInt(area.getHeight() * scl);
        attachedScale_ = scl;
        setRenderViewScale(renderView_, scl);        // v0.11.0: Linux resizes the X window with the drawable
        BgfxContext::get().resizeWindow(renderView_, physW_, physH_);
        gpuLog("scale: " + juce::String(scl) + " (" + juce::String(physW_) + " x " + juce::String(physH_) + " px)");
    }

    // The window's backing scale, or UI_SCALE from frame UI_SCALE_AFTER on, so the change path above can be driven
    // and dumped where no second display exists (HR :743-752).
    double EditorHost::backingScaleFor(void* view) const
    {
        if (capture_.uiScale > 0.0f && diag_.frames >= static_cast<uint32_t>(capture_.uiScaleAfter))
            return static_cast<double>(capture_.uiScale);
        return getBackingScale(view);
    }

    void EditorHost::detachSurface()
    {
        // The display link is attached to renderView_, so it has to go first. A host re-parenting the editor reaches
        // here with the pump still holding a link on the view we are about to release (HR :754-770).
        FramePump::get().releaseClockFor(this);

        if (renderView_ != nullptr)
        {
            if (surfaceOk_)
                BgfxContext::get().release(renderView_);
            destroyRenderView(renderView_);
            renderView_ = nullptr;
        }
        surfaceOk_ = false;
        attachedPeer_ = nullptr;
    }

    void EditorHost::parentHierarchyChanged()
    {
        auto* peer = getPeer();
        if (peer == nullptr)
        {
            detachSurface();
            return;
        }
        // Several AU and VST3 wrappers re-parent the editor. The render view was left as a subview of the old peer, so
        // the editor simply went black with no recovery (HR :772-784).
        if (surfaceOk_ && peer->getNativeHandle() != attachedPeer_)
        {
            gpuLog("re-parented: re-attaching");
            detachSurface();
        }
        refitZoom();                                 // G7c: the editor's display is known now; resize before attaching
        attachSurfaceIfPossible();
    }

    void EditorHost::moved()
    {
        followPlacement();
    }

    // The editor's area of its peer moved: its own move (moved()) or an ancestor's (ParentWatcher). The view goes
    // where the editor now is; the drawable keeps its size.
    void EditorHost::followPlacement()
    {
        if (!surfaceOk_) return;
        if (auto* peer = getPeer())
        {
            const auto area = peer->getAreaCoveredBy(*this);
            setRenderViewFrame(renderView_, area.getX(), area.getY(), area.getWidth(), area.getHeight());
        }
        refreshDrawableScale();                      // a move is how a window changes display
    }

    void EditorHost::resized()
    {
        if (!surfaceOk_) return;
        auto* peer = getPeer();
        if (peer == nullptr) return;
        const auto area = peer->getAreaCoveredBy(*this);
        const double scl = backingScaleFor(renderView_);
        const double s = scl > 0.0 ? scl : 1.0;     // guarded: an unguarded read halved the drawable during a
                                                     // transient detach (HR :804-806)
        setRenderViewFrame(renderView_, area.getX(), area.getY(), area.getWidth(), area.getHeight());
        setRenderViewScale(renderView_, s);
        physW_ = juce::roundToInt(area.getWidth() * s);
        physH_ = juce::roundToInt(area.getHeight() * s);
        attachedScale_ = s;
        BgfxContext::get().resizeWindow(renderView_, physW_, physH_);
    }

    void EditorHost::paint(juce::Graphics& g)
    {
        // Only ever visible when the GPU path is unavailable. A half-working UI would be worse than an honest one, so
        // no controls are drawn (HR :816-840).
        g.fillAll(colour(theme_.ground));
        if (!bgfxFailed_) return;

        // Laid out in the logical size and scaled by the zoom (G7c); at 100 % these are the editor's own bounds.
        if (zoomApplied_ != 100)
            g.addTransform(juce::AffineTransform::scale(zoomScale_));
        const juce::Rectangle<int> bounds(0, 0, logicalW_, logicalH_);

        const juce::String title = config_.fallbackTitle != nullptr
                                     ? juce::String::fromUTF8(config_.fallbackTitle)
                                     : juce::String::fromUTF8(config::kProductName).toUpperCase();
        g.setColour(colour(theme_.ink100));
        g.setFont(juce::Font(juce::FontOptions(13.0f)).withExtraKerningFactor(0.2f));
        g.drawText(title, 40, 24, juce::jmax(0, logicalW_ - 80), 18, juce::Justification::centredLeft);

        g.setColour(colour(theme_.ink52));
        g.setFont(juce::Font(juce::FontOptions(18.0f)));
        g.drawText("GPU RENDERER UNAVAILABLE", bounds.withTrimmedBottom(40), juce::Justification::centred);

        g.setColour(colour(theme_.ink32));
        g.setFont(juce::Font(juce::FontOptions(11.0f)).withExtraKerningFactor(0.14f));
        g.drawText("AUDIO IS PROCESSING NORMALLY. CONTROL THE PLUGIN FROM YOUR HOST'S GENERIC EDITOR.",
                   bounds.withTrimmedTop(logicalH_ / 2 + 16), juce::Justification::centredTop);
    }

    //==================================================================================================================
    // The frame (02 §5.1 "submitFrame(dt) order"; HR :843-1096)
    //==================================================================================================================

    FrameClient::Result EditorHost::submitFrame(float dt)
    {
        Result result;

        // 1. Follow the preferences when another editor in this process changes them: the theme, skipped under the
        //    UI_THEME override, which would otherwise be undone at once (HR :849-860); the zoom (G7c). Then apply a
        //    zoom chosen since the last frame (here or in another editor): the editor, its render view and its
        //    drawable take the new size before this frame ticks, records and submits.
        {
            auto& prefs = UiPreferences::get();
            if (prefs.revision() != prefsRevision_)
            {
                prefsRevision_ = prefs.revision();
                if (capture_.uiTheme < 0)
                    applyTheme(prefs.theme());
                followZoomPreference();
            }
            if (zoomTarget_ != zoomApplied_)
                applyZoom();
        }

        // 2. A minimised or hidden host window would otherwise burn a GPU frame every tick, for every open instance,
        //    forever (HR :862-864).
        if (!isShowing()) return result;

        // 3. Attach, back off between retries, and paint the fallback once per state change, not every tick
        //    (HR :866-884).
        if (!surfaceOk_)
        {
            retrySec_ += dt;
            if (retrySec_ >= (bgfxFailed_ ? kRetryFailedSec : kRetryHopefulSec))
            {
                retrySec_ = 0.0f;
                attachSurfaceIfPossible();
            }
            if (!surfaceOk_)
            {
                if (!fallbackPainted_)
                {
                    fallbackPainted_ = true;
                    repaint();
                }
                return result;
            }
        }
        // The context can be torn down and rebuilt underneath a live editor (the window backing the default swapchain
        // closed and the rebuild on a survivor failed): registration is the truth; if it says no, detach and retry on
        // the next tick (HR :886-899).
        auto& ctx = BgfxContext::get();
        if (!ctx.owns(renderView_) || !ctx.valid())
        {
            gpuLog("owns: surface no longer registered; re-attaching");
            detachSurface();
            retrySec_ = kRetryHopefulSec;
            fallbackPainted_ = false;
            return result;
        }

        // 4. Follow the window between displays of different backing scale.
        refreshDrawableScale();

        // 5. First frame only: the key replay, then the accessibility tree it produced (its dump follows this frame,
        //    step 10). Here and not in the constructor: JUCE creates accessibility handlers only for a component that
        //    has a window (HR :904-925).
        const bool firstFrame = !firstFrameDone_;
        if (firstFrame)
        {
            firstFrameDone_ = true;
            if (!capture_.uiKeys.empty())
                replayKeys(capture_.uiKeys);
            syncAccessibility();
        }

        // 6. Advance the Panel: seconds, never frames; the fixed capture dt when one is set.
        const float tickDt = capture_.fixedDt > 0.0f ? capture_.fixedDt : dt;
        panel_->tick(tickDt);
        seconds_ += static_cast<double>(tickDt);
        ++ticks_;

        // 7. Record the frame.
        const auto& pump = FramePump::get();
        FrameInfo info;
        info.logicalW = panel_->width();
        info.logicalH = panel_->height();
        // Device px per logical px: the backing scale times the zoom (G7c). The drawable is the zoomed editor in
        // device px, and the logical height is the Panel's (the editor's own at 100 %).
        const int logicalH = logicalH_;
        float dpi = logicalH > 0 ? static_cast<float>(physH_) / static_cast<float>(logicalH) : 1.0f;
        if (!(dpi > 0.05f)) dpi = 1.0f;
        info.dpi = dpi;
        info.clear = theme_.ground;
        info.textGamma = theme_.textGamma;
        info.theme = themeIdx_;
        info.seconds = static_cast<float>(seconds_);
        info.frame = ticks_;
        info.dt = tickDt;
        info.fixedClock = capture_.fixedDt > 0.0f;
        info.displayLinked = pump.isDisplayLinked();
        info.fps = info.fixedClock ? 0.0f : pump.measuredFps();
        info.fullRate = pump.wantedFullRate();
        info.overflows = diag_.overflows;
        canvas_.begin(info);
        panel_->draw(canvas_, theme_);
        const PrimList& list = canvas_.end();

        // 8. The capture dump, once, of frame CANVAS_DUMP_AFTER.
        if (captureMode_ && !dumpAttempted_ && diag_.frames >= static_cast<uint32_t>(capture_.canvasDumpAfter))
        {
            dumpAttempted_ = true;
            const bool ok = writeCanvasDump(list);
            gpuLog(juce::String(ok ? "CANVAS_DUMP: wrote " : "CANVAS_DUMP: could not write ") + capture_.canvasDump
                   + " (frame " + juce::String(static_cast<int64_t>(diag_.frames)) + ")");
        }

        // 9. Submit. Deliberately no bgfx::frame() here: it presents every view at once, so the pump calls it once
        //    after all open editors have submitted.
        if (sink_.submit(list, ctx.viewIdFor(renderView_), ctx.framebufferFor(renderView_), physW_, physH_)
            == BgfxSink::Result::droppedOverflow)
            onOverflow();
        ++diag_.frames;
        result.submitted = true;

        // 10. Accessibility: structure on a revision change, values at <= 10 Hz; after the first frame, its dump
        //     (02 §5.1: the tree as it stands once the replayed keys have been ticked and drawn).
        if (firstFrame || panel_->a11yRevision() != a11yRevision_ || wallSeconds() - a11ySyncedAt_ >= kA11yValuePeriod)
            syncAccessibility();
        if (firstFrame && !capture_.a11yDump.empty() && !writeA11yDump(capture_.a11yDump))
            gpuLog("A11Y_DUMP: could not write " + juce::String(capture_.a11yDump));

        // 11. Full rate only while something moves.
        result.wantsFullRate = panel_->wantsFullRate();
        return result;
    }

    void EditorHost::onOverflow()
    {
        ++diag_.overflows;
        if (!overflowLogged_ && capture_.gpuLog)
        {
            overflowLogged_ = true;
            gpuLog("overflow: frame dropped, " + juce::String(static_cast<int64_t>(sink_.overflowCount()))
                   + " so far; transient VB " + juce::String(static_cast<int64_t>(BgfxContext::get().transientVbBytes()))
                   + " B shared by every editor in the process");
        }
        jassertfalse;                                // the transient budget (02 §4.5) should make this unreachable
    }

    void EditorHost::syncAccessibility()
    {
        a11yItems_.clear();
        panel_->accessibility(a11yItems_);
        a11y_.sync(*this, a11yItems_, *panel_);
        a11yRevision_ = panel_->a11yRevision();
        a11ySyncedAt_ = wallSeconds();
    }

    bool EditorHost::writeA11yDump(const std::string& path) const
    {
        return writeAtomically(path, [this](std::FILE* f) {
            bool ok = true;
            for (const A11yItem& item : a11y_.items())
                if (item.visible)
                {
                    const std::string line = a11yDumpLine(item);
                    ok = ok && std::fputs(line.c_str(), f) >= 0 && std::fputc('\n', f) != EOF;
                }
            return ok;
        });
    }

    bool EditorHost::writeCanvasDump(const PrimList& list) const
    {
        return writeAtomically(capture_.canvasDump, [&list](std::FILE* f) { return list.writeText(f); });
    }

    void EditorHost::timerCallback()
    {
        panel_->idle(nowSeconds());
    }

    //==================================================================================================================
    // Input (HR :1720-2047): JUCE events -> the Panel's plain structs. The widgets do the hit tests, gestures and host
    // menus (A §3.2-3.7); this only converts.
    //==================================================================================================================

    // Editor px -> the Panel's logical px (G7c): divided by the zoom the editor is sized at (exact at 100 %).
    PointerEvent EditorHost::pointer(const juce::MouseEvent& e) const
    {
        PointerEvent p;
        p.x = e.position.x / zoomScale_;
        p.y = e.position.y / zoomScale_;
        p.mods = modsFrom(e.mods);
        p.clicks = e.getNumberOfClicks();
        p.popup = e.mods.isPopupMenu();              // right-click or ctrl-click: the host menu, never a write
        return p;
    }

    void EditorHost::updateCursor()
    {
        using MC = juce::MouseCursor;
        switch (panel_->cursor())
        {
            case Cursor::normal:       setMouseCursor(MC::NormalCursor); break;
            case Cursor::leftRight:    setMouseCursor(MC::LeftRightResizeCursor); break;
            case Cursor::upDown:       setMouseCursor(MC::UpDownResizeCursor); break;
            case Cursor::pointingHand: setMouseCursor(MC::PointingHandCursor); break;
            case Cursor::crosshair:    setMouseCursor(MC::CrosshairCursor); break;
        }
    }

    void EditorHost::mouseMove(const juce::MouseEvent& e)
    {
        // The panel idles at a low refresh rate; ask for the full one the moment the pointer arrives rather than one
        // frame later (HR :1722-1724).
        nudgeFullRate();
        panel_->pointerMove(pointer(e));
        updateCursor();
    }

    void EditorHost::mouseExit(const juce::MouseEvent&)
    {
        panel_->pointerExit();
        updateCursor();
    }

    void EditorHost::mouseDown(const juce::MouseEvent& e)
    {
        nudgeFullRate();
        dragSource_ = e.source;                      // a widget that starts a drag asks for unbounded movement
        panel_->pointerDown(pointer(e));
        updateCursor();
    }

    void EditorHost::mouseDrag(const juce::MouseEvent& e)
    {
        nudgeFullRate();
        panel_->pointerDrag(pointer(e));
        updateCursor();
    }

    void EditorHost::mouseUp(const juce::MouseEvent& e)
    {
        panel_->pointerUp(pointer(e));
        if (unbounded_)
            setUnboundedDrag(false);                 // the widget ended its drag (or never started one)
        dragSource_.reset();
        updateCursor();
    }

    void EditorHost::mouseDoubleClick(const juce::MouseEvent& e)
    {
        nudgeFullRate();
        panel_->doubleClick(pointer(e));
        updateCursor();
    }

    void EditorHost::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
    {
        WheelEvent w;
        w.x = e.position.x / zoomScale_;
        w.y = e.position.y / zoomScale_;
        w.dx = wheel.deltaX;
        w.dy = wheel.deltaY;
        w.smooth = wheel.isSmooth;
        w.reversed = wheel.isReversed;
        w.inertial = wheel.isInertial;
        w.mods = modsFrom(e.mods);
        nudgeFullRate();
        // Hit-tested by the widgets: scrolling the host window with the pointer anywhere else over the plugin must
        // not write automation (HR :1887-1890), so an unconsumed wheel goes to the parent.
        const bool used = panel_->wheel(w);
        updateCursor();
        if (!used)
            juce::Component::mouseWheelMove(e, wheel);
    }

    bool EditorHost::keyPressed(const juce::KeyPress& k)
    {
        using KP = juce::KeyPress;
        KeyEvent ev;
        ev.mods = modsFrom(k.getModifiers());
        const int code = k.getKeyCode();
        if (code == KP::tabKey)                ev.key = Key::tab;
        else if (code == KP::upKey)            ev.key = Key::up;
        else if (code == KP::downKey)          ev.key = Key::down;
        else if (code == KP::leftKey)          ev.key = Key::left;
        else if (code == KP::rightKey)         ev.key = Key::right;
        else if (code == KP::pageUpKey)        ev.key = Key::pageUp;
        else if (code == KP::pageDownKey)      ev.key = Key::pageDown;
        else if (code == KP::homeKey)          ev.key = Key::home;
        else if (code == KP::endKey)           ev.key = Key::end;
        else if (code == KP::escapeKey)        ev.key = Key::escape;
        else if (code == KP::returnKey)        ev.key = Key::enter;
        else if (code == KP::backspaceKey)     ev.key = Key::backspace;
        else if (code == KP::deleteKey)        ev.key = Key::del;
        else if (code == KP::spaceKey)
        {
            ev.key = Key::space;
            ev.ch = U' ';
        }
        else
        {
            // A character arrives as the OS delivers it: its text, or with cmd held no text and the (lower-case) key
            // code, which is what HeadlessHost::keys gives for the same token.
            const juce::juce_wchar text = k.getTextCharacter();
            char32_t ch = 0;
            if (text != 0)
                ch = static_cast<char32_t>(text);
            else if (code >= 0x20 && code < 0x7f)
                ch = static_cast<char32_t>(code);
            if (ch == 0)
                return false;
            ev.key = Key::character;
            ev.ch = ch;
        }
        nudgeFullRate();
        const bool used = panel_->key(ev);           // true keeps Logic and Live from swallowing the key (HR :1987)
        updateCursor();
        return used;
    }

    // UI_KEYS: HeadlessHost::keys's grammar (HR replayKeys :1993-2036 plus home, end, pageup, pagedown and ctrl+),
    // delivered through keyPressed() as real juce::KeyPresses, so nothing is bypassed.
    void EditorHost::replayKeys(const std::string& spec)
    {
        juce::StringArray tokens;
        tokens.addTokens(juce::String::fromUTF8(spec.c_str()), ",", "");
        for (auto t : tokens)
        {
            t = t.trim();
            int mods = 0;
            for (;;)
            {
                const auto lower = t.toLowerCase();
                if (lower.startsWith("shift+"))      mods |= juce::ModifierKeys::shiftModifier;
                else if (lower.startsWith("cmd+"))   mods |= juce::ModifierKeys::commandModifier;
                else if (lower.startsWith("alt+"))   mods |= juce::ModifierKeys::altModifier;
                else if (lower.startsWith("ctrl+"))  mods |= juce::ModifierKeys::ctrlModifier;
                else break;
                t = t.fromFirstOccurrenceOf("+", false, false);
            }
            const auto name = t.toLowerCase();
            using KP = juce::KeyPress;
            int code = 0;
            juce::juce_wchar text = 0;
            if (name == "tab")            code = KP::tabKey;
            else if (name == "up")        code = KP::upKey;
            else if (name == "down")      code = KP::downKey;
            else if (name == "left")      code = KP::leftKey;
            else if (name == "right")     code = KP::rightKey;
            else if (name == "escape")    code = KP::escapeKey;
            else if (name == "return")    code = KP::returnKey;
            else if (name == "backspace") code = KP::backspaceKey;
            else if (name == "delete")    code = KP::deleteKey;
            else if (name == "home")      code = KP::homeKey;
            else if (name == "end")       code = KP::endKey;
            else if (name == "pageup")    code = KP::pageUpKey;
            else if (name == "pagedown")  code = KP::pageDownKey;
            else if (name == "space")
            {
                code = KP::spaceKey;
                text = ' ';
            }
            else if (t.length() == 1)
            {
                text = t[0];
                code = static_cast<int>(juce::CharacterFunctions::toLowerCase(text));
                if ((mods & juce::ModifierKeys::commandModifier) != 0)
                    text = 0;
            }
            else
                continue;                            // unknown tokens are skipped, as HR skipped them
            keyPressed(KP(code, juce::ModifierKeys(mods), text));
        }
    }

    bool EditorHost::isInterestedInFileDrag(const juce::StringArray& files)
    {
        return panel_->filesInterest(pathsOf(files));
    }

    void EditorHost::filesDropped(const juce::StringArray& files, int, int)
    {
        panel_->filesDropped(pathsOf(files));
        nudgeFullRate();
    }

    // JUCE sends these only for a drag isInterestedInFileDrag() accepted; a drop ends the drag without an exit.
    void EditorHost::fileDragEnter(const juce::StringArray& files, int x, int y)
    {
        panel_->filesDragEnter(pathsOf(files), static_cast<float>(x) / zoomScale_, static_cast<float>(y) / zoomScale_);
        nudgeFullRate();
    }

    void EditorHost::fileDragMove(const juce::StringArray&, int x, int y)
    {
        panel_->filesDragMove(static_cast<float>(x) / zoomScale_, static_cast<float>(y) / zoomScale_);
        nudgeFullRate();
    }

    void EditorHost::fileDragExit(const juce::StringArray&)
    {
        panel_->filesDragExit();
        nudgeFullRate();
    }

    //==================================================================================================================
    // HostServices
    //==================================================================================================================

    void EditorHost::setUnboundedDrag(bool on)
    {
        // Infinite drag range with the pointer hidden and restored on release, so a long sweep cannot walk the cursor
        // off the control or off the screen (HR :1822-1823). Never during a capture, which takes no pointer.
        if (on)
        {
            if (!captureMode_ && dragSource_.has_value() && dragSource_->canDoUnboundedMovement())
            {
                dragSource_->enableUnboundedMouseMovement(true, true);
                unbounded_ = true;
            }
            return;
        }
        if (unbounded_ && dragSource_.has_value())
            dragSource_->enableUnboundedMouseMovement(false, false);
        unbounded_ = false;
    }

    void EditorHost::showParamMenu(ParamPort& port, float x, float y)
    {
        // Right-click and ctrl-click never move a parameter: the host's own menu brings automation, MIDI learn and
        // typed entry for free (HR :1768-1778). No host context (a Standalone, the gallery) means no menu.
        if (captureMode_) return;
        auto* host = getHostContext();
        auto* param = static_cast<juce::RangedAudioParameter*>(port.native());
        if (host == nullptr || param == nullptr) return;
        // The Panel's logical px back to the editor's (G7c): the menu opens where the pointer is.
        if (auto menu = host->getContextMenuForParameter(param))
            menu->showNativeMenu({ juce::roundToInt(x * zoomScale_), juce::roundToInt(y * zoomScale_) });
    }

    void EditorHost::nudgeFullRate()
    {
        FramePump::get().nudgeFullRate();
    }

    double EditorHost::nowSeconds() const
    {
        return capture_.fixedDt > 0.0f ? seconds_ : wallSeconds();
    }

    void EditorHost::beginBatch()
    {
        if (config_.beginBatch)
            config_.beginBatch();
    }

    void EditorHost::endBatch()
    {
        if (config_.endBatch)
            config_.endBatch();
    }

    // What the next frame draws with. Without the override, submitFrame's step 1 sets themeIdx_ to the preference
    // before the Panel is ticked, so a preference written since the last frame is already the answer here.
    int EditorHost::themeIndex() const
    {
        return capture_.uiTheme >= 0 ? themeIdx_ : UiPreferences::get().theme();
    }

    juce::Component* EditorHost::ownerComponent()
    {
        return this;
    }

    //==================================================================================================================
    // Zoom (G7c; EditorHost.h "Zoom", HostServices.h)
    //==================================================================================================================

    int EditorHost::zoomPercent() const
    {
        return zoomTarget_;
    }

    std::span<const int> EditorHost::zoomSteps() const
    {
        return zoomSteps_;
    }

    void EditorHost::setZoomPercent(int percent)
    {
        if (!isZoomStep(percent))
            return;                                  // not a step this product offers: nothing written, nothing moves
        if (config_.zoomPrefKey != nullptr && config_.zoomPrefKey[0] != '\0')
            UiPreferences::get().setInt(config_.zoomPrefKey, percent);   // every editor follows its revision
        zoomChosen_ = percent;
        updateZoomTarget();
        nudgeFullRate();                             // the next frame, soon, applies it
    }

    bool EditorHost::zoomFits(int percent) const
    {
        if (!isZoomStep(percent))
            return false;
        return zoomPin_ > 0 || fitZoom(percent) == percent;   // fitZoom: the largest step <= percent that fits
    }

    bool EditorHost::isZoomStep(int percent) const noexcept
    {
        return std::binary_search(zoomSteps_.begin(), zoomSteps_.end(), percent);
    }

    void EditorHost::initZoom()
    {
        for (const int s : config_.zoomSteps)
            if (s >= kMinZoomPercent && s <= kMaxZoomPercent)
                zoomSteps_.push_back(s);
        std::sort(zoomSteps_.begin(), zoomSteps_.end());
        zoomSteps_.erase(std::unique(zoomSteps_.begin(), zoomSteps_.end()), zoomSteps_.end());
        // Steps are percent in 25..400, ascending, each once, and the default is one of them.
        jassert(zoomSteps_ == config_.zoomSteps);

        if (!zoomSteps_.empty())
        {
            const int want = config_.defaultZoomPercent;
            zoomDefault_ = zoomSteps_.front();
            for (const int s : zoomSteps_)           // ascending: a tie keeps the smaller step
                if (std::abs(s - want) < std::abs(zoomDefault_ - want))
                    zoomDefault_ = s;
            jassert(zoomDefault_ == want);
        }

        zoomPin_ = capture_.uiZoom > 0 ? capture_.uiZoom : (captureMode_ ? 100 : 0);
        zoomChosen_ = readZoomPreference();
        updateZoomTarget();
    }

    int EditorHost::readZoomPreference() const
    {
        if (zoomSteps_.empty())
            return 100;
        if (config_.zoomPrefKey == nullptr || config_.zoomPrefKey[0] == '\0')
            return zoomDefault_;                     // not persisted: every editor opens at the default
        const int v = UiPreferences::get().getInt(config_.zoomPrefKey, zoomDefault_, std::numeric_limits<int>::min(),
                                                  std::numeric_limits<int>::max());
        return isZoomStep(v) ? v : zoomDefault_;
    }

    void EditorHost::followZoomPreference()
    {
        if (zoomSteps_.empty() || config_.zoomPrefKey == nullptr || config_.zoomPrefKey[0] == '\0')
            return;
        const int chosen = readZoomPreference();
        if (chosen == zoomChosen_)
            return;
        zoomChosen_ = chosen;
        updateZoomTarget();
    }

    void EditorHost::updateZoomTarget()
    {
        if (zoomPin_ > 0)
            zoomTarget_ = zoomPin_;
        else if (zoomSteps_.empty())
            zoomTarget_ = 100;
        else
            zoomTarget_ = fitZoom(zoomChosen_);
    }

    // round(W * z), halves away from zero (std::lround; juce::roundToInt would round 1312.5 to even).
    int EditorHost::zoomedSize(int logical, int percent) const noexcept
    {
        return percent == 100 ? logical : static_cast<int>(std::lround(static_cast<double>(logical) * percent / 100.0));
    }

    juce::Rectangle<int> EditorHost::fitArea() const
    {
        const auto& displays = juce::Desktop::getInstance().getDisplays();
        const juce::Displays::Display* d = getPeer() != nullptr
                                             ? displays.getDisplayForRect(getScreenBounds())
                                             : displays.getDisplayForPoint(juce::Desktop::getMousePosition());
        if (d == nullptr)
            d = displays.getPrimaryDisplay();
        return d != nullptr ? d->userArea : juce::Rectangle<int>();
    }

    int EditorHost::fitZoom(int chosen) const
    {
        const auto area = fitArea();
        if (area.isEmpty())
            return chosen;                           // no display known: nothing to fit
        int best = 0;
        for (const int s : zoomSteps_)               // ascending
        {
            if (s > chosen)
                break;
            if (zoomedSize(logicalW_, s) <= area.getWidth() && zoomedSize(logicalH_, s) <= area.getHeight())
                best = s;
        }
        return best > 0 ? best : zoomSteps_.front();
    }

    void EditorHost::refitZoom()
    {
        if (zoomPin_ > 0 || zoomSteps_.empty())
            return;
        updateZoomTarget();
        if (zoomTarget_ != zoomApplied_)
            applyZoom();
    }

    void EditorHost::applyZoom()
    {
        const int before = zoomApplied_;
        zoomApplied_ = zoomTarget_;
        zoomScale_ = static_cast<float>(zoomApplied_) / 100.0f;
        a11y_.setScale(zoomScale_);
        const int w = zoomedSize(logicalW_, zoomApplied_);
        const int h = zoomedSize(logicalH_, zoomApplied_);
        if (w == getWidth() && h == getHeight())
            return;
        // resized() moves the render view and resizes the drawable at once; the host follows the editor's size.
        setSize(w, h);
        if (before != zoomApplied_)
            gpuLog("zoom: " + juce::String(zoomApplied_) + " % (" + juce::String(w) + " x " + juce::String(h)
                   + "), chosen " + juce::String(zoomChosen_) + " %" + (zoomPin_ > 0 ? ", pinned" : ""));
    }
}

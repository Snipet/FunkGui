#pragma once

// Runs a Panel in a plain console process (02 §3.6): a simulated clock, synthesised input, frames recorded into an
// owned Canvas over FontService's atlas, and every HostServices call logged for asserts. With a fixed dt everything is
// deterministic, and settle() ticks until the Panel no longer wants full rate, which the snapping eases make exact.
//
// Declared in G2 (v0.2.0, frozen at FZ1); implemented by G3 (src/panel/HeadlessHost.cpp).

#include <funkgui/a11y/A11yItem.h>
#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/core/Geometry.h>
#include <funkgui/core/Theme.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/panel/Input.h>
#include <funkgui/panel/Panel.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
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
        // One frame into the owned Canvas. Its statistics are the list's: missingGlyphs, and (v0.20.0) images.size()
        // pictures drawn and missingImages Canvas::image calls of ids not registered (canvas/Images.h).
        const PrimList& draw();
        void  move(float x, float y);
        void  click(float x, float y, Mods = {});
        void  doubleClick(float x, float y, Mods = {});
        void  drag(float x0, float y0, float x1, float y1, int steps = 8, Mods = {});
        void  wheel(float x, float y, float dy, bool smooth = false, Mods = {});
        void  keys(const char* spec);          // HR replayKeys grammar + home,end,pageup,pagedown
        std::vector<A11yItem> accessibility() const;
        bool  writeDump(const char* path) const;                     // the last draw()'s frame, dump v2
        bool  writePng (const char* path, int supersample = 2) const;   // SoftRaster: agents can look at frames

        // HostServices calls, for asserts. `batches` counts beginBatch calls; `batchDepth` is the open depth; `zooms`
        // counts setZoomPercent calls, accepted or not (G7c).
        struct Log
        {
            int  menus = 0;
            bool unbounded = false;
            int  nudges = 0;
            int  batches = 0;
            int  batchDepth = 0;
            int  zooms = 0;
            int  resizes = 0;                        // 0.18.0: resize() calls, taken or not
            // Web Sprint B (v0.12.0): the services. `menus` above stays showParamMenu's count. Each call is counted
            // whether or not it was taken, and its request kept until the next one, answered or not.
            int  menuRequests = 0;                   // showMenu
            int  menuDismissals = 0;                 // dismissMenus
            int  fileRequests = 0;                   // chooseFiles
            int  copies = 0;                         // copyText
            MenuRequest lastMenu{};                  // the items, the anchor, the theme
            FileRequest lastFiles{};                 // the mode, the title, the pattern, the suggested name
            std::string lastCopy{};                  // the text
            // FSynth G1: file drops, speech and the screen reader's focus, each in call order.
            int  drops = 0;                          // dropFiles calls, accepted or not
            std::vector<std::string> announcements{};   // announce: every text that is not empty, any priority
            uint32_t a11yFocusId = 0;                // a11yFocus: the last id asked for
            int  a11yFocusCalls = 0;                 // a11yFocus calls
        } log;

        // G7c (v0.8.0): the UI zoom a Panel's ZOOM control reads and writes, simulated. HeadlessHost stays logical:
        // whatever the zoom, it draws, takes input and lists accessibility in the Panel's own px at the constructor's
        // dpi, so a frame never depends on it (the zoom scales only EditorHost's window). setZoom sets what
        // zoomSteps() (sorted, duplicates and values <= 0 dropped) and zoomPercent() answer; before any call they are
        // empty and 100, HostServices' defaults (a zoomSteps() span is valid until the next setZoom). setZoomPercent(p)
        // is counted in log.zooms and taken when p is a step.
        void setZoom(std::vector<int> steps, int percent);
        // Lead (v0.8.0): the largest step zoomFits() accepts (a simulated display); 0 (the default) = every step fits.
        void setZoomFitLimit(int maxPercent) { zoomFitLimit_ = maxPercent; }

        // 0.18.0: the user resizes the window to w x h logical px, as EditorHost delivers it (Panel.h "Resizing"):
        // a resizable Panel gets resize() with the size raised to its minimum, unless that is its size already; a
        // fixed Panel gets nothing. Returns whether resize() was called; counted in log.resizes either way. The next
        // draw() is at the Panel's new width() x height().
        bool resize(int w, int h);

        // Web Sprint B (v0.12.0): the services of HostServices.h, scripted. HeadlessHost reports all three and shows
        // nothing: a showMenu or chooseFiles that was taken is pending until the test answers it with one of the calls
        // below, and the callback runs inside that call, so the next line of the test sees what the Panel did with the
        // answer. Each returns whether a callback was due and the answer was one the user could have given; when it
        // returns false nothing ran and the request is still pending.
        //
        //   host.click(x, y, ctrl);                                       // the Panel calls showMenu
        //   P.eq("menu.shown", host.pendingMenu() != nullptr, 1);         // its items: host.pendingMenu()->items
        //   P.eq("menu.chosen", host.chooseMenuItem("Copy A to B"), 1);   // the callback has run
        //
        // The pending request (&log.lastMenu, &log.lastFiles), or nullptr when nothing waits for an answer.
        const MenuRequest* pendingMenu() const noexcept { return menuPending_ ? &log.lastMenu : nullptr; }
        const FileRequest* pendingFiles() const noexcept { return filesPending_ ? &log.lastFiles : nullptr; }
        // The user chooses an item: by id, or by label (the first item with exactly that label). False for an item
        // the menu does not hold, a disabled one or a separator.
        bool chooseMenuItem(int id);
        bool chooseMenuItem(std::string_view label);
        bool cancelMenu();                           // the user dismisses the menu: its callback runs with 0
        // The user picks files. False for no path (that is cancelFiles), or for several when the request's mode is
        // not openMany. Mode::save: the path is given the pattern's extension, by HostServices::chooseFiles' rule.
        bool returnFiles(std::vector<std::string> paths);
        bool cancelFiles();                          // the user cancels the chooser: its callback runs with no path
        // What commandKeyIsMeta() answers; until a call, HostServices' default (the platform), so a probe whose rows
        // print the command key's name sets it.
        void setCommandKeyIsMeta(bool meta) { commandKeyIsMeta_ = meta; }

        // ---- FSynth G1 additions: a drag a probe can draw half-way, file drops, the clipboard, the cursor -----------
        // drag() stays atomic (move, down, drags, up). press, moveTo and release split it, so draw() can run between
        // the steps and a probe can pin a frame in the middle of a drag:
        //
        //   host.press(40, 20);                 // pointerMove, pointerDown
        //   host.moveTo(120, 60, 4);            // four pointerDrag events along the line
        //   const PrimList& mid = host.draw();  // the Panel as it is while the hand is down
        //   host.release();                     // pointerUp at (120, 60)
        //
        // press = pointerMove then pointerDown at (x, y) with the Mods (clicks 1; ctrl = a popup press, as click()).
        // moveTo = `steps` events (at least 1) along the line from pointer() to (x, y), the last exactly at (x, y):
        // pointerDrag with the press's Mods while pressed, pointerMove with none otherwise. release = pointerUp at
        // pointer() with the press's Mods; nothing when not pressed. A second press while pressed releases first.
        // press(x0, y0, m), moveTo(x1, y1, steps), release() send the events of drag(x0, y0, x1, y1, steps, m), bit
        // for bit. click(), doubleClick() and drag() release an open press first (its pointerUp at pointer(), as
        // release() sends it), then send their own events, which end with their own pointerUp; move() and wheel()
        // leave a press open. A press still open when the host is destroyed is closed by Panel::closeGestures(), as
        // a live one is. No input ticks the clock. The same on every platform, without JUCE and as wasm32.
        void  press(float x, float y, Mods = {});
        void  moveTo(float x, float y, int steps = 1);
        void  release();
        bool  pressed() const noexcept { return pressed_; }
        Point pointer() const noexcept { return pointer_; }   // the last position a pointer or wheel event was sent at;
                                                              // (0, 0) before any

        // A file drag as EditorHost forwards one: Panel::filesInterest(paths); when that accepts,
        // filesDragEnter(paths, x, y), filesDragMove(x, y) and filesDropped(paths). Returns filesInterest's answer; a
        // refused drag sends nothing else. Counted in log.drops either way. It is not pointer input: pointer() and
        // pressed() do not change.
        bool  dropFiles(std::vector<std::string> paths, float x, float y);

        // The in-memory clipboard that clipboardText() answers. setClipboard is the user copying somewhere else;
        // copyText (v0.12.0) sets it too and is logged as before (log.copies, log.lastCopy). No system clipboard is
        // touched, in either direction.
        void  setClipboard(std::string text);

        Cursor cursor() const;                       // Panel::cursor() as it stands after the last event

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
        // G7c (v0.8.0): see setZoom().
        int    zoomPercent() const override;
        void   setZoomPercent(int percent) override;
        std::span<const int> zoomSteps() const override;
        bool   zoomFits(int percent) const override;   // a listed step <= setZoomFitLimit (any, when 0)
        // Web Sprint B (v0.12.0): logged, and pending until answered (see pendingMenu() above). The destructor drops
        // what is still pending before it closes the Panel's gestures.
        unsigned services() const override;
        bool   showMenu(const MenuRequest&, MenuCallback) override;
        void   dismissMenus() override;
        bool   chooseFiles(const FileRequest&, FilesCallback) override;
        bool   copyText(std::string_view utf8) override;
        bool   commandKeyIsMeta() const override;
        // FSynth G1: the in-memory clipboard (setClipboard above); speech and the screen reader's focus are logged
        // (log.announcements; log.a11yFocusId, log.a11yFocusCalls) and nothing is spoken or moved.
        std::string clipboardText() const override;
        void   announce(std::string_view text, Announce priority) override;
        void   a11yFocus(uint32_t itemId) override;

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
        std::vector<int> zoomSteps_;                 // G7c: setZoom()
        int      zoomPercent_ = 100;
        int      zoomFitLimit_ = 0;                  // setZoomFitLimit; 0 = every step fits
        MenuCallback  menuDone_;                     // Web Sprint B: the pending menu's callback (may be empty)
        FilesCallback filesDone_;                    // and the pending chooser's
        bool     menuPending_ = false;
        bool     filesPending_ = false;
        bool     commandKeyIsMeta_ = false;          // the constructor sets HostServices' default
        std::string clipboard_;                      // FSynth G1: setClipboard(), copyText()
        Point    pointer_{};                         // the last position sent
        Mods     pressMods_{};                       // the Mods of the press in progress
        bool     pressed_ = false;                   // between press() and release()
    };
}

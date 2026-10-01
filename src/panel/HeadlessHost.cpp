#include <funkgui/panel/HeadlessHost.h>

#include <funkgui/canvas/SoftRaster.h>
#include <funkgui/text/FontService.h>
#include <funkgui/text/TextFit.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

// A Panel in a console process (02 §3.6): a simulated clock, synthesised input, frames recorded into an owned Canvas
// over FontService's CPU-baked atlas, and a log of the HostServices calls. Choices where 02 §3.6 is silent (G3):
//
// - tick(frames, dt): per frame, Panel::tick(dt), then the clock advances by dt (nowSeconds() is the time the next
//   frame is drawn at), the frame counter by 1, then Panel::idle(now) (the host's >= 10 Hz idle call, here every frame,
//   which is what closes wheel gestures in a probe).
// - settle(maxFrames, dt) ticks one frame at a time while the Panel wants full rate and returns the frames it ticked:
//   0 when it already did not, maxFrames + 1 when it still does after maxFrames (not settled, so a probe's
//   le("settle", host.settle(), 600) row fails, which a return of 600 or -1 would not).
// - Pointer input is JUCE's sequence as EditorHost will forward it: click = move, down, up; doubleClick = move,
//   down (clicks 1), up, down (clicks 2), doubleClick (clicks 2), up (clicks 2); drag = move, down at the start,
//   `steps` drags along the line (the last at the end), up at the end; wheel = move, then the wheel event (dx 0, not
//   reversed, not inertial). A ctrl click is a popup click (JUCE's isPopupMenu() on macOS). No input ticks the clock.
// - keys(spec): HR's replayKeys grammar (BgfxEditor.cpp:1988-2035) plus home, end, pageup, pagedown and a ctrl+
//   modifier: comma-separated tokens, each "[shift+|cmd+|alt+|ctrl+]*<name|char>" (modifiers and names
//   case-insensitive, surrounding spaces ignored); names tab up down left right escape return backspace delete space
//   home end pageup pagedown; a single UTF-8 character is Key::character with ch = that character, or its lower case
//   with cmd held (HR sends the key code and no text then). Unknown tokens are skipped, as HR skipped them.
// - draw(): FrameInfo is the panel's size, the host's dpi and theme (its ground as the clear colour and its text
//   gamma), seconds = the simulated clock, frame = frames ticked, dt = the last tick's dt, clock fixed, fps 0,
//   rate = wantsFullRate(), no overflows.
// - writeDump() writes the last draw()'s frame and returns false when nothing was drawn yet. writePng() rasterises that
//   same frame with canvas/SoftRaster (G4) over FontService's atlas at the frame's physical size (logical size * dpi),
//   `supersample` samples per physical pixel and axis (clamped to 1..4), and returns false when nothing was drawn yet
//   or the file cannot be written; `funkgui_framerender <dump> <png> [ss]` gives the same picture from the dump.
// - The destructor calls Panel::closeGestures() (EditorHost's rule when the host closes the editor mid-gesture), so
//   the Panel must outlive its host.
// - Zoom (G7c): simulated for a Panel's ZOOM control only; nothing drawn, hit-tested or listed depends on it.
// - Services (Web Sprint B, v0.12.0): every call is counted and its request kept in `log` before it is judged, so a
//   refused request can be read too. A request replaces the pending one of its kind, whose callback is destroyed
//   unrun, whether or not it is taken itself (pendingMenu() is always log.lastMenu or nothing). A menu is refused by
//   HostServices::showMenu's rule, as EditorHost refuses it (no item that is not a separator, or one with an id <= 0),
//   so a probe cannot pass on a menu the live host would not open. An answer first takes the callback out of the host
//   and clears the pending state, then calls it:
//   the callback may ask for the next menu or chooser, which is then pending when the answering call returns. An
//   empty callback is taken like any other (the request is pending, the answer returns true and calls nothing). The
//   destructor destroys the pending callbacks, then closes the Panel's gestures, so nothing a Panel does while its
//   host goes can reach one. Mode::save's extension rule is JUCE's File::hasFileExtension and withFileExtension, as
//   EditorHost applies them: a path that does not end in the extension (ASCII case ignored) loses what follows the
//   last '.' of its file name and gains the extension.

namespace funkgui
{
    namespace
    {
        PointerEvent pointer(float x, float y, Mods m, int clicks)
        {
            PointerEvent e;
            e.x = x;
            e.y = y;
            e.mods = m;
            e.clicks = clicks;
            e.popup = m.ctrl;
            return e;
        }

       #if defined(_WIN32)
        constexpr std::string_view kSeparators = "\\/";   // juce::File's, per platform
       #else
        constexpr std::string_view kSeparators = "/";
       #endif

        // The extension a save must end in: ".ext" for a pattern that is a single "*.ext", else "" (no rule).
        std::string_view saveExtension(std::string_view pattern)
        {
            if (pattern.size() < 3 || pattern[0] != '*' || pattern[1] != '.')
                return {};
            const std::string_view ext = pattern.substr(1);
            return ext.find_first_of("*?;, ") == std::string_view::npos ? ext : std::string_view{};
        }

        bool endsWithNoCase(std::string_view s, std::string_view suffix)
        {
            if (s.size() < suffix.size())
                return false;
            const auto low = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
            const size_t at = s.size() - suffix.size();
            for (size_t i = 0; i < suffix.size(); ++i)
                if (low(s[at + i]) != low(suffix[i]))
                    return false;
            return true;
        }

        // `path` ending in `ext` (".ext"): unchanged when it does already, else what follows the last '.' of its file
        // name is replaced by it, or it is appended.
        std::string withExtension(std::string path, std::string_view ext)
        {
            if (ext.empty() || endsWithNoCase(path, ext))
                return path;
            const size_t slash = path.find_last_of(kSeparators);
            const size_t dot = path.rfind('.');
            if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
                path.erase(dot);
            path.append(ext);
            return path;
        }

        bool startsWithNoCase(std::string_view s, std::string_view prefix)
        {
            if (s.size() < prefix.size())
                return false;
            for (size_t i = 0; i < prefix.size(); ++i)
            {
                char c = s[i];
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
                if (c != prefix[i])
                    return false;
            }
            return true;
        }

        std::string lower(std::string_view s)
        {
            std::string out(s);
            for (char& c : out)
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
            return out;
        }

        std::string_view trim(std::string_view s)
        {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
                s.remove_prefix(1);
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
                s.remove_suffix(1);
            return s;
        }

        // One replay token as a key event; false for a token HR's grammar does not name.
        bool keyFromToken(std::string_view t, KeyEvent& ev)
        {
            ev = KeyEvent{};
            for (;;)
            {
                if (startsWithNoCase(t, "shift+"))     ev.mods.shift = true;
                else if (startsWithNoCase(t, "cmd+"))  ev.mods.cmd = true;
                else if (startsWithNoCase(t, "alt+"))  ev.mods.alt = true;
                else if (startsWithNoCase(t, "ctrl+")) ev.mods.ctrl = true;
                else break;
                t.remove_prefix(t.find('+') + 1);
            }
            const std::string name = lower(t);
            struct Named { const char* name; Key key; };
            static constexpr Named kNamed[] = {
                { "tab", Key::tab },       { "up", Key::up },         { "down", Key::down },
                { "left", Key::left },     { "right", Key::right },   { "escape", Key::escape },
                { "return", Key::enter },  { "backspace", Key::backspace }, { "delete", Key::del },
                { "home", Key::home },     { "end", Key::end },       { "pageup", Key::pageUp },
                { "pagedown", Key::pageDown },
            };
            for (const Named& n : kNamed)
                if (name == n.name)
                {
                    ev.key = n.key;
                    return true;
                }
            if (name == "space")
            {
                ev.key = Key::space;
                ev.ch = U' ';
                return true;
            }
            // A single character (one UTF-8 codepoint), delivered as the OS delivers one.
            if (t.empty())
                return false;
            const std::string one(t);
            const char* p = one.c_str();
            const uint32_t cp = text::decodeUtf8(p);
            if (cp == 0 || *p != 0)
                return false;
            ev.key = Key::character;
            ev.ch = static_cast<char32_t>(ev.mods.cmd && cp >= 'A' && cp <= 'Z' ? cp - 'A' + 'a' : cp);
            return true;
        }
    }

    HeadlessHost::HeadlessHost(Panel& panel, int themeIdx, float dpi)
        : panel_(panel), theme_(Theme::byIndex(themeIdx)), themeIdx_(themeIdx), dpi_(dpi),
          canvas_(FontService::get().atlas()), commandKeyIsMeta_(HostServices::commandKeyIsMeta())
    {
        panel_.attach(*this);
    }

    HeadlessHost::~HeadlessHost()
    {
        // The host lets go of the Panel: what is still pending is dropped unrun, before the Panel hears of it.
        menuPending_ = filesPending_ = false;
        menuDone_ = nullptr;
        filesDone_ = nullptr;
        panel_.closeGestures();
    }

    // ---- clock ------------------------------------------------------------------------------------------------------

    void HeadlessHost::tick(int frames, float dt)
    {
        for (int i = 0; i < frames; ++i)
        {
            panel_.tick(dt);
            now_ += static_cast<double>(dt);
            ++frame_;
            lastDt_ = dt;
            panel_.idle(now_);
        }
    }

    int HeadlessHost::settle(int maxFrames, float dt)
    {
        int n = 0;
        while (panel_.wantsFullRate())
        {
            if (n >= maxFrames)
                return maxFrames + 1;                    // not settled
            tick(1, dt);
            ++n;
        }
        return n;
    }

    const PrimList& HeadlessHost::draw()
    {
        FrameInfo info;
        info.logicalW = panel_.width();
        info.logicalH = panel_.height();
        info.dpi = dpi_;
        info.clear = theme_.ground;
        info.textGamma = theme_.textGamma;
        info.theme = themeIdx_;
        info.seconds = static_cast<float>(now_);
        info.frame = frame_;
        info.dt = lastDt_;
        info.fixedClock = true;
        info.fullRate = panel_.wantsFullRate();
        canvas_.begin(info);
        panel_.draw(canvas_, theme_);
        return canvas_.end();
    }

    // ---- input ------------------------------------------------------------------------------------------------------

    void HeadlessHost::move(float x, float y)
    {
        panel_.pointerMove(pointer(x, y, {}, 1));
    }

    void HeadlessHost::click(float x, float y, Mods m)
    {
        const PointerEvent e = pointer(x, y, m, 1);
        panel_.pointerMove(e);
        panel_.pointerDown(e);
        panel_.pointerUp(e);
    }

    void HeadlessHost::doubleClick(float x, float y, Mods m)
    {
        const PointerEvent one = pointer(x, y, m, 1), two = pointer(x, y, m, 2);
        panel_.pointerMove(one);
        panel_.pointerDown(one);
        panel_.pointerUp(one);
        panel_.pointerDown(two);
        panel_.doubleClick(two);
        panel_.pointerUp(two);
    }

    void HeadlessHost::drag(float x0, float y0, float x1, float y1, int steps, Mods m)
    {
        const PointerEvent start = pointer(x0, y0, m, 1);
        panel_.pointerMove(start);
        panel_.pointerDown(start);
        const int n = steps < 1 ? 1 : steps;
        for (int k = 1; k <= n; ++k)
        {
            const float t = static_cast<float>(k) / static_cast<float>(n);
            const float x = k == n ? x1 : x0 + (x1 - x0) * t;
            const float y = k == n ? y1 : y0 + (y1 - y0) * t;
            panel_.pointerDrag(pointer(x, y, m, 1));
        }
        panel_.pointerUp(pointer(x1, y1, m, 1));
    }

    void HeadlessHost::wheel(float x, float y, float dy, bool smooth, Mods m)
    {
        panel_.pointerMove(pointer(x, y, m, 1));
        WheelEvent w;
        w.x = x;
        w.y = y;
        w.dy = dy;
        w.smooth = smooth;
        w.mods = m;
        panel_.wheel(w);
    }

    void HeadlessHost::keys(const char* spec)
    {
        if (spec == nullptr)
            return;
        std::string_view rest(spec);
        while (!rest.empty())
        {
            const size_t comma = rest.find(',');
            const std::string_view token = trim(rest.substr(0, comma));
            rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
            KeyEvent ev;
            if (!token.empty() && keyFromToken(token, ev))
                panel_.key(ev);
        }
    }

    std::vector<A11yItem> HeadlessHost::accessibility() const
    {
        std::vector<A11yItem> items;
        panel_.accessibility(items);
        return items;
    }

    // ---- output -----------------------------------------------------------------------------------------------------

    bool HeadlessHost::writeDump(const char* path) const
    {
        // Canvas::end() only returns the list it recorded into (valid until the next begin), so reading it through a
        // const host changes nothing.
        const PrimList& last = const_cast<Canvas&>(canvas_).end();
        if (path == nullptr || last.info.logicalW <= 0 || last.info.logicalH <= 0)
            return false;                                // nothing drawn yet
        // Written beside the destination and renamed onto it once closed (HR SdfCanvas.cpp:290-319), so a reader that
        // polls for the file never sees half a frame.
        const std::string tmp = std::string(path) + ".partial";
        std::FILE* f = std::fopen(tmp.c_str(), "w");
        if (f == nullptr)
            return false;
        const bool written = last.writeText(f);
        const bool closed = std::fclose(f) == 0;
        if (written && closed && std::rename(tmp.c_str(), path) == 0)
            return true;
        std::remove(tmp.c_str());
        return false;
    }

    bool HeadlessHost::writePng(const char* path, int supersample) const
    {
        const PrimList& last = const_cast<Canvas&>(canvas_).end();   // as writeDump(): reading changes nothing
        if (path == nullptr || last.info.logicalW <= 0 || last.info.logicalH <= 0)
            return false;                                // nothing drawn yet
        const Image img = rasterise(last, FontService::get().atlas(), supersample);
        return img.w > 0 && img.h > 0 && funkgui::writePng(img, path);
    }

    // ---- HostServices -----------------------------------------------------------------------------------------------

    void HeadlessHost::setUnboundedDrag(bool on) { log.unbounded = on; }
    void HeadlessHost::showParamMenu(ParamPort&, float, float) { ++log.menus; }
    void HeadlessHost::nudgeFullRate() { ++log.nudges; }
    double HeadlessHost::nowSeconds() const { return now_; }

    void HeadlessHost::beginBatch()
    {
        ++log.batches;
        ++log.batchDepth;
    }

    void HeadlessHost::endBatch()
    {
        --log.batchDepth;
    }

    // Theme::byIndex draws graphite (0) for an index that names no theme, so 0 is the index of what draw() uses then.
    int HeadlessHost::themeIndex() const
    {
        return themeIdx_ >= 0 && themeIdx_ < Theme::kCount ? themeIdx_ : 0;
    }

    void HeadlessHost::setZoom(std::vector<int> steps, int percent)
    {
        steps.erase(std::remove_if(steps.begin(), steps.end(), [](int s) { return s <= 0; }), steps.end());
        std::sort(steps.begin(), steps.end());
        steps.erase(std::unique(steps.begin(), steps.end()), steps.end());
        zoomSteps_ = std::move(steps);
        zoomPercent_ = percent;
    }

    int HeadlessHost::zoomPercent() const { return zoomPercent_; }

    void HeadlessHost::setZoomPercent(int percent)
    {
        ++log.zooms;
        if (std::find(zoomSteps_.begin(), zoomSteps_.end(), percent) != zoomSteps_.end())
            zoomPercent_ = percent;
    }

    std::span<const int> HeadlessHost::zoomSteps() const { return zoomSteps_; }

    bool HeadlessHost::zoomFits(int percent) const
    {
        return std::find(zoomSteps_.begin(), zoomSteps_.end(), percent) != zoomSteps_.end()
               && (zoomFitLimit_ <= 0 || percent <= zoomFitLimit_);
    }

    // ---- services (Web Sprint B) ------------------------------------------------------------------------------------

    unsigned HeadlessHost::services() const
    {
        return hostservice::menus | hostservice::fileChooser | hostservice::clipboard;
    }

    bool HeadlessHost::showMenu(const MenuRequest& request, MenuCallback done)
    {
        ++log.menuRequests;
        log.lastMenu = request;
        menuPending_ = false;                            // the menu still pending is replaced, its callback unrun,
        menuDone_ = nullptr;                             // even when this request is refused
        bool anyItem = false;                            // refused: an item with no id, or nothing but separators
        for (const MenuItem& it : request.items)
        {
            if (it.separator)
                continue;
            if (it.id <= 0)
                return false;
            anyItem = true;
        }
        if (!anyItem)
            return false;
        menuDone_ = std::move(done);
        menuPending_ = true;
        return true;
    }

    void HeadlessHost::dismissMenus()
    {
        ++log.menuDismissals;
        menuPending_ = false;
        menuDone_ = nullptr;                             // dropped unrun
    }

    bool HeadlessHost::chooseFiles(const FileRequest& request, FilesCallback done)
    {
        ++log.fileRequests;
        log.lastFiles = request;
        filesDone_ = std::move(done);                    // the replaced chooser's callback goes unrun
        filesPending_ = true;
        return true;
    }

    bool HeadlessHost::copyText(std::string_view utf8)
    {
        ++log.copies;
        log.lastCopy.assign(utf8);
        return true;
    }

    bool HeadlessHost::commandKeyIsMeta() const { return commandKeyIsMeta_; }

    bool HeadlessHost::chooseMenuItem(int id)
    {
        if (!menuPending_ || id <= 0)
            return false;
        const auto& items = log.lastMenu.items;
        const auto it = std::find_if(items.begin(), items.end(),
                                     [id](const MenuItem& m) { return !m.separator && m.id == id; });
        if (it == items.end() || !it->enabled)
            return false;
        menuPending_ = false;
        const MenuCallback done = std::exchange(menuDone_, nullptr);
        if (done)
            done(id);
        return true;
    }

    bool HeadlessHost::chooseMenuItem(std::string_view label)
    {
        if (!menuPending_)
            return false;
        for (const MenuItem& m : log.lastMenu.items)
            if (!m.separator && m.label == label)
                return chooseMenuItem(m.id);
        return false;
    }

    bool HeadlessHost::cancelMenu()
    {
        if (!menuPending_)
            return false;
        menuPending_ = false;
        const MenuCallback done = std::exchange(menuDone_, nullptr);
        if (done)
            done(0);
        return true;
    }

    bool HeadlessHost::returnFiles(std::vector<std::string> paths)
    {
        if (!filesPending_ || paths.empty() || (paths.size() > 1 && log.lastFiles.mode != FileRequest::Mode::openMany))
            return false;
        if (log.lastFiles.mode == FileRequest::Mode::save)
            paths.front() = withExtension(std::move(paths.front()), saveExtension(log.lastFiles.pattern));
        filesPending_ = false;
        const FilesCallback done = std::exchange(filesDone_, nullptr);
        if (done)
            done(paths);
        return true;
    }

    bool HeadlessHost::cancelFiles()
    {
        if (!filesPending_)
            return false;
        filesPending_ = false;
        const FilesCallback done = std::exchange(filesDone_, nullptr);
        if (done)
            done({});
        return true;
    }
}

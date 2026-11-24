#include <funkgui/desktop/MenuOverlay.h>

#include <funkgui/canvas/Canvas.h>
#include <funkgui/core/Col.h>
#include <funkgui/text/FontService.h>
#include <funkgui/text/TextFit.h>
#include <funkgui/text/TextStyle.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

// MenuOverlay (v0.28.0; desktop/MenuOverlay.h has the rules). The look is juce/MenuLook.h's, redrawn with the Canvas:
// its metrics, and its palette by the same mixes of the theme's tokens (MenuLook::paletteFor, which needs JUCE's
// header to name, so the arithmetic is repeated here; fg.desktop.menu pins the colours both themes get).

namespace funkgui::desktop
{
    namespace
    {
        constexpr TextStyle kLabel{ MenuOverlay::kTextPx, 0.0f, 0.0f, false };
        constexpr TextStyle kHeader{ MenuOverlay::kHeaderTextPx, 0.9f, 0.03f, false };
        constexpr float kWheelRows = 3.0f;           // a notch of the wheel scrolls this many rows

        struct Palette
        {
            Col surface, edge, hover, text, dim, disabled, rule, mark, shadow;
        };

        float luma(Col c)
        {
            return 0.2126f * static_cast<float>(c.r) + 0.7152f * static_cast<float>(c.g)
                 + 0.0722f * static_cast<float>(c.b);
        }

        Palette paletteFor(const Theme& th)
        {
            constexpr Col white{ 255, 255, 255, 255 }, black{ 0, 0, 0, 255 };
            const bool dark = luma(th.ground) < luma(th.ink100);
            Palette p;
            p.surface = dark ? mix(th.ground, th.ink100, 0.05f) : mix(th.ground, white, 0.72f);
            p.edge = dark ? mix(th.ground, th.ink100, 0.16f) : mix(th.ground, th.ink100, 0.15f);
            p.hover = dark ? mix(th.ground, th.ink100, 0.14f) : mix(p.surface, th.ink100, 0.08f);
            p.text = th.ink100;
            p.dim = th.ink52;
            p.disabled = th.ink32;
            p.rule = dark ? mix(th.ground, th.ink100, 0.12f) : mix(p.surface, th.ink100, 0.11f);
            p.mark = th.ink100;
            p.shadow = black.withAlpha(dark ? 0.5f : 0.2f);
            return p;
        }

        char lower(char c)
        {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        }

        std::string lowered(const std::string& s)
        {
            std::string out = s;
            for (char& c : out)
                c = lower(c);
            return out;
        }

        std::string upper(const std::string& s)
        {
            std::string out = s;
            for (char& c : out)
                if (c >= 'a' && c <= 'z')
                    c = static_cast<char>(c - 'a' + 'A');
            return out;
        }

        // A label less its leading spaces, lower-cased: what the type-ahead compares.
        std::string keyOf(const std::string& label)
        {
            std::size_t at = 0;
            while (at < label.size() && label[at] == ' ')
                ++at;
            return lowered(label.substr(at));
        }

        bool startsWith(const std::string& s, const std::string& prefix)
        {
            return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
        }

        // One code point as UTF-8 (the type-ahead's prefix is text).
        void appendUtf8(std::string& out, char32_t cp)
        {
            if (cp < 0x80u)
                out.push_back(static_cast<char>(cp));
            else if (cp < 0x800u)
            {
                out.push_back(static_cast<char>(0xc0u | (cp >> 6)));
                out.push_back(static_cast<char>(0x80u | (cp & 0x3fu)));
            }
            else if (cp < 0x10000u)
            {
                out.push_back(static_cast<char>(0xe0u | (cp >> 12)));
                out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3fu)));
                out.push_back(static_cast<char>(0x80u | (cp & 0x3fu)));
            }
            else
            {
                out.push_back(static_cast<char>(0xf0u | (cp >> 18)));
                out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3fu)));
                out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3fu)));
                out.push_back(static_cast<char>(0x80u | (cp & 0x3fu)));
            }
        }

        // The next row that can be highlighted from `current` in direction `delta`, wrapping; -1 when there is none.
        int step(const MenuOverlay::Level& level, int current, int delta)
        {
            const int n = static_cast<int>(level.rows.size());
            if (n == 0)
                return -1;
            int at = current < 0 || current >= n ? (delta > 0 ? -1 : n) : current;
            for (int k = 0; k < n; ++k)
            {
                at = ((at + (delta > 0 ? 1 : -1)) % n + n) % n;
                if (level.rows[static_cast<std::size_t>(at)].selectable)
                    return at;
            }
            return -1;
        }

        // The first (or the last) row that can be highlighted.
        int edge(const MenuOverlay::Level& level, bool last)
        {
            const int n = static_cast<int>(level.rows.size());
            for (int k = 0; k < n; ++k)
            {
                const int at = last ? n - 1 - k : k;
                if (level.rows[static_cast<std::size_t>(at)].selectable)
                    return at;
            }
            return -1;
        }
    }

    MenuOverlay::MenuOverlay() = default;
    MenuOverlay::~MenuOverlay() = default;

    float MenuOverlay::textWidth(const std::string& utf8, bool header) const
    {
        return text::width(FontService::get().atlas(), utf8.c_str(), header ? kHeader : kLabel);
    }

    //==================================================================================================================
    // Layout
    //==================================================================================================================

    // The rows and the panel's size; the panel's place is the caller's (placeTop, placeChild).
    void MenuOverlay::buildLevel(Level& level, const std::vector<MenuItem>& items, float minWidth) const
    {
        level = Level{};
        level.items = &items;
        float y = 0.0f, labelW = 0.0f, rightW = 0.0f;
        for (std::size_t i = 0; i < items.size(); ++i)
        {
            const MenuItem& item = items[i];
            Row row;
            row.item = static_cast<int>(i);
            row.y = y;
            if (item.separator)
            {
                row.kind = RowKind::separator;
                row.h = kSeparatorPx;
            }
            else if (item.header)
            {
                row.kind = RowKind::header;
                row.h = kHeaderPx;
                labelW = std::max(labelW, textWidth(upper(item.label), true));
            }
            else
            {
                row.kind = RowKind::item;
                row.h = kRowPx;
                row.submenu = !item.submenu.empty();
                row.selectable = item.enabled;
                labelW = std::max(labelW, textWidth(item.label, false));
                if (row.submenu)
                    rightW = std::max(rightW, kChevronPx);
                else if (!item.shortcut.empty())
                    rightW = std::max(rightW, textWidth(item.shortcut, false));
            }
            y += row.h;
            level.rows.push_back(row);
        }
        level.contentH = y;
        float w = 2.0f * kInsetPx + kLabelPx + labelW + kRowPadPx;
        if (rightW > 0.0f)
            w += kColumnGapPx + rightW;
        w = std::ceil(std::max({ w, kMinPanelPx, minWidth }));
        const float most = std::max(kMinPanelPx, boundsW_ - 2.0f * kEdgePx);
        level.panel.w = std::min(w, most);
        level.panel.h = level.contentH + 2.0f * kPadPx;
    }

    // Inside the bounds less kEdgePx: as tall as they allow at most (the rest scrolls), then moved left and up.
    void MenuOverlay::clampInto(Level& level) const
    {
        const float mostH = std::max(kRowPx + 2.0f * kPadPx, boundsH_ - 2.0f * kEdgePx);
        level.panel.h = std::min(level.contentH + 2.0f * kPadPx, mostH);
        level.panel.x = std::max(kEdgePx, std::min(level.panel.x, boundsW_ - kEdgePx - level.panel.w));
        level.panel.y = std::max(kEdgePx, std::min(level.panel.y, boundsH_ - kEdgePx - level.panel.h));
        const float mostScroll = std::max(0.0f, level.contentH + 2.0f * kPadPx - level.panel.h);
        level.scroll = std::clamp(level.scroll, 0.0f, mostScroll);
    }

    void MenuOverlay::placeTop()
    {
        Level& top = levels_.front();
        const Rect& a = request_.anchor;
        const float fullH = top.contentH + 2.0f * kPadPx;
        const float under = boundsH_ - kEdgePx - (a.bottom() + kGapPx);      // the room under the anchor
        const float over = a.y - kGapPx - kEdgePx;                            // and over it
        top.panel.x = a.x;
        top.panel.y = fullH <= under || under >= over ? a.bottom() + kGapPx : a.y - kGapPx - std::min(fullH, over);
        clampInto(top);
    }

    void MenuOverlay::placeChild(std::size_t index)
    {
        Level& child = levels_[index];
        const Level& parent = levels_[index - 1];
        const Rect row = rowRect(static_cast<int>(index - 1), parent.open);
        const float right = parent.panel.right() - kOverlapPx;
        const float left = parent.panel.x + kOverlapPx - child.panel.w;
        child.panel.x = right + child.panel.w <= boundsW_ - kEdgePx || left < kEdgePx ? right : left;
        child.panel.y = row.y - kPadPx;
        clampInto(child);
    }

    void MenuOverlay::setBounds(float boundsW, float boundsH)
    {
        if (boundsW == boundsW_ && boundsH == boundsH_)
            return;
        boundsW_ = boundsW;
        boundsH_ = boundsH;
        if (levels_.empty())
            return;
        placeTop();
        for (std::size_t i = 1; i < levels_.size(); ++i)
            placeChild(i);
        touch();
    }

    Rect MenuOverlay::rowRect(int level, int row) const
    {
        if (level < 0 || level >= static_cast<int>(levels_.size()))
            return {};
        const Level& l = levels_[static_cast<std::size_t>(level)];
        if (row < 0 || row >= static_cast<int>(l.rows.size()))
            return {};
        const Row& r = l.rows[static_cast<std::size_t>(row)];
        return { l.panel.x + kInsetPx, l.panel.y + kPadPx + r.y - l.scroll, l.panel.w - 2.0f * kInsetPx, r.h };
    }

    void MenuOverlay::scrollIntoView(Level& level, int row)
    {
        if (row < 0 || row >= static_cast<int>(level.rows.size()))
            return;
        const Row& r = level.rows[static_cast<std::size_t>(row)];
        const float viewH = level.panel.h - 2.0f * kPadPx;
        float scroll = level.scroll;
        if (r.y < scroll)
            scroll = r.y;
        else if (r.y + r.h > scroll + viewH)
            scroll = r.y + r.h - viewH;
        level.scroll = std::clamp(scroll, 0.0f, std::max(0.0f, level.contentH - viewH));
    }

    int MenuOverlay::hitLevel(float x, float y) const
    {
        for (std::size_t i = levels_.size(); i-- > 0;)
            if (levels_[i].panel.contains({ x, y }))
                return static_cast<int>(i);
        return -1;
    }

    // The row of `level` under the point, or -1 (the panel's padding, a point outside its rows' strip).
    int MenuOverlay::hitRow(const Level& level, float x, float y) const
    {
        if (x < level.panel.x + kInsetPx || x >= level.panel.right() - kInsetPx)
            return -1;
        const float top = level.panel.y + kPadPx, bottom = level.panel.bottom() - kPadPx;
        if (y < top || y >= bottom)
            return -1;
        const float at = y - top + level.scroll;
        for (std::size_t i = 0; i < level.rows.size(); ++i)
            if (at >= level.rows[i].y && at < level.rows[i].y + level.rows[i].h)
                return static_cast<int>(i);
        return -1;
    }

    //==================================================================================================================
    // Opening and closing
    //==================================================================================================================

    bool MenuOverlay::show(const MenuRequest& request, MenuCallback done, float boundsW, float boundsH)
    {
        dismiss();                                   // a menu still showing is replaced, even by a refused request
        if (!validMenu(request))
            return false;                            // `done` is dropped unrun
        request_ = request;
        done_ = std::move(done);
        boundsW_ = boundsW;
        boundsH_ = boundsH;
        typed_.clear();
        levels_.emplace_back();
        buildLevel(levels_.front(), request_.items, request_.minWidth);
        placeTop();
        if (request_.selectedId > 0)
        {
            Level& top = levels_.front();
            for (std::size_t i = 0; i < top.rows.size(); ++i)
            {
                const MenuItem& item = request_.items[static_cast<std::size_t>(top.rows[i].item)];
                if (top.rows[i].kind == RowKind::item && item.submenu.empty() && item.id == request_.selectedId)
                {
                    if (top.rows[i].selectable)
                        top.highlighted = static_cast<int>(i);
                    scrollIntoView(top, static_cast<int>(i));
                    break;
                }
            }
        }
        touch();
        return true;
    }

    void MenuOverlay::dismiss()
    {
        if (levels_.empty())
            return;
        levels_.clear();
        done_ = nullptr;                             // dropped unrun; what it captured is released
        request_ = MenuRequest{};
        typed_.clear();
        touch();
    }

    // The menu closes, then the callback runs: it may show another menu.
    void MenuOverlay::finish(int id)
    {
        MenuCallback done = std::move(done_);
        dismiss();
        if (done)
            done(id);
    }

    void MenuOverlay::closeFrom(std::size_t level)
    {
        if (level == 0 || level >= levels_.size())
            return;
        levels_.resize(level);
        levels_.back().open = -1;
        touch();
    }

    void MenuOverlay::openChild(std::size_t level, int row, bool highlightFirst)
    {
        if (level >= levels_.size())
            return;
        closeFrom(level + 1);
        Level& parent = levels_[level];
        if (row < 0 || row >= static_cast<int>(parent.rows.size()))
            return;
        const Row& r = parent.rows[static_cast<std::size_t>(row)];
        if (!r.submenu || !r.selectable)
            return;
        const std::vector<MenuItem>& items = (*parent.items)[static_cast<std::size_t>(r.item)].submenu;
        parent.open = row;
        levels_.emplace_back();
        buildLevel(levels_.back(), items, 0.0f);
        placeChild(levels_.size() - 1);
        if (highlightFirst)
            levels_.back().highlighted = edge(levels_.back(), false);
        touch();
    }

    void MenuOverlay::setHighlight(std::size_t level, int row)
    {
        if (level >= levels_.size() || levels_[level].highlighted == row)
            return;
        levels_[level].highlighted = row;
        touch();
    }

    //==================================================================================================================
    // Input
    //==================================================================================================================

    bool MenuOverlay::pointerMove(float x, float y)
    {
        if (levels_.empty())
            return false;
        const int hit = hitLevel(x, y);
        if (hit < 0)
        {
            // Off every panel: the last panel's highlight goes, unless it is the row whose submenu is open.
            Level& last = levels_.back();
            if (last.highlighted != last.open)
                setHighlight(levels_.size() - 1, last.open);
            return true;
        }
        const auto level = static_cast<std::size_t>(hit);
        const int row = hitRow(levels_[level], x, y);
        const bool selectable = row >= 0 && levels_[level].rows[static_cast<std::size_t>(row)].selectable;
        if (!selectable)
        {
            // A separator, a header, a disabled row or the padding: nothing is lit there, and an open submenu stays.
            if (levels_[level].open < 0)
                setHighlight(level, -1);
            return true;
        }
        setHighlight(level, row);
        if (levels_[level].open != row)
        {
            if (levels_[level].rows[static_cast<std::size_t>(row)].submenu)
                openChild(level, row, false);
            else
                closeFrom(level + 1);
        }
        return true;
    }

    bool MenuOverlay::pointerDown(float x, float y)
    {
        if (levels_.empty())
            return false;
        if (hitLevel(x, y) < 0)
            finish(0);                               // a press outside the menu dismisses it
        else
            pointerMove(x, y);
        return true;
    }

    bool MenuOverlay::pointerUp(float x, float y)
    {
        if (levels_.empty())
            return false;
        const int hit = hitLevel(x, y);
        if (hit < 0)
            return true;                             // the release of the press that opened the menu, say
        const Level& level = levels_[static_cast<std::size_t>(hit)];
        const int row = hitRow(level, x, y);
        if (row < 0)
            return true;
        const Row& r = level.rows[static_cast<std::size_t>(row)];
        if (!r.selectable)
            return true;
        if (r.submenu)
        {
            if (level.open != row)
                openChild(static_cast<std::size_t>(hit), row, false);
            return true;
        }
        finish((*level.items)[static_cast<std::size_t>(r.item)].id);
        return true;
    }

    bool MenuOverlay::pointerExit()
    {
        if (levels_.empty())
            return false;
        Level& last = levels_.back();
        if (last.highlighted != last.open)
            setHighlight(levels_.size() - 1, last.open);
        return true;
    }

    bool MenuOverlay::wheel(float x, float y, float dy)
    {
        if (levels_.empty())
            return false;
        const int hit = hitLevel(x, y);
        if (hit < 0)
            return true;
        Level& level = levels_[static_cast<std::size_t>(hit)];
        const float viewH = level.panel.h - 2.0f * kPadPx;
        const float most = std::max(0.0f, level.contentH - viewH);
        // WheelEvent::dy is 50 / 256 a notch (JUCE on X11), positive away from the user: the rows come down.
        const float scroll = std::clamp(level.scroll - dy * (256.0f / 50.0f) * kWheelRows * kRowPx, 0.0f, most);
        if (scroll != level.scroll)
        {
            level.scroll = scroll;
            closeFrom(static_cast<std::size_t>(hit) + 1);    // an open submenu would no longer be beside its row
            touch();
            pointerMove(x, y);                       // another row is under the pointer now
        }
        return true;
    }

    bool MenuOverlay::key(const KeyEvent& ev, double nowSeconds)
    {
        if (levels_.empty())
            return false;
        const std::size_t at = levels_.size() - 1;
        Level& level = levels_[at];
        const auto move = [&](int row)
        {
            if (row < 0)
                return;
            setHighlight(at, row);
            const float before = level.scroll;
            scrollIntoView(level, row);
            if (before != level.scroll)
                touch();
        };
        const auto activate = [&]
        {
            const int row = level.highlighted;
            if (row < 0)
                return;
            const Row& r = level.rows[static_cast<std::size_t>(row)];
            if (r.submenu)
                openChild(at, row, true);
            else
                finish((*level.items)[static_cast<std::size_t>(r.item)].id);
        };
        switch (ev.key)
        {
            case Key::up:       move(step(level, level.highlighted, -1)); break;
            case Key::down:     move(step(level, level.highlighted, +1)); break;
            case Key::home:     move(edge(level, false)); break;
            case Key::end:      move(edge(level, true)); break;
            case Key::pageUp:
            case Key::pageDown:
            {
                // A page of rows towards the edge, stopping at the last row that can be highlighted on the way.
                const int rows = std::max(1, static_cast<int>((level.panel.h - 2.0f * kPadPx) / kRowPx) - 1);
                const int dir = ev.key == Key::pageDown ? 1 : -1;
                int row = level.highlighted;
                for (int k = 0; k < rows; ++k)
                {
                    const int next = step(level, row, dir);
                    if (next < 0 || (row >= 0 && (dir > 0 ? next <= row : next >= row)))
                        break;                       // it would wrap: the edge is reached
                    row = next;
                }
                move(row);
                break;
            }
            case Key::right:
                if (level.highlighted >= 0 && level.rows[static_cast<std::size_t>(level.highlighted)].submenu)
                    openChild(at, level.highlighted, true);
                break;
            case Key::left:
                if (at > 0)
                    closeFrom(at);
                break;
            case Key::enter: activate(); break;
            case Key::escape: finish(0); break;
            case Key::space:
            case Key::character:
            {
                if (ev.key == Key::space && (typed_.empty() || nowSeconds - typedAt_ > kTypeAheadSeconds))
                {
                    activate();                      // Space chooses, unless it continues a typed prefix
                    break;
                }
                if (ev.mods.cmd || ev.mods.ctrl || ev.ch < 0x20u)
                    break;                           // a chord is no text
                if (nowSeconds - typedAt_ > kTypeAheadSeconds)
                    typed_.clear();
                typedAt_ = nowSeconds;
                std::string one;
                appendUtf8(one, ev.ch);
                one = lowered(one);
                const std::string longer = typed_ + one;
                const int n = static_cast<int>(level.rows.size());
                const auto labelOf = [&](int row)
                {
                    return keyOf((*level.items)[static_cast<std::size_t>(level.rows[static_cast<std::size_t>(row)].item)].label);
                };
                const auto find = [&](const std::string& prefix, int from) -> int
                {
                    for (int k = 0; k < n; ++k)
                    {
                        const int row = ((from + k) % n + n) % n;
                        if (level.rows[static_cast<std::size_t>(row)].selectable && startsWith(labelOf(row), prefix))
                            return row;
                    }
                    return -1;
                };
                // The longer prefix refines the current match (searched from the current row); when no row starts
                // with it and it is one letter typed again, the next row that starts with that letter.
                int row = find(longer, longer.size() > one.size() ? std::max(0, level.highlighted)
                                                                  : level.highlighted + 1);
                if (row >= 0)
                    typed_ = longer;
                else if (!typed_.empty() && typed_.find_first_not_of(one[0]) == std::string::npos && one.size() == 1)
                    row = find(one, level.highlighted + 1);
                else
                {
                    row = find(one, level.highlighted + 1);  // a new prefix
                    if (row >= 0)
                        typed_ = one;
                }
                move(row);
                break;
            }
            case Key::tab:
            case Key::backspace:
            case Key::del:
                break;                               // the menu's while it shows, and nothing
        }
        return true;
    }

    //==================================================================================================================
    // Drawing
    //==================================================================================================================

    void MenuOverlay::draw(Canvas& c) const
    {
        if (levels_.empty())
            return;
        const Palette p = paletteFor(request_.theme);
        const float dpi = c.dpi() > 0.05f ? c.dpi() : 1.0f;
        const float hair = 1.0f / dpi;               // a hairline: one device px
        for (const Level& level : levels_)
        {
            const Rect& r = level.panel;
            // The shadow: a soft rounded rectangle a little lower than the panel, then the panel with its hairline.
            c.rrect(r.x - 1.0f, r.y + 2.0f, r.w + 2.0f, r.h + 2.0f, kRadiusPx + 1.0f, p.shadow, 0.0f, {}, kShadowPx);
            c.rrect(r.x, r.y, r.w, r.h, kRadiusPx, p.surface, hair, p.edge);

            const Rect view{ c.snapX(r.x + hair), c.snapY(r.y + kPadPx), r.w - 2.0f * hair, r.h - 2.0f * kPadPx };
            const Canvas::ClipScope clip(c, view);
            const float left = r.x + kInsetPx, width = r.w - 2.0f * kInsetPx;
            for (std::size_t i = 0; i < level.rows.size(); ++i)
            {
                const Row& row = level.rows[i];
                const float top = r.y + kPadPx + row.y - level.scroll;
                if (top + row.h <= view.y || top >= view.bottom())
                    continue;                        // scrolled out of the panel
                const MenuItem& item = (*level.items)[static_cast<std::size_t>(row.item)];
                const float cy = top + row.h * 0.5f;
                if (row.kind == RowKind::separator)
                {
                    c.hairlineH(left + 3.0f, cy, width - 6.0f, p.rule);
                    continue;
                }
                if (row.kind == RowKind::header)
                {
                    c.text(upper(item.label).c_str(), left + kLabelPx, c.capCentreTop(cy + 1.0f, kHeader), kHeader,
                           p.dim);
                    continue;
                }
                const bool lit = row.selectable && (static_cast<int>(i) == level.highlighted
                                                    || static_cast<int>(i) == level.open);
                if (lit)
                    c.rrect(left, top, width, row.h, kPillRadiusPx, p.hover);
                const Col ink = row.selectable ? p.text : p.disabled;
                if (item.checked && !row.submenu)
                {
                    const Col mark = row.selectable ? p.mark : p.disabled;
                    const float mx = left + kMarkPx;
                    if (item.radio)
                        c.disc(mx, cy, 2.5f, mark);
                    else
                    {
                        c.segment(mx - 4.0f, cy + 0.5f, mx - 1.2f, cy + 3.3f, 1.5f, mark);
                        c.segment(mx - 1.2f, cy + 3.3f, mx + 4.2f, cy - 3.3f, 1.5f, mark);
                    }
                }
                c.text(item.label.c_str(), left + kLabelPx, c.capCentreTop(cy, kLabel), kLabel, ink);
                const float right = left + width - kRowPadPx;
                if (row.submenu)
                {
                    const Col chev = row.selectable ? (lit ? p.text : p.dim) : p.disabled;
                    c.segment(right - kChevronPx + 1.0f, cy - 3.5f, right - 1.0f, cy, 1.3f, chev);
                    c.segment(right - 1.0f, cy, right - kChevronPx + 1.0f, cy + 3.5f, 1.3f, chev);
                }
                else if (!item.shortcut.empty())
                    c.text(item.shortcut.c_str(), right, c.capCentreTop(cy, kLabel), kLabel,
                           row.selectable ? p.dim : p.disabled, Align::right);
            }
        }
    }
}

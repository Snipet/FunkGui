#include <funkgui/widgets/RuleSlider.h>

#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Col.h>
#include <funkgui/core/Ease.h>
#include <funkgui/core/Theme.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/params/GestureController.h>
#include <funkgui/params/ParamPort.h>
#include <funkgui/text/FontService.h>
#include <funkgui/text/TextFit.h>
#include <funkgui/widgets/AttachedWord.h>
#include <funkgui/widgets/FocusRing.h>

#include <cmath>
#include <cstring>
#include <string>

// RuleSlider (02 §5.4, §8.1–§8.3, §8.9; F §3.3–§3.4): HR's slot (BgfxEditor.cpp:1562-1620, input :1745-1930) over a
// ValueModel, with detents, hybrid end cells and the five states. Every write goes through the GestureController:
// drags are one gesture from pointer down to up (dragTo), wheel notches one burst (wheelTo), and keys, clicks,
// double-clicks and a11y are begin/set/end triples that write nothing when the value would not change (tap, or the
// model's writeDetent for a detent). Locked, derived and n/a views refuse every write gesture; the popup click still
// opens the host menu. Choices where 02 is silent are listed in the header.

namespace funkgui
{
    namespace
    {
        constexpr float kSpan       = 240.0f;    // px per full track (HR); Shift 1200, Cmd 6000
        constexpr float kSpanFine   = 1200.0f;
        constexpr float kSpanUltra  = 6000.0f;
        constexpr float kWheelStep  = 0.025f;    // track per wheel unit (HR); Shift 0.005; non-smooth deltas x4
        constexpr float kWheelFine  = 0.005f;
        constexpr float kKeyStep    = 0.01f;     // track per arrow (02 §8.9); Shift 0.001; page 0.1
        constexpr float kKeyFine    = 0.001f;
        constexpr float kPageStep   = 0.1f;
        constexpr float kHysteresis = 6.0f;      // stepped commit at 0.5·p + 6 px of travel (02 §5.4)
        constexpr float kCellPitch  = 24.0f;     // hybrid end cells step like 24 px detents: 0.5·24 + 6 = 18 px
        constexpr float kSoftStick  = 4.0f;      // a drag sticks to a soft notch within ±4 px (02 §8.1)
        constexpr float kClickSlop  = 3.0f;      // a detent-label click becomes a drag past 3 px (02 §5.4)
        constexpr float kEdge       = 1.0e-4f;   // "at the range edge" for hybrid key/wheel/a11y crossing
        constexpr float kPairGap    = 2.5f;      // 02 §8.3 adjacent-pair margin
        constexpr float kEndSlack   = 2.0f;      // 02 §8.3 end labels within [x-2, x+w+2]
        constexpr float kTagGap     = 6.0f;      // a tag moved to the sub line, then the text
        constexpr float kUnitGap    = 6.0f;      // value, then unit (HR)
        constexpr float kLocalFlash = 0.16f;     // label flash ease-back tau (02 §8.7)

        constexpr const char* kEnDash = "\xE2\x80\x93";        // U+2013, the n/a value (01 §4.6, K1 #15)

        float clamp01(float v) noexcept { return v <= 0.0f ? 0.0f : (v >= 1.0f ? 1.0f : v); }

        float stepPitch(int n) noexcept
        {
            const float p = n > 1 ? 240.0f / static_cast<float>(n - 1) : 64.0f;
            return p < 24.0f ? 24.0f : (p > 64.0f ? 64.0f : p);
        }

        bool nonEmpty(const char* s) noexcept { return s != nullptr && s[0] != '\0'; }

        std::string lowerAscii(const char* s)
        {
            std::string out = s != nullptr ? s : "";
            for (char& ch : out)
                if (ch >= 'A' && ch <= 'Z')
                    ch = static_cast<char>(ch - 'A' + 'a');
            return out;
        }

        // Appends to a fixed buffer without splitting a UTF-8 codepoint; the result is always NUL-terminated, and the
        // first append that does not fit whole ends the line (nothing after a cut).
        struct Line
        {
            char*  out;
            size_t cap;
            size_t len = 0;
            bool   full = false;

            void add(const char* s) noexcept
            {
                if (full || out == nullptr || cap == 0 || s == nullptr)
                    return;
                size_t take = std::strlen(s);
                if (len + take + 1 > cap)
                {
                    full = true;
                    take = cap - 1 - len;
                    while (take > 0 && (static_cast<unsigned char>(s[take]) & 0xC0u) == 0x80u)
                        --take;                          // back off to a codepoint boundary
                }
                std::memcpy(out + len, s, take);
                len += take;
                out[len] = '\0';
            }
        };
    }

    // ---- construction and the view --------------------------------------------------------------------------------

    RuleSlider::RuleSlider(ValueModel& model, SlotGeom geom, uint32_t a11yId)
        : model_(model), geom_(geom), a11yId_(a11yId)
    {
        refresh();
        caretX_ = caretTarget();
    }

    void RuleSlider::setWord(AttachedWord* word)
    {
        word_ = word;
        refresh();
    }

    const ValueView& RuleSlider::view() const { return view_; }

    void RuleSlider::refresh()
    {
        const uint64_t k = model_.key();
        if (!viewValid_ || k != viewKey_)
        {
            ValueView v;
            model_.view(v);
            view_ = v;
            viewKey_ = k;
            viewValid_ = true;
            fitRule_ = false;
            firstLabelLeft_ = 0.0f;
            if (view_.state == ValueState::stepped && view_.detents != nullptr && view_.nDetents > 0)
            {
                const FontAtlasSdf& atlas = FontService::get().atlas();
                fitRule_ = detentLabelsFit(atlas, view_.detents, view_.nDetents, geom_.w);
                const float c = geom_.w / static_cast<float>(view_.nDetents);
                firstLabelLeft_ = 0.5f * c - 0.5f * text::width(atlas, view_.detents[0].label, type::kMicro);
            }
            tagWidth_ = nonEmpty(view_.tag) ? text::width(FontService::get().atlas(), view_.tag, type::kMicro) : 0.0f;
        }
        // Word visibility can change without this view changing: a tag moved to the detent line needs room there.
        labelsDrawn_ = fitRule_ && (!tagOnSubLine() || firstLabelLeft_ >= tagWidth_ + 4.0f);
        if (!labelsDrawn_)
            hoverLabel_ = -1;
    }

    bool RuleSlider::refused() const noexcept
    {
        return view_.state == ValueState::locked || view_.state == ValueState::derived
            || view_.state == ValueState::na;
    }

    bool RuleSlider::hybrid() const noexcept
    {
        return view_.state == ValueState::continuous
            && ((view_.nEndLo > 0 && view_.endLo != nullptr) || (view_.nEndHi > 0 && view_.endHi != nullptr));
    }

    bool RuleSlider::tagOnSubLine() const noexcept
    {
        return nonEmpty(view_.tag) && word_ != nullptr && word_->visible();
    }

    int RuleSlider::nLo() const noexcept { return view_.endLo != nullptr && view_.nEndLo > 0 ? view_.nEndLo : 0; }
    int RuleSlider::nHi() const noexcept { return view_.endHi != nullptr && view_.nEndHi > 0 ? view_.nEndHi : 0; }

    // ---- layout ------------------------------------------------------------------------------------------------------

    float RuleSlider::contLeft() const noexcept
    {
        const int lo = hybrid() ? nLo() : 0;
        return geom_.x + (lo > 0 ? kEndCell * static_cast<float>(lo) + kEndGap : 0.0f);
    }

    float RuleSlider::contRight() const noexcept
    {
        const int hi = hybrid() ? nHi() : 0;
        return geom_.x + geom_.w - (hi > 0 ? kEndCell * static_cast<float>(hi) + kEndGap : 0.0f);
    }

    float RuleSlider::cellCentre(int ord) const noexcept
    {
        const int lo = nLo();
        if (ord < lo)
            return geom_.x + kEndCell * static_cast<float>(ord) + 0.5f * kEndCell;
        const int k = ord - lo;                          // 1-based high cell
        return geom_.x + geom_.w - kEndCell * static_cast<float>(nHi() - k + 1) + 0.5f * kEndCell;
    }

    const Detent* RuleSlider::endCell(int ord) const noexcept
    {
        const int lo = nLo();
        if (ord >= 0 && ord < lo)
            return &view_.endLo[ord];
        const int k = ord - lo;
        if (k >= 1 && k <= nHi())
            return &view_.endHi[k - 1];
        return nullptr;
    }

    int RuleSlider::hybridOrdinal() const noexcept
    {
        const int lo = nLo(), hi = nHi();
        if (view_.activeEnd < 0)
        {
            const int k = -view_.activeEnd;
            return k <= lo ? k - 1 : lo;
        }
        if (view_.activeEnd > 0)
            return view_.activeEnd <= hi ? lo + view_.activeEnd : lo;
        return lo;
    }

    float RuleSlider::detentCentre(int i) const noexcept
    {
        const float c = geom_.w / static_cast<float>(view_.nDetents > 0 ? view_.nDetents : 1);
        return geom_.x + c * (static_cast<float>(i) + 0.5f);
    }

    int RuleSlider::activeDetent() const noexcept
    {
        const int n = view_.nDetents;
        if (n <= 0)
            return -1;
        if (view_.detent >= 0 && view_.detent < n)
            return view_.detent;
        const int i = static_cast<int>(std::floor(clamp01(view_.track) * static_cast<float>(n)));
        return i >= n ? n - 1 : i;
    }

    float RuleSlider::caretTarget() const noexcept
    {
        const float x = geom_.x, w = geom_.w;
        switch (view_.state)
        {
            case ValueState::stepped:
                return view_.nDetents > 0 ? detentCentre(activeDetent()) : x + clamp01(view_.track) * w;
            case ValueState::continuous:
                if (hybrid())
                {
                    const int ord = hybridOrdinal();
                    if (ord != nLo())
                        return cellCentre(ord);
                    return contLeft() + clamp01(view_.track) * (contRight() - contLeft());
                }
                return x + clamp01(view_.track) * w;
            case ValueState::locked:
            case ValueState::derived:
                return x + clamp01(view_.track) * w;
            case ValueState::na:
                return x;
        }
        return x;
    }

    Rect RuleSlider::labelRect(int i) const noexcept
    {
        const float c = geom_.w / static_cast<float>(view_.nDetents > 0 ? view_.nDetents : 1);
        return { geom_.x + c * static_cast<float>(i), geom_.subTop() - 3.0f, c, 15.0f };
    }

    // ---- the fit rule ------------------------------------------------------------------------------------------------

    bool RuleSlider::detentLabelsFit(const FontAtlasSdf& atlas, const Detent* d, int n, float w) noexcept
    {
        if (d == nullptr || n <= 0 || !(w > 0.0f))
            return false;
        const float c = w / static_cast<float>(n);
        float prev = text::width(atlas, d[0].label, type::kMicro);
        if (0.5f * c - 0.5f * prev < -kEndSlack)
            return false;                                // the first label starts left of x - 2
        for (int i = 1; i < n; ++i)
        {
            const float wi = text::width(atlas, d[i].label, type::kMicro);
            if (0.5f * (prev + wi) + kPairGap > c)
                return false;                            // an adjacent pair collides
            prev = wi;
        }
        return c * (static_cast<float>(n) - 0.5f) + 0.5f * prev <= w + kEndSlack;   // the last label ends by x+w+2
    }

    // ---- per frame ---------------------------------------------------------------------------------------------------

    void RuleSlider::tick(float dt, bool hovered, bool focused, bool alwaysChrome)
    {
        refresh();
        alwaysChrome_ = alwaysChrome;
        hoverOn_ = hovered || focused || drag_ != Drag::none;
        if (!hovered)
            hoverLabel_ = -1;
        hover_ = ease::hover(hover_, hoverOn_, dt);

        const float target = caretTarget();
        const bool detented = view_.state == ValueState::stepped || (hybrid() && hybridOrdinal() != nLo());
        if (landing_)
        {
            caretX_ = ease::toward(caretX_, target, dt, 0.09f);     // a Mode switch: the move is the information
            if (ease::sameBits(caretX_, target))
                landing_ = false;
        }
        else if (detented)
            caretX_ = ease::toward(caretX_, target, dt, 0.06f);
        else
            caretX_ = ease::shown(caretX_, target, dt, 0.09f, 0.15f * geom_.w);

        if (flashHold_ > 0.0f)
            flashHold_ = flashHold_ > dt ? flashHold_ - dt : 0.0f;
        else
            flash_ = ease::toward(flash_, 0.0f, dt, kLocalFlash);
    }

    bool RuleSlider::settled() const
    {
        if (!viewValid_ || model_.key() != viewKey_)
            return false;                                // a newer view is waiting for the next tick
        if (!ease::sameBits(hover_, hoverOn_ ? 1.0f : 0.0f))
            return false;
        if (!ease::sameBits(caretX_, caretTarget()))
            return false;
        return !(flashHold_ > 0.0f) && ease::sameBits(flash_, 0.0f);
    }

    void RuleSlider::flashLabel(float seconds)
    {
        refresh();                                       // the landing is measured against the new view
        landing_ = !ease::sameBits(caretX_, caretTarget());
        if (seconds > 0.0f)
        {
            flash_ = 1.0f;
            flashHold_ = seconds;
        }
    }

    // ---- hit testing -------------------------------------------------------------------------------------------------

    bool RuleSlider::contains(Point p) const
    {
        if (word_ != nullptr && word_->visible() && word_->contains(p))
            return false;                                // the word is carved out of the slot (checked first)
        return geom_.hit().contains(p);
    }

    int RuleSlider::detentLabelAt(Point p) const
    {
        if (view_.state != ValueState::stepped || !labelsDrawn_ || !contains(p))
            return -1;
        for (int i = 0; i < view_.nDetents; ++i)
            if (labelRect(i).contains(p))
                return i;
        return -1;
    }

    Cursor RuleSlider::cursorAt(Point p) const
    {
        if (!contains(p) || refused())
            return Cursor::normal;
        return detentLabelAt(p) >= 0 ? Cursor::pointingHand : Cursor::leftRight;
    }

    void RuleSlider::pointerMove(const PointerEvent& e)
    {
        const Point p{ e.x, e.y };
        hoverOn_ = contains(p) || drag_ != Drag::none;
        hoverLabel_ = detentLabelAt(p);
    }

    void RuleSlider::pointerExit()
    {
        hoverOn_ = drag_ != Drag::none;
        hoverLabel_ = -1;
    }

    // ---- writes ------------------------------------------------------------------------------------------------------

    void RuleSlider::write(float host01, GestureController& g, Write how, double nowSec)
    {
        ParamPort* p = model_.port();
        if (p == nullptr)
            return;
        switch (how)
        {
            case Write::tap:   g.tap(*p, host01); break;
            case Write::drag:  g.dragTo(host01); break;
            case Write::wheel: g.wheelTo(*p, host01, nowSec); break;
        }
    }

    void RuleSlider::writeDetentBy(int i, GestureController& g, Write how, double nowSec)
    {
        const int n = view_.nDetents;
        if (view_.detents == nullptr || i < 0 || i >= n)
            return;
        if (how == Write::tap)
        {
            if (i != view_.detent)
                model_.writeDetent(i, g);                // the product's detent write (default: tap)
            return;
        }
        write(view_.detents[i].host01, g, how, nowSec);
    }

    // A hybrid position: an end cell's detent, or the continuous range at track value t.
    void RuleSlider::writeHybrid(int ord, float t, GestureController& g, Write how, double nowSec)
    {
        if (const Detent* d = endCell(ord))
            write(d->host01, g, how, nowSec);
        else
            write(model_.host01FromTrack(clamp01(t)), g, how, nowSec);
    }

    // One discrete step (dir ±1) of `amount` track units on a hybrid slot: inside the range the value moves and
    // stops at the edge; from the edge, the next step enters the adjacent end cell; between cells it moves one cell;
    // out of the innermost cell it lands on the range's edge (02 §8.1: keys, wheel and a11y cross one cell at a time).
    void RuleSlider::stepHybrid(int dir, float amount, GestureController& g, Write how, double nowSec)
    {
        const int lo = nLo(), last = lo + nHi();
        const int ord = hybridOrdinal();
        if (ord == lo)
        {
            const float t = clamp01(view_.track);
            if (dir < 0 && t <= kEdge && lo > 0)
                writeHybrid(lo - 1, 0.0f, g, how, nowSec);
            else if (dir > 0 && t >= 1.0f - kEdge && last > lo)
                writeHybrid(lo + 1, 0.0f, g, how, nowSec);
            else
                writeHybrid(lo, t + static_cast<float>(dir) * amount, g, how, nowSec);
            return;
        }
        const int to = ord + dir;
        if (to < 0 || to > last)
            return;                                      // already the outermost cell
        writeHybrid(to, to == lo ? (dir > 0 ? 0.0f : 1.0f) : 0.0f, g, how, nowSec);
    }

    // ---- pointer -----------------------------------------------------------------------------------------------------

    void RuleSlider::beginStepped(Point at, GestureController& g, ParamPort& port)
    {
        g.beginDrag(port);
        drag_ = Drag::stepped;
        down_ = at;
        anchorS_ = 0.0f;
        dragDetent_ = activeDetent();
        dragTravel_ = 0.0f;
    }

    float RuleSlider::travel(Point p) const noexcept
    {
        return (p.x - down_.x) + (down_.y - p.y);        // right or up increases (HR)
    }

    void RuleSlider::pointerDown(const PointerEvent& e, GestureController& g, HostServices& host)
    {
        refresh();
        if (drag_ != Drag::none)
            finishDrag(g);                               // a down without the previous up: close what was open
        if (e.popup)
        {
            if (ParamPort* p = model_.port())
                host.showParamMenu(*p, e.x, e.y);        // the host menu, never a write (HR :1692-1700)
            return;
        }
        if (refused())
            return;                                      // no gesture, no write; the footer shows the reason
        ParamPort* port = model_.port();
        if (port == nullptr)
            return;                                      // display-only
        const Point p{ e.x, e.y };
        down_ = p;
        if (view_.state == ValueState::stepped)
        {
            const int label = e.clicks < 2 ? detentLabelAt(p) : -1;
            if (label >= 0)
            {
                drag_ = Drag::label;                     // armed; committed on up unless it becomes a drag
                armedLabel_ = label;
                return;
            }
            beginStepped(p, g, *port);
            return;
        }
        g.beginDrag(*port);
        drag_ = hybrid() ? Drag::hybrid : Drag::continuous;
        fine_ = e.mods.shift;
        ultra_ = e.mods.cmd;
        dragT_ = clamp01(view_.track);
        dragDetent_ = hybrid() ? hybridOrdinal() : 0;
        const float span = ultra_ ? kSpanUltra : (fine_ ? kSpanFine : kSpan);
        edgeS_ = -dragT_ * span;                         // travel at which the track is 0
        if (drag_ == Drag::hybrid && dragDetent_ != nLo())
            edgeS_ = -cellTravel(dragDetent_, 0.0f, span);   // the pointer starts on its cell's nominal position
    }

    // The nominal travel of hybrid ordinal `ord` relative to the range's low edge: cells sit one 24 px pitch apart
    // beyond each edge of the range, which spans `span` px of travel.
    float RuleSlider::cellTravel(int ord, float edge, float span) const noexcept
    {
        const int lo = nLo();
        if (ord < lo)
            return edge - kCellPitch * static_cast<float>(lo - ord);
        return edge + span + kCellPitch * static_cast<float>(ord - lo);
    }

    void RuleSlider::pointerDrag(const PointerEvent& e, GestureController& g)
    {
        if (drag_ == Drag::none)
            return;
        refresh();
        if (refused())
        {
            finishDrag(g);                               // the view became locked, derived or n/a mid-drag
            return;
        }
        const Point p{ e.x, e.y };
        if (drag_ == Drag::label)
        {
            const float dx = p.x - down_.x, dy = p.y - down_.y;
            if (dx * dx + dy * dy <= kClickSlop * kClickSlop)
                return;                                  // still a click
            ParamPort* port = model_.port();
            if (port == nullptr)
            {
                drag_ = Drag::none;
                return;
            }
            armedLabel_ = -1;
            beginStepped(down_, g, *port);               // a relative drag from the current detent
        }
        if (g.dragParam() != model_.port())
        {
            drag_ = Drag::none;                          // the drag was closed elsewhere (closeAll)
            return;
        }
        const float s = travel(p);

        if (drag_ == Drag::stepped)
        {
            const int n = view_.nDetents;
            if (n <= 0 || view_.detents == nullptr)
                return;
            const float pitch = stepPitch(n), commit = 0.5f * pitch + kHysteresis;
            float rel = s - anchorS_;
            int i = dragDetent_;
            while (rel >= commit && i < n - 1)
            {
                ++i;
                anchorS_ += pitch;                       // the anchor is the committed detent's own position
                rel -= pitch;
            }
            while (rel <= -commit && i > 0)
            {
                --i;
                anchorS_ -= pitch;
                rel += pitch;
            }
            if ((i == n - 1 && rel > 0.0f) || (i == 0 && rel < 0.0f))
            {
                anchorS_ += rel;                         // an end stop: travel past the end is not banked
                rel = 0.0f;
            }
            dragTravel_ = rel;
            if (i != dragDetent_)
            {
                dragDetent_ = i;
                writeDetentBy(i, g, Write::drag, 0.0);
            }
            return;
        }

        // Continuous and hybrid: HR's rule in track space, re-anchored when a modifier toggles (HR :1746-1772).
        const float oldSpan = ultra_ ? kSpanUltra : (fine_ ? kSpanFine : kSpan);
        if (e.mods.shift != fine_ || e.mods.cmd != ultra_)
        {
            fine_ = e.mods.shift;
            ultra_ = e.mods.cmd;
            const float span = ultra_ ? kSpanUltra : (fine_ ? kSpanFine : kSpan);
            if (dragDetent_ == nLo() || drag_ == Drag::continuous)
                edgeS_ = s - dragT_ * span;              // the value under the pointer stays put
            else
                edgeS_ = cellTravel(dragDetent_, edgeS_, oldSpan) - cellTravel(dragDetent_, 0.0f, span);
        }
        const float span = ultra_ ? kSpanUltra : (fine_ ? kSpanFine : kSpan);

        if (drag_ == Drag::continuous)
        {
            float t = clamp01((s - edgeS_) / span);
            const float trackW = contRight() - contLeft();
            float best = kSoftStick;
            for (int k = 0; view_.notches != nullptr && k < view_.nNotches; ++k)
            {
                const float d = std::fabs(t - view_.notches[k]) * trackW;
                if (d <= best)
                {
                    best = d;
                    t = clamp01(view_.notches[k]);       // sticks within ±4 px (02 §8.1)
                }
            }
            dragT_ = t;
            write(model_.host01FromTrack(t), g, Write::drag, 0.0);
            return;
        }

        // Hybrid: continuous inside the range; 18 px of travel past an edge enters the adjacent end cell, and from a
        // cell the next one is 18 px of travel from its own position (24 px pitch, 6 px hysteresis each way).
        const int lo = nLo(), last = lo + nHi();
        const float commit = 0.5f * kCellPitch + kHysteresis;
        int ord = dragDetent_;
        float t = dragT_;
        for (int guard = 0; guard <= last + 1; ++guard)
        {
            if (ord == lo)
            {
                const float u = (s - edgeS_) / span;
                if (lo > 0 && edgeS_ - s >= commit)
                {
                    ord = lo - 1;
                    continue;
                }
                if (last > lo && s - (edgeS_ + span) >= commit)
                {
                    ord = lo + 1;
                    continue;
                }
                t = clamp01(u);
                break;
            }
            // In a cell. Moving on by a commit lands at least 6 px short of the next position's own commit back, so
            // the loop never oscillates; back in the range, the range branch resolves the value (clamped to its edge).
            const float rel = s - cellTravel(ord, edgeS_, span);
            if (rel >= commit && ord < last)
            {
                ++ord;
                continue;
            }
            if (rel <= -commit && ord > 0)
            {
                --ord;
                continue;
            }
            break;
        }
        const bool moved = ord != dragDetent_ || (ord == lo && !ease::sameBits(t, dragT_));
        dragDetent_ = ord;
        if (ord == lo)
            dragT_ = t;
        if (moved)
            writeHybrid(ord, dragT_, g, Write::drag, 0.0);
    }

    void RuleSlider::pointerUp(const PointerEvent&, GestureController& g)
    {
        if (drag_ == Drag::label)
        {
            refresh();
            const int i = armedLabel_;
            drag_ = Drag::none;
            armedLabel_ = -1;
            if (!refused() && i >= 0)
                writeDetentBy(i, g, Write::tap, 0.0);    // nothing when it is the current detent
            return;
        }
        finishDrag(g);
    }

    void RuleSlider::finishDrag(GestureController& g)
    {
        if (drag_ != Drag::none && drag_ != Drag::label && g.dragging() && g.dragParam() == model_.port())
            g.endDrag();
        drag_ = Drag::none;
        armedLabel_ = -1;
        dragTravel_ = 0.0f;
    }

    void RuleSlider::doubleClick(GestureController& g)
    {
        refresh();
        if (drag_ == Drag::label)
        {
            drag_ = Drag::none;                          // a double-click is not a label jump
            armedLabel_ = -1;
        }
        if (refused())
            return;
        write(model_.defaultHost01(), g, Write::tap, 0.0);   // the Mode default (02 §8.4.6); inside an open drag
    }

    // ---- wheel -------------------------------------------------------------------------------------------------------

    // Whole notches in a wheel event: one per discrete event, or what smooth deltas have accumulated in units of
    // kWheelNotch (the remainder is kept for the burst's next event). Non-finite deltas count nothing, and one burst
    // banks at most 64 notches.
    int RuleSlider::notches(float v, bool smooth) noexcept
    {
        if (!std::isfinite(v))
            return 0;
        if (!smooth)
            return v > 0.0f ? 1 : (v < 0.0f ? -1 : 0);
        constexpr float cap = 64.0f * kWheelNotch;
        wheelAcc_ += v;
        wheelAcc_ = wheelAcc_ < -cap ? -cap : (wheelAcc_ > cap ? cap : wheelAcc_);
        const int k = static_cast<int>(wheelAcc_ / kWheelNotch);    // truncates toward zero
        wheelAcc_ -= static_cast<float>(k) * kWheelNotch;
        return k;
    }

    bool RuleSlider::wheel(const WheelEvent& e, GestureController& g, double nowSec)
    {
        refresh();
        if (refused() || model_.port() == nullptr)
            return false;                                // nothing to write: the host may scroll
        if (wheelLast_ < 0.0 || nowSec - wheelLast_ >= GestureController::kWheelIdle)
            wheelAcc_ = 0.0f;                            // a new burst starts from nothing
        wheelLast_ = nowSec;

        // macOS turns shift+scroll into a horizontal event: take whichever axis carries the motion (HR :1830).
        const float delta = e.dy != 0.0f ? e.dy : e.dx;
        const float v = (e.reversed ? -1.0f : 1.0f) * delta;

        if (view_.state == ValueState::stepped)
        {
            const int k = notches(v, e.smooth);
            const int n = view_.nDetents;
            if (k != 0 && n > 0)
            {
                const int from = activeDetent();
                int to = from + k;
                to = to < 0 ? 0 : (to > n - 1 ? n - 1 : to);
                if (to != from)
                    writeDetentBy(to, g, Write::wheel, nowSec);
            }
            return true;
        }

        const float dv = v * (e.smooth ? 1.0f : 4.0f) * (e.mods.shift ? kWheelFine : kWheelStep);
        if (hybrid())
        {
            const int lo = nLo();
            const int ord = hybridOrdinal();
            const float t = clamp01(view_.track);
            const bool atLowEdge = ord == lo && t <= kEdge && dv < 0.0f && lo > 0;
            const bool atHighEdge = ord == lo && t >= 1.0f - kEdge && dv > 0.0f && nHi() > 0;
            if (ord != lo || atLowEdge || atHighEdge)
            {
                const int k = notches(v, e.smooth);      // cells and edge crossings step one notch at a time
                for (int i = 0; i < (k < 0 ? -k : k); ++i)
                {
                    stepHybrid(k < 0 ? -1 : 1, 0.0f, g, Write::wheel, nowSec);
                    refresh();
                }
                return true;
            }
            if (std::fabs(dv) >= 1.0e-7f)
                writeHybrid(lo, t + dv, g, Write::wheel, nowSec);
            return true;
        }
        if (std::fabs(dv) >= 1.0e-7f)
            write(model_.host01FromTrack(clamp01(clamp01(view_.track) + dv)), g, Write::wheel, nowSec);
        return true;
    }

    // ---- keys and a11y -----------------------------------------------------------------------------------------------

    bool RuleSlider::key(const KeyEvent& e, GestureController& g)
    {
        int dir = 0;
        bool page = false, home = false, end = false, reset = false;
        switch (e.key)
        {
            case Key::up: case Key::right:   dir = 1; break;
            case Key::down: case Key::left:  dir = -1; break;
            case Key::pageUp:                dir = 1; page = true; break;
            case Key::pageDown:              dir = -1; page = true; break;
            case Key::home:                  home = true; break;
            case Key::end:                   end = true; break;
            case Key::del: case Key::backspace: reset = true; break;
            case Key::character: case Key::tab: case Key::escape: case Key::enter: case Key::space:
                return false;
        }
        refresh();
        if (refused() || model_.port() == nullptr)
            return true;                                 // no write; the footer shows the reason
        if (reset)
        {
            write(model_.defaultHost01(), g, Write::tap, 0.0);
            return true;
        }
        if (view_.state == ValueState::stepped)
        {
            const int n = view_.nDetents, from = activeDetent();
            if (n <= 0)
                return true;
            int to = home ? 0 : (end ? n - 1 : from + dir);
            to = to < 0 ? 0 : (to > n - 1 ? n - 1 : to);
            writeDetentBy(to, g, Write::tap, 0.0);
            return true;
        }
        const float step = page ? kPageStep : (e.mods.shift ? kKeyFine : kKeyStep);
        if (hybrid())
        {
            const int lo = nLo(), last = lo + nHi();
            if (home)
                writeHybrid(0, 0.0f, g, Write::tap, 0.0);          // the outermost position (a cell or t = 0)
            else if (end)
                writeHybrid(last, 1.0f, g, Write::tap, 0.0);
            else
                stepHybrid(dir, step, g, Write::tap, 0.0);
            return true;
        }
        const float t = home ? 0.0f : (end ? 1.0f : clamp01(view_.track) + static_cast<float>(dir) * step);
        write(model_.host01FromTrack(clamp01(t)), g, Write::tap, 0.0);
        return true;
    }

    void RuleSlider::a11yAction(A11yAction a, double value, GestureController& g)
    {
        refresh();
        if (a == A11yAction::showMenu)
        {
            if (ParamPort* p = model_.port())
            {
                const Rect r = geom_.hit();
                g.host().showParamMenu(*p, r.centreX(), r.centreY());
            }
            return;
        }
        if (refused() || model_.port() == nullptr)
            return;                                      // never writes, from any input path (02 §8.4.5)
        const bool inc = a == A11yAction::increment, dec = a == A11yAction::decrement;
        if (a != A11yAction::setValue && !inc && !dec)
            return;                                      // press, toggle, focus: nothing to write on a slider
        if (a == A11yAction::setValue && std::isnan(value))
            return;

        if (view_.state == ValueState::stepped)
        {
            const int n = view_.nDetents;
            if (n <= 0)
                return;
            int to = activeDetent() + (inc ? 1 : (dec ? -1 : 0));
            if (a == A11yAction::setValue)
            {
                const double top = static_cast<double>(n - 1);
                to = static_cast<int>(std::lround(value < 0.0 ? 0.0 : (value > top ? top : value)));
            }
            to = to < 0 ? 0 : (to > n - 1 ? n - 1 : to);
            writeDetentBy(to, g, Write::tap, 0.0);       // index space (F §8.2)
            return;
        }
        if (a == A11yAction::setValue)
        {
            const float t = clamp01(static_cast<float>(value));
            if (hybrid())
                writeHybrid(nLo(), t, g, Write::tap, 0.0);
            else
                write(model_.host01FromTrack(t), g, Write::tap, 0.0);
            return;
        }
        if (hybrid())
            stepHybrid(inc ? 1 : -1, kKeyStep, g, Write::tap, 0.0);
        else
            write(model_.host01FromTrack(clamp01(clamp01(view_.track) + (inc ? kKeyStep : -kKeyStep))), g,
                  Write::tap, 0.0);
    }

    void RuleSlider::accessibility(A11yItem& item) const
    {
        const ValueView& v = view_;
        item = A11yItem{};
        item.id = a11yId_;
        item.bounds = geom_.hit();
        item.title = v.label != nullptr ? v.label : "";
        if (nonEmpty(v.aka))
            item.description = "controls " + lowerAscii(v.aka);
        std::string spoken = v.text.spoken;
        if (spoken.empty())
        {
            spoken = v.text.value;
            if (v.text.unit[0] != '\0')
                spoken += " " + lowerAscii(v.text.unit);
        }
        const std::string reason = nonEmpty(v.reason) ? v.reason : "";

        switch (v.state)
        {
            case ValueState::continuous:
            {
                item.role = A11yRole::slider;
                item.lo = 0.0;
                item.hi = 1.0;
                item.step = kKeyStep;
                item.value = spoken;
                item.help = reason;
                const int ord = hybrid() ? hybridOrdinal() : 0;
                item.v = hybrid() && ord != nLo() ? (ord < nLo() ? 0.0 : 1.0) : static_cast<double>(clamp01(v.track));
                if (hybrid())
                {
                    std::string steps;
                    for (int k = 0; k < nLo() + nHi() + 1; ++k)
                        if (const Detent* d = endCell(k))
                            steps += std::string(steps.empty() ? "" : ", ") + (nonEmpty(d->spoken) ? d->spoken : d->label);
                    item.help = (reason.empty() ? "" : reason + "; ") + "end steps: " + steps;
                }
                break;
            }
            case ValueState::stepped:
            {
                item.role = A11yRole::slider;
                const int n = v.nDetents > 0 ? v.nDetents : 1;
                item.lo = 0.0;
                item.hi = static_cast<double>(n - 1);
                item.step = 1.0;
                item.v = static_cast<double>(activeDetent() < 0 ? 0 : activeDetent());
                if (v.text.spoken[0] == '\0' && v.detents != nullptr && activeDetent() >= 0
                    && nonEmpty(v.detents[activeDetent()].spoken))
                    spoken = v.detents[activeDetent()].spoken;
                item.value = spoken;
                std::string help = std::to_string(v.nDetents) + " steps: ";
                for (int i = 0; v.detents != nullptr && i < v.nDetents; ++i)
                    help += std::string(i > 0 ? ", " : "") + (v.detents[i].label != nullptr ? v.detents[i].label : "");
                item.help = reason.empty() ? help : help + "; " + reason;
                break;
            }
            case ValueState::locked:
                item.role = A11yRole::slider;
                item.readOnly = true;
                item.enabled = false;
                item.v = static_cast<double>(clamp01(v.track));
                item.value = spoken.empty() ? "fixed" : spoken + ", fixed";
                item.help = reason;
                break;
            case ValueState::derived:
                item.role = A11yRole::slider;
                item.readOnly = true;
                item.v = static_cast<double>(clamp01(v.track));
                item.value = spoken;
                item.help = reason;
                break;
            case ValueState::na:
                item.role = A11yRole::staticText;
                item.readOnly = true;
                item.enabled = false;
                item.title += ", not applicable";
                item.help = reason;
                break;
        }
    }

    // ---- the footer --------------------------------------------------------------------------------------------------

    void RuleSlider::specLine(char* out, size_t n) const
    {
        if (out == nullptr || n == 0)
            return;
        out[0] = '\0';
        const ValueView& v = view_;
        Line line{ out, n };
        line.add(v.label);
        if (nonEmpty(v.aka))
        {
            line.add(" (");
            line.add(v.aka);
            line.add(")");
        }
        const bool extension = v.tag != nullptr && std::strcmp(v.tag, "+") == 0;
        if (nonEmpty(v.reason))
        {
            line.add("   ");
            line.add(v.reason);
        }
        if (refused())
            return;                                      // the reason is the whole story
        if (extension)
            line.add("   EXTENSION \xE2\x80\x94 NOT ON THE ORIGINAL UNIT \xE2\x80\x94 NEUTRAL AT DEFAULT");
        if (v.clamped && v.text.sub[0] != '\0')
        {
            line.add("   ");
            line.add(v.text.sub);
        }
        if (v.state == ValueState::stepped)
        {
            line.add("   STEPS ");
            for (int i = 0; v.detents != nullptr && i < v.nDetents; ++i)
            {
                if (i > 0)
                    line.add(" \xC2\xB7 ");
                line.add(v.detents[i].label);
            }
            line.add("   DRAG / WHEEL / ARROWS STEP");
            if (labelsDrawn_)
                line.add("   CLICK A STEP");
            line.add("   DBL-CLICK RESET");
            return;
        }
        if (hybrid())
        {
            line.add("   END STEPS ");
            bool first = true;
            for (int k = 0; k < nLo() + nHi() + 1; ++k)
                if (const Detent* d = endCell(k))
                {
                    if (!first)
                        line.add(" \xC2\xB7 ");
                    line.add(d->label);
                    first = false;
                }
        }
        line.add("   DRAG   SHIFT FINE   DBL-CLICK RESET");
    }

    // ---- drawing -----------------------------------------------------------------------------------------------------

    void RuleSlider::draw(Canvas& c, const Theme& th, bool focusRing) const
    {
        const ValueView& v = view_;
        const float x = geom_.x, w = geom_.w, top = geom_.top, ty = geom_.trackY();
        const bool primary = geom_.isPrimary();
        const TextStyle& vs = primary ? type::kValueP : type::kValueS;
        const TextStyle& us = primary ? type::kLabel : type::kMicro;
        const float h = hover_;
        const bool writableState = v.state == ValueState::continuous || v.state == ValueState::stepped;

        // Inks by state (02 §8.1): label, value, unit.
        Col labelCol = th.ink52, valueCol = th.ink100, unitCol = th.ink52;
        switch (v.state)
        {
            case ValueState::continuous:
            case ValueState::stepped:
                labelCol = mix(th.ink52, th.ink70, h);
                valueCol = mix(v.atDefault ? th.ink32 : th.ink100, th.accent, h);   // accent under the hand (HR)
                break;
            case ValueState::locked:
                labelCol = mix(th.ink32, th.ink52, h);
                valueCol = th.ink32;
                unitCol = th.ink32;
                break;
            case ValueState::derived:
                valueCol = th.ink52;
                break;
            case ValueState::na:
                labelCol = mix(th.ink16, th.ink32, h);
                valueCol = th.ink16;
                break;
        }
        labelCol = mix(labelCol, th.ink100, flash_);    // the Mode-switch flash (02 §8.7)

        // Label row: the label, and the tag right-aligned at x+w unless a word holds that place.
        const bool hasTag = nonEmpty(v.tag) && v.state != ValueState::na;
        const bool tagOnLine = hasTag && tagOnSubLine();
        {
            const Canvas::Scope s(c, tags::slotLabel, false);
            c.text(v.label, x, top, type::kLabel, labelCol);
            if (hasTag && !tagOnLine)
                c.text(v.tag, x + w, c.sharedBaselineTop(top, type::kLabel, type::kMicro), type::kMicro, th.ink32,
                       Align::right);
        }

        // Value and unit on a shared baseline (HR).
        {
            const bool liveValue = v.state == ValueState::derived && v.live;
            const Canvas::Scope s(c, tags::slotValue, liveValue);
            const float vy = geom_.valueTop();
            if (v.state == ValueState::na)
                c.text(kEnDash, x, vy, vs, valueCol);
            else
            {
                c.text(v.text.value, x, vy, vs, valueCol);
                if (v.text.unit[0] != '\0')
                    c.text(v.text.unit, x + c.textWidth(v.text.value, vs) + kUnitGap,
                           c.sharedBaselineTop(vy, vs, us), us, unitCol);
            }
        }

        // Sub / detent line.
        const float sy = geom_.subTop();
        float subX = x;
        if (tagOnLine)
        {
            const Canvas::Scope s(c, tags::slotSub, false);
            c.text(v.tag, x, sy, type::kMicro, th.ink32);
            subX = x + tagWidth_ + kTagGap;
        }
        const bool isHybrid = hybrid();
        const int ord = isHybrid ? hybridOrdinal() : 0;
        if (v.state == ValueState::stepped && labelsDrawn_)
        {
            const Canvas::Scope s(c, tags::detentLabel, false);
            const int active = activeDetent();
            for (int i = 0; i < v.nDetents; ++i)
            {
                const Col col = i == active ? th.ink100 : (i == hoverLabel_ ? th.ink70 : th.ink32);
                c.text(v.detents[i].label, detentCentre(i), sy, type::kMicro, col, Align::centre);
            }
        }
        else if (v.state != ValueState::na)
        {
            if (isHybrid)
            {
                const Canvas::Scope s(c, tags::detentLabel, false);
                for (int k = 0; k < nLo() + nHi() + 1; ++k)
                    if (const Detent* d = endCell(k))
                        c.text(d->label, cellCentre(k), sy, type::kMicro, k == ord ? th.ink100 : th.ink32,
                               Align::centre);
                if (nLo() > 0 && subX < contLeft() + kEndGap)
                    subX = contLeft() + kEndGap;     // clear of the low cells' labels, which reach x + 16·n
            }
            if (v.text.sub[0] != '\0')
            {
                const Canvas::Scope s(c, tags::slotSub, v.live);
                c.text(v.text.sub, subX, sy, type::kMicro, th.ink32);
            }
        }

        // Track.
        switch (v.state)
        {
            case ValueState::na:
                break;
            case ValueState::locked:
            {
                {
                    const Canvas::Scope s(c, tags::slotTrack, false);
                    c.dotted(x, ty, w, 3.0f, th.ink16);
                }
                if (!v.clamped && v.track >= 0.0f && v.track <= 1.0f)
                {
                    const Canvas::Scope s(c, tags::slotNotch, false);
                    c.hairlineV(x + v.track * w, ty + 1.0f, 5.0f, th.ink32);
                }
                break;
            }
            case ValueState::derived:
            {
                {
                    const Canvas::Scope s(c, tags::slotTrack, false);
                    c.hairlineH(x, ty, w, th.ink16);
                }
                const Canvas::Scope s(c, tags::slotCaret, v.live);
                c.rrect(caretX_ - 1.5f, ty + 3.5f - 9.0f, 3.0f, 9.0f, 0.0f, th.ink52.withAlpha(0.0f), 1.0f, th.ink52);
                break;
            }
            case ValueState::continuous:
            case ValueState::stepped:
            {
                const float x0 = contLeft(), x1 = contRight();
                const float notchX = x0 + clamp01(v.trackDefault) * (x1 - x0);
                {
                    const Canvas::Scope s(c, tags::slotTrack, false);
                    // Hover fill: 0 -> caret (bipolar: from the default notch), under the hairline (HR).
                    if (h > 0.01f && writableState && !(isHybrid && ord != nLo()))
                    {
                        float a = v.state == ValueState::stepped ? x : x0, b = caretX_;
                        if (v.bipolar && v.state == ValueState::continuous)
                        {
                            a = notchX < caretX_ ? notchX : caretX_;
                            b = notchX < caretX_ ? caretX_ : notchX;
                        }
                        if (b > a)
                            c.rrect(a, ty - 1.0f, b - a, 3.0f, 0.0f, th.accentDim.withAlpha(h));
                    }
                    c.hairlineH(x0, ty, x1 - x0, alwaysChrome_ ? th.ink32 : th.ink16);
                }
                if (v.state == ValueState::stepped)
                {
                    const float cell = w / static_cast<float>(v.nDetents > 0 ? v.nDetents : 1);
                    if (cell >= 4.0f)                    // ticks are omitted below 4 px cells (02 §8.3)
                    {
                        const Canvas::Scope s(c, tags::detentTick, false);
                        const int active = activeDetent();
                        for (int i = 0; i < v.nDetents; ++i)
                            c.hairlineV(detentCentre(i), ty + 1.0f, 5.0f, i == active ? th.ink100 : th.ink32);
                    }
                }
                else
                {
                    if (isHybrid)
                    {
                        const Canvas::Scope s(c, tags::detentTick, false);
                        for (int k = 0; k < nLo() + nHi() + 1; ++k)
                            if (endCell(k) != nullptr)
                                c.hairlineV(cellCentre(k), ty + 1.0f, 5.0f, k == ord ? th.ink100 : th.ink32);
                    }
                    const Canvas::Scope s(c, tags::slotNotch, false);
                    c.hairlineV(notchX, ty + 1.0f, 3.0f, th.ink32);        // the Mode-default notch
                    for (int k = 0; v.notches != nullptr && k < v.nNotches; ++k)
                        c.hairlineV(x0 + clamp01(v.notches[k]) * (x1 - x0), ty + 1.0f, 3.0f, th.ink32);
                }

                const float caretH = (alwaysChrome_ ? 9.0f : 7.0f) + 4.0f * h;
                const Canvas::Scope s(c, tags::slotCaret, false);
                if (drag_ == Drag::stepped && v.state == ValueState::stepped && v.nDetents > 0)
                {
                    // The ghost caret follows the raw travel: one detent pitch of pointer travel is one cell.
                    const float cell = w / static_cast<float>(v.nDetents);
                    float gx = detentCentre(dragDetent_) + dragTravel_ * cell / stepPitch(v.nDetents);
                    gx = gx < x ? x : (gx > x + w ? x + w : gx);
                    c.hairlineV(gx, ty + 3.5f - caretH, caretH, th.ink32);
                }
                c.rrect(caretX_ - 1.0f, ty + 3.5f - caretH, 2.0f, caretH, 0.0f, valueCol);
                break;
            }
        }

        if (focusRing)
            drawFocusRing(c, geom_.hit(), th.accent);
    }
}

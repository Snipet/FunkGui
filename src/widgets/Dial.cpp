#include <funkgui/widgets/Dial.h>

#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Col.h>
#include <funkgui/core/Ease.h>
#include <funkgui/core/Theme.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/panel/HostServices.h>
#include <funkgui/params/GestureController.h>
#include <funkgui/params/ParamPort.h>
#include <funkgui/widgets/FocusRing.h>

#include <cmath>
#include <cstring>
#include <string>

// Dial (FSynth 02 §4.1, §14.1; FC 02 §5.3–§5.4, §8.1, §8.9): RuleSlider's contract on a 270-degree sweep. The input
// rules are RuleSlider's (src/widgets/RuleSlider.cpp) with the travel of a dial: one gesture from pointer down to up
// (dragTo), wheel notches one burst (wheelTo), and keys, double-clicks and a11y begin/set/end triples that write
// nothing when the value would not change; locked, derived and n/a views refuse every write, and the popup click still
// opens the host menu. There are no detent labels to click. Choices where 02 is silent are listed in the header.
//
// Every point on a circle is centre + r * (cos a, sin a) with the cosine and the sine taken in double and rounded to
// float once (FSynth 02 §1.1, "One set of goldens"); arcs go through Canvas::arc, whose points are core/Flatten.h's.

namespace funkgui
{
    namespace
    {
        constexpr float kPi        = 3.14159265358979323846f;
        constexpr float kStartA    = 0.75f * kPi;        // 135 degrees: 7:30
        constexpr float kSweepA    = 1.5f * kPi;         // 270 degrees, clockwise through 12:00
        constexpr float kCellA     = Dial::kEndCellDeg * kPi / 180.0f;
        constexpr float kWheelStep = 0.025f;             // track per wheel unit (RuleSlider); Shift 0.005; non-smooth x4
        constexpr float kWheelFine = 0.005f;
        constexpr float kKeyStep   = 0.01f;              // track per arrow (FC 02 §8.9); Shift 0.001; page 0.1
        constexpr float kKeyFine   = 0.001f;
        constexpr float kPageStep  = 0.1f;
        constexpr float kWheelNotch = 0.10f;             // smooth wheel delta per detent (RuleSlider::kWheelNotch)
        constexpr float kHysteresis = 6.0f;              // stepped commit at 0.5·p + 6 px of travel
        constexpr float kCellPitch = 24.0f;              // hybrid end cells step like 24 px detents: 0.5·24 + 6 = 18 px
        constexpr float kSoftStick = 4.0f;               // a drag sticks to a soft notch within 4 px of 240 px of travel
        constexpr float kEdge      = 1.0e-4f;            // "at the range edge" for hybrid key/wheel/a11y crossing
        constexpr float kLocalFlash = 0.16f;             // label flash ease-back tau (FC 02 §8.7)

        constexpr float kTrackW    = 2.0f;               // track, value arc and band width
        constexpr float kFillW     = 4.0f;               // the hover fill under them
        constexpr float kMarkOut   = 3.0f;               // radial marks centre at radius() + 3
        constexpr float kPointerIn = 6.0f, kPointerOut = 2.0f;   // the pointer spans radius()-6 .. radius()+2
        constexpr float kHollowR   = 3.0f;               // a derived dial's hollow pointer
        constexpr float kDotR      = 2.5f;               // the live dot
        constexpr float kDotStep   = 3.0f, kDotW = 1.5f; // a locked dial's dotted track
        constexpr float kMinCellArc = 4.0f;              // detent ticks need a cell of at least 4 px of track arc
        constexpr float kUnitGap   = 4.0f;               // value, then unit
        constexpr float kLinkedR   = 4.0f;               // the LINKED box (FC ADR-74): radius, fill strength
        constexpr float kLinkedFill = 0.35f;
        constexpr float kLinkedGrow = 2.0f;              // the box: the hit rect grown 2 px up and down
        constexpr float kRingIn    = 2.0f, kRingOut = 4.0f;   // ringContains: radius()+2 .. ringRadius()+4

        constexpr const char* kEnDash = "\xE2\x80\x93";        // U+2013, the n/a value

        float clamp01(float v) noexcept { return v <= 0.0f ? 0.0f : (v >= 1.0f ? 1.0f : v); }
        float unit01(float v) noexcept { return !(v > 0.0f) ? 0.0f : (v >= 1.0f ? 1.0f : v); }   // NaN -> 0

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

        // The overlay as the widget keeps it: every position in 0..1 and every pair in order.
        ModOverlay sanitised(const ModOverlay& m) noexcept
        {
            ModOverlay s = m;
            s.lo = unit01(m.lo);
            s.hi = unit01(m.hi);
            if (s.hi < s.lo)
            {
                const float t = s.lo;
                s.lo = s.hi;
                s.hi = t;
            }
            s.editLo = unit01(m.editLo);
            s.editHi = unit01(m.editHi);
            if (s.editHi < s.editLo)
            {
                const float t = s.editLo;
                s.editLo = s.editHi;
                s.editHi = t;
            }
            s.live = unit01(m.live);
            return s;
        }

        // The modulation part of an a11y description: "modulated by 2 routes" (", no effect" when inert).
        std::string modDescription(const ModOverlay& m)
        {
            std::string s = "modulated by " + std::to_string(m.routes) + (m.routes == 1 ? " route" : " routes");
            if (m.inert)
                s += ", no effect";
            return s;
        }

        struct Dir
        {
            float c, s;
        };

        Dir dirOf(float a) noexcept
        {
            return { static_cast<float>(std::cos(static_cast<double>(a))),
                     static_cast<float>(std::sin(static_cast<double>(a))) };
        }

        Point onCircle(Point c, float r, Dir d) noexcept { return { c.x + r * d.c, c.y + r * d.s }; }

        // A radial capsule of width w whose drawn extent runs from r0 to r1 (the round ends included).
        void radial(Canvas& cv, Point c, float a, float r0, float r1, float w, Col col)
        {
            const Dir d = dirOf(a);
            const float h = 0.5f * w;
            const Point p0 = onCircle(c, r0 + h, d), p1 = onCircle(c, r1 - h, d);
            cv.segment(p0.x, p0.y, p1.x, p1.y, w, col);
        }

        // A radial mark of length len centred at radius rc, 1 px wide.
        void mark(Canvas& cv, Point c, float a, float rc, float len, Col col)
        {
            radial(cv, c, a, rc - 0.5f * len, rc + 0.5f * len, 1.0f, col);
        }

        // The LINKED box and the focus ring: the hit rect grown 2 px up and down, so the label at the top and the sub
        // line, which runs to top + 78, sit inside it ({x + 2, top - 2, w - 4, h}).
        Rect boxOf(const DialGeom& g) noexcept
        {
            const Rect h = g.hit();
            return { h.x, h.y - kLinkedGrow, h.w, h.h + 2.0f * kLinkedGrow };
        }

        // An arc from a to b in increasing angle; nothing when the two are the same.
        void arcBetween(Canvas& cv, Point c, float r, float a, float b, float w, Col col)
        {
            if (ease::sameBits(a, b))
                return;
            cv.arc(c, r, a < b ? a : b, a < b ? b : a, w, col);
        }
    }

    // ---- construction, the view and the overlay ---------------------------------------------------------------------

    Dial::Dial(ValueModel& model, DialGeom geom, uint32_t a11yId) : model_(model), geom_(geom), a11yId_(a11yId)
    {
        refresh();
        pointerA_ = pointerTarget();
    }

    void Dial::setMod(const ModOverlay* m)
    {
        mod_ = m;
        modValid_ = false;
        refresh();
    }

    const ValueView& Dial::view() const { return view_; }

    void Dial::refresh()
    {
        const uint64_t k = model_.key();
        if (!viewValid_ || k != viewKey_)
        {
            ValueView v;
            model_.view(v);
            view_ = v;
            viewKey_ = k;
            viewValid_ = true;
        }
        if (mod_ == nullptr)
        {
            if (modView_.routes != 0 || modValid_)
                modView_ = ModOverlay{};
            modValid_ = false;
        }
        else if (!modValid_ || mod_->key != modView_.key)
        {
            modView_ = sanitised(*mod_);
            modValid_ = true;
        }
    }

    bool Dial::refused() const noexcept
    {
        return view_.state == ValueState::locked || view_.state == ValueState::derived || view_.state == ValueState::na;
    }

    bool Dial::hybrid() const noexcept
    {
        return view_.state == ValueState::continuous
            && ((view_.nEndLo > 0 && view_.endLo != nullptr) || (view_.nEndHi > 0 && view_.endHi != nullptr));
    }

    bool Dial::writable() const noexcept
    {
        return !refused() && model_.port() != nullptr;
    }

    bool Dial::hasRoutes() const noexcept { return modView_.routes > 0; }

    int Dial::nLo() const noexcept { return view_.endLo != nullptr && view_.nEndLo > 0 ? view_.nEndLo : 0; }
    int Dial::nHi() const noexcept { return view_.endHi != nullptr && view_.nEndHi > 0 ? view_.nEndHi : 0; }

    // ---- layout -----------------------------------------------------------------------------------------------------

    float Dial::angleOf(float t) const noexcept { return kStartA + t * kSweepA; }

    // Hybrid ordinal ord: low cells 0..nLo-1 left (outermost) to right, then the range, then the high cells inner to
    // outer. Low cell k from the sweep (1 = next to it) is ordinal nLo - k; high cell k is ordinal nLo + k.
    float Dial::cellAngle(int ord) const noexcept
    {
        const int lo = nLo();
        if (ord < lo)
            return kStartA - kCellA * (static_cast<float>(lo - ord) - 0.5f);
        return kStartA + kSweepA + kCellA * (static_cast<float>(ord - lo) - 0.5f);
    }

    const Detent* Dial::endCell(int ord) const noexcept
    {
        const int lo = nLo();
        if (ord >= 0 && ord < lo)
            return &view_.endLo[ord];
        const int k = ord - lo;
        if (k >= 1 && k <= nHi())
            return &view_.endHi[k - 1];
        return nullptr;
    }

    int Dial::hybridOrdinal() const noexcept
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

    int Dial::activeDetent() const noexcept
    {
        const int n = view_.nDetents;
        if (n <= 0)
            return -1;
        if (view_.detent >= 0 && view_.detent < n)
            return view_.detent;
        const int i = static_cast<int>(std::floor(clamp01(view_.track) * static_cast<float>(n)));
        return i >= n ? n - 1 : i;
    }

    float Dial::detentTrack(int i) const noexcept
    {
        const int n = view_.nDetents > 0 ? view_.nDetents : 1;
        return (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
    }

    float Dial::pointerTarget() const noexcept
    {
        switch (view_.state)
        {
            case ValueState::stepped:
                return angleOf(view_.nDetents > 0 ? detentTrack(activeDetent()) : clamp01(view_.track));
            case ValueState::continuous:
                if (hybrid())
                {
                    const int ord = hybridOrdinal();
                    if (ord != nLo())
                        return cellAngle(ord);
                }
                return angleOf(clamp01(view_.track));
            case ValueState::locked:
            case ValueState::derived:
                return angleOf(clamp01(view_.track));
            case ValueState::na:
                return kStartA;
        }
        return kStartA;
    }

    Rect Dial::valueBox() const
    {
        const float y = (geom_.isPrimary() ? geom_.valueTop() : geom_.labelTop()) - 2.0f;
        return { geom_.x + 2.0f, y, geom_.w() - 4.0f, 14.0f };
    }

    bool Dial::takesTypedKeys() const
    {
        return writable();
    }

    // ---- per frame --------------------------------------------------------------------------------------------------

    void Dial::tick(float dt, bool hovered, bool focused, bool alwaysChrome)
    {
        refresh();
        alwaysChrome_ = alwaysChrome;
        hoverOn_ = hovered || focused || drag_ != Drag::none;
        hover_ = ease::hover(hover_, hoverOn_, dt);

        const float target = pointerTarget();
        const bool detented = view_.state == ValueState::stepped || (hybrid() && hybridOrdinal() != nLo());
        if (landing_)
        {
            pointerA_ = ease::toward(pointerA_, target, dt, 0.09f);    // a Mode switch: the move is the information
            if (ease::sameBits(pointerA_, target))
                landing_ = false;
        }
        else if (detented)
            pointerA_ = ease::toward(pointerA_, target, dt, 0.06f);
        else
            pointerA_ = ease::shown(pointerA_, target, dt, 0.09f, 0.15f * kSweepA);

        if (flashHold_ > 0.0f)
            flashHold_ = flashHold_ > dt ? flashHold_ - dt : 0.0f;
        else
            flash_ = ease::toward(flash_, 0.0f, dt, kLocalFlash);
    }

    bool Dial::settled() const
    {
        if (!viewValid_ || model_.key() != viewKey_)
            return false;                                // a newer view is waiting for the next tick
        if (mod_ != nullptr && (!modValid_ || mod_->key != modView_.key))
            return false;                                // so is a newer overlay
        if (!ease::sameBits(hover_, hoverOn_ ? 1.0f : 0.0f))
            return false;
        if (!ease::sameBits(pointerA_, pointerTarget()))
            return false;
        return !(flashHold_ > 0.0f) && ease::sameBits(flash_, 0.0f);
    }

    void Dial::flashLabel(float seconds)
    {
        refresh();                                       // the landing is measured against the new view
        landing_ = !ease::sameBits(pointerA_, pointerTarget());
        if (seconds > 0.0f)
        {
            flash_ = 1.0f;
            flashHold_ = seconds;
        }
    }

    // ---- hit testing ------------------------------------------------------------------------------------------------

    bool Dial::contains(Point p) const { return geom_.hit().contains(p); }

    bool Dial::ringContains(Point p) const
    {
        const Point c = geom_.centre();
        const float dx = p.x - c.x, dy = p.y - c.y;
        const float d2 = dx * dx + dy * dy;
        const float r0 = geom_.radius() + kRingIn, r1 = geom_.ringRadius() + kRingOut;
        return d2 >= r0 * r0 && d2 < r1 * r1;
    }

    Cursor Dial::cursorAt(Point p) const
    {
        if (hasRoutes() && !refused() && ringContains(p))
            return Cursor::grab;                         // a depth drag (the product's, FSynth 02 §14.4)
        if (!contains(p) || !writable())
            return Cursor::normal;
        return Cursor::upDown;
    }

    // ---- writes -----------------------------------------------------------------------------------------------------

    void Dial::write(float host01, GestureController& g, Write how, double nowSec)
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

    void Dial::writeDetentBy(int i, GestureController& g, Write how, double nowSec)
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

    void Dial::writeHybrid(int ord, float t, GestureController& g, Write how, double nowSec)
    {
        if (const Detent* d = endCell(ord))
            write(d->host01, g, how, nowSec);
        else
            write(model_.host01FromTrack(clamp01(t)), g, how, nowSec);
    }

    // One discrete step (dir ±1) of `amount` track units on a hybrid dial, crossing the range's edge one cell at a time
    // (RuleSlider::stepHybrid).
    void Dial::stepHybrid(int dir, float amount, GestureController& g, Write how, double nowSec)
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

    // ---- pointer ----------------------------------------------------------------------------------------------------

    float Dial::travel(Point p) const noexcept
    {
        return (p.x - down_.x) + (down_.y - p.y);        // dx - dy: right or up increases
    }

    // The nominal travel of hybrid ordinal `ord` relative to the range's low edge: cells sit one 24 px pitch apart
    // beyond each edge of the range, which spans `span` px of travel.
    float Dial::cellTravel(int ord, float edge, float span) const noexcept
    {
        const int lo = nLo();
        if (ord < lo)
            return edge - kCellPitch * static_cast<float>(lo - ord);
        return edge + span + kCellPitch * static_cast<float>(ord - lo);
    }

    void Dial::pointerDown(const PointerEvent& e, GestureController& g, HostServices& host)
    {
        refresh();
        if (drag_ != Drag::none)
            finishDrag(g);                               // a down without the previous up: close what was open
        if (e.popup)
        {
            if (ParamPort* p = model_.port())
                host.showParamMenu(*p, e.x, e.y);        // the host menu, never a write
            return;
        }
        if (refused())
            return;                                      // no gesture, no write; the footer shows the reason
        ParamPort* port = model_.port();
        if (port == nullptr)
            return;                                      // display-only
        down_ = { e.x, e.y };
        g.beginDrag(*port);
        if (view_.state == ValueState::stepped)
        {
            drag_ = Drag::stepped;
            anchorS_ = 0.0f;
            dragDetent_ = activeDetent();
            dragTravel_ = 0.0f;
            return;
        }
        drag_ = hybrid() ? Drag::hybrid : Drag::continuous;
        fine_ = e.mods.shift;
        ultra_ = e.mods.cmd;
        dragT_ = clamp01(view_.track);
        dragDetent_ = hybrid() ? hybridOrdinal() : 0;
        const float span = ultra_ ? kUltraPx : (fine_ ? kFinePx : kDragPx);
        edgeS_ = -dragT_ * span;                         // travel at which the track is 0
        if (drag_ == Drag::hybrid && dragDetent_ != nLo())
            edgeS_ = -cellTravel(dragDetent_, 0.0f, span);   // the pointer starts on its cell's nominal position
    }

    void Dial::pointerDrag(const PointerEvent& e, GestureController& g)
    {
        if (drag_ == Drag::none)
            return;
        refresh();
        if (refused())
        {
            finishDrag(g);                               // the view became locked, derived or n/a mid-drag
            return;
        }
        if (g.dragParam() != model_.port())
        {
            drag_ = Drag::none;                          // the drag was closed elsewhere (closeAll)
            return;
        }
        const float s = travel({ e.x, e.y });

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

        // Continuous and hybrid: track space, re-anchored when a modifier toggles.
        const float oldSpan = ultra_ ? kUltraPx : (fine_ ? kFinePx : kDragPx);
        if (e.mods.shift != fine_ || e.mods.cmd != ultra_)
        {
            fine_ = e.mods.shift;
            ultra_ = e.mods.cmd;
            const float span = ultra_ ? kUltraPx : (fine_ ? kFinePx : kDragPx);
            if (dragDetent_ == nLo() || drag_ == Drag::continuous)
                edgeS_ = s - dragT_ * span;              // the value under the pointer stays put
            else
                edgeS_ = cellTravel(dragDetent_, edgeS_, oldSpan) - cellTravel(dragDetent_, 0.0f, span);
        }
        const float span = ultra_ ? kUltraPx : (fine_ ? kFinePx : kDragPx);

        if (drag_ == Drag::continuous)
        {
            float t = clamp01((s - edgeS_) / span);
            float best = kSoftStick;
            for (int k = 0; view_.notches != nullptr && k < view_.nNotches; ++k)
            {
                const float d = std::fabs(t - view_.notches[k]) * kDragPx;
                if (d <= best)
                {
                    best = d;
                    t = clamp01(view_.notches[k]);       // sticks within 4 px of a full sweep's 240
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

    void Dial::pointerUp(const PointerEvent&, GestureController& g)
    {
        finishDrag(g);
    }

    void Dial::finishDrag(GestureController& g)
    {
        if (drag_ != Drag::none && g.dragging() && g.dragParam() == model_.port())
            g.endDrag();
        drag_ = Drag::none;
        dragTravel_ = 0.0f;
    }

    void Dial::doubleClick(GestureController& g)
    {
        refresh();
        if (refused())
            return;
        write(model_.defaultHost01(), g, Write::tap, 0.0);   // the Mode default; inside an open drag
    }

    // ---- wheel ------------------------------------------------------------------------------------------------------

    int Dial::notches(float v, bool smooth) noexcept
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

    bool Dial::wheel(const WheelEvent& e, GestureController& g, double nowSec)
    {
        refresh();
        if (refused() || model_.port() == nullptr)
            return false;                                // nothing to write: the host may scroll
        if (wheelLast_ < 0.0 || nowSec - wheelLast_ >= GestureController::kWheelIdle)
            wheelAcc_ = 0.0f;                            // a new burst starts from nothing
        wheelLast_ = nowSec;

        const float delta = e.dy != 0.0f ? e.dy : e.dx;  // shift+scroll arrives horizontal on macOS
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

    // ---- keys and a11y ----------------------------------------------------------------------------------------------

    bool Dial::key(const KeyEvent& e, GestureController& g)
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

    void Dial::a11yAction(A11yAction a, double value, GestureController& g)
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
            return;                                      // never writes, from any input path
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
            writeDetentBy(to, g, Write::tap, 0.0);       // index space
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

    void Dial::accessibility(A11yItem& item) const
    {
        const ValueView& v = view_;
        item = A11yItem{};
        item.id = a11yId_;
        item.bounds = geom_.hit();
        item.title = v.label != nullptr ? v.label : "";
        if (nonEmpty(v.aka))
            item.description = "controls " + lowerAscii(v.aka);
        if (hasRoutes())
            item.description += (item.description.empty() ? "" : ", ") + modDescription(modView_);
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

    // ---- the footer -------------------------------------------------------------------------------------------------

    void Dial::specLine(char* out, size_t n) const
    {
        if (out == nullptr || n == 0)
            return;
        out[0] = '\0';
        size_t len = 0;
        bool full = false;
        // Appends without splitting a UTF-8 codepoint; the first append that does not fit whole ends the line.
        const auto add = [&](const char* s) {
            if (full || s == nullptr)
                return;
            size_t take = std::strlen(s);
            if (len + take + 1 > n)
            {
                full = true;
                take = n - 1 - len;
                while (take > 0 && (static_cast<unsigned char>(s[take]) & 0xC0u) == 0x80u)
                    --take;
            }
            std::memcpy(out + len, s, take);
            len += take;
            out[len] = '\0';
        };
        const ValueView& v = view_;
        add(v.label);
        if (nonEmpty(v.aka))
        {
            add(" (");
            add(v.aka);
            add(")");
        }
        const bool extension = v.tag != nullptr && std::strcmp(v.tag, "+") == 0;
        if (nonEmpty(v.reason))
        {
            add("   ");
            add(v.reason);
        }
        if (refused())
            return;                                      // the reason is the whole story
        if (extension)
            add("   EXTENSION \xE2\x80\x94 NOT ON THE ORIGINAL UNIT \xE2\x80\x94 NEUTRAL AT DEFAULT");
        if (v.clamped && v.text.sub[0] != '\0')
        {
            add("   ");
            add(v.text.sub);
        }
        if (v.state == ValueState::stepped)
        {
            add("   STEPS ");
            for (int i = 0; v.detents != nullptr && i < v.nDetents; ++i)
            {
                if (i > 0)
                    add(" \xC2\xB7 ");
                add(v.detents[i].label);
            }
            add("   DRAG / WHEEL / ARROWS STEP   DBL-CLICK RESET");
            return;
        }
        if (hybrid())
        {
            add("   END STEPS ");
            bool first = true;
            for (int k = 0; k < nLo() + nHi() + 1; ++k)
                if (const Detent* d = endCell(k))
                {
                    if (!first)
                        add(" \xC2\xB7 ");
                    add(d->label);
                    first = false;
                }
        }
        add("   DRAG   SHIFT FINE   DBL-CLICK RESET");
    }

    // ---- drawing ----------------------------------------------------------------------------------------------------

    void Dial::draw(Canvas& c, const Theme& th, bool focusRing) const
    {
        const ValueView& v = view_;
        const Point ctr = geom_.centre();
        const float r = geom_.radius(), rMark = r + kMarkOut;
        const float cx = ctr.x;
        const bool primary = geom_.isPrimary();
        const float h = hover_;
        const bool isHybrid = hybrid();
        const int ord = isHybrid ? hybridOrdinal() : 0;
        const bool inCell = isHybrid && ord != nLo();

        // A derived dial sits in a LINKED box (FC ADR-74), under everything else.
        if (v.state == ValueState::derived)
        {
            const Canvas::Scope s(c, tags::dialTrack, false);
            const Rect b = boxOf(geom_);
            c.rrect(b.x, b.y, b.w, b.h, kLinkedR, premix(th.ground, th.ink16, kLinkedFill), 1.0f, th.ink32);
        }

        // Inks by state (FSynth 02 §14.1): label, value, unit.
        Col labelCol = th.ink52, valueCol = th.ink100, unitCol = th.ink52;
        switch (v.state)
        {
            case ValueState::continuous:
            case ValueState::stepped:
                labelCol = mix(th.ink52, th.ink70, h);
                valueCol = mix(v.atDefault ? th.ink32 : th.ink100, th.accent, h);   // accent under the hand
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
        labelCol = mix(labelCol, th.ink100, flash_);    // the Mode-switch flash

        // Text, centred on the cell: the label, then (primary only) the value line and the sub line.
        {
            const Canvas::Scope s(c, tags::slotLabel, false);
            c.text(v.label, cx, geom_.labelTop(), primary ? type::kCaption : type::kMicro, labelCol, Align::centre);
        }
        if (primary)
        {
            {
                const Canvas::Scope s(c, tags::slotValue, v.state == ValueState::derived && v.live);
                const float vy = geom_.valueTop();
                if (v.state == ValueState::na)
                    c.text(kEnDash, cx, vy, type::kMicro, valueCol, Align::centre);
                else if (v.text.unit[0] == '\0')
                    c.text(v.text.value, cx, vy, type::kMicro, valueCol, Align::centre);
                else
                {
                    const float wv = c.textWidth(v.text.value, type::kMicro);
                    const float wu = c.textWidth(v.text.unit, type::kMicro);
                    const float x0 = cx - 0.5f * (wv + kUnitGap + wu);
                    c.text(v.text.value, x0, vy, type::kMicro, valueCol);
                    c.text(v.text.unit, x0 + wv + kUnitGap, vy, type::kMicro, unitCol);
                }
            }
            const char* sub = nullptr;
            if (v.state == ValueState::continuous && v.clamped)
                sub = "CLAMPED";
            else if (v.state != ValueState::na && nonEmpty(v.tag))
                sub = v.tag;
            if (sub != nullptr)
            {
                const Canvas::Scope s(c, tags::slotSub, false);
                c.text(sub, cx, geom_.subTop(), type::kMicro, th.ink32, Align::centre);
            }
        }

        // The track, its marks and the pointer.
        const float a0 = kStartA, a1 = kStartA + kSweepA;
        const Col trackCol = alwaysChrome_ ? th.ink32 : th.ink16;
        switch (v.state)
        {
            case ValueState::na:
                break;
            case ValueState::locked:
            {
                {
                    const Canvas::Scope s(c, tags::dialTrack, false);
                    c.arcDotted(ctr, r, a0, a1, kDotStep, kDotW, th.ink16);
                }
                if (!v.clamped && v.track >= 0.0f && v.track <= 1.0f)
                {
                    const Canvas::Scope s(c, tags::dialNotch, false);
                    mark(c, ctr, angleOf(v.track), rMark, 5.0f, th.ink32);
                }
                break;
            }
            case ValueState::derived:
            {
                {
                    const Canvas::Scope s(c, tags::dialTrack, false);
                    c.arc(ctr, r, a0, a1, kTrackW, th.ink16);
                }
                const Canvas::Scope s(c, tags::dialPointer, v.live);
                const Point p = onCircle(ctr, r, dirOf(pointerA_));
                c.disc(p.x, p.y, kHollowR, th.ink52.withAlpha(0.0f), 1.0f, th.ink52);
                break;
            }
            case ValueState::continuous:
            case ValueState::stepped:
            {
                const bool bipolar = v.bipolar && v.state == ValueState::continuous;
                const float from = bipolar ? angleOf(0.5f) : a0;
                {
                    const Canvas::Scope s(c, tags::dialTrack, false);
                    if (h > 0.01f && !inCell)            // the hover fill, under the track (RuleSlider's track fill)
                        arcBetween(c, ctr, r, from, pointerA_, kFillW, premix(th.ground, th.accentDim, h));
                    c.arc(ctr, r, a0, a1, kTrackW, trackCol);
                }
                if (v.state == ValueState::continuous && !inCell)
                {
                    const Canvas::Scope s(c, tags::dialArc, false);
                    arcBetween(c, ctr, r, from, pointerA_, kTrackW, mix(th.ink70, th.accent, h));
                }
                if (v.state == ValueState::stepped)
                {
                    const int n = v.nDetents > 0 ? v.nDetents : 1;
                    if (r * kSweepA / static_cast<float>(n) >= kMinCellArc && v.nDetents > 0)
                    {
                        const Canvas::Scope s(c, tags::detentTick, false);
                        const int active = activeDetent();
                        for (int i = 0; i < v.nDetents; ++i)
                            mark(c, ctr, angleOf(detentTrack(i)), rMark, 5.0f, i == active ? th.ink100 : th.ink32);
                    }
                }
                else
                {
                    if (isHybrid)
                    {
                        const Canvas::Scope s(c, tags::detentTick, false);
                        for (int k = 0; k < nLo() + nHi() + 1; ++k)
                            if (endCell(k) != nullptr)
                                mark(c, ctr, cellAngle(k), rMark, 5.0f, k == ord ? th.ink100 : th.ink32);
                    }
                    const Canvas::Scope s(c, tags::dialNotch, false);
                    mark(c, ctr, angleOf(clamp01(v.trackDefault)), rMark, 3.0f, th.ink32);   // the Mode default
                    for (int k = 0; v.notches != nullptr && k < v.nNotches; ++k)
                        mark(c, ctr, angleOf(clamp01(v.notches[k])), rMark, 3.0f, th.ink32);
                }

                const Canvas::Scope s(c, tags::dialPointer, false);
                if (drag_ == Drag::stepped && v.state == ValueState::stepped && v.nDetents > 0)
                {
                    // The ghost pointer follows the raw travel: one detent pitch of pointer travel is one cell.
                    const float n = static_cast<float>(v.nDetents);
                    const float gt = clamp01(detentTrack(dragDetent_) + dragTravel_ / stepPitch(v.nDetents) / n);
                    radial(c, ctr, angleOf(gt), r - kPointerIn, r + kPointerOut, 1.0f, th.ink32);
                }
                radial(c, ctr, pointerA_, r - kPointerIn, r + kPointerOut, 2.0f, th.ink100);
                break;
            }
        }

        // The modulation ring (ModOverlay): nothing at all without routes.
        if (hasRoutes())
        {
            const ModOverlay& m = modView_;
            const float rr = geom_.ringRadius();
            {
                const Canvas::Scope s(c, tags::modBand, false);
                c.arc(ctr, rr, angleOf(m.lo), angleOf(m.hi), kTrackW, m.inert ? th.ink16 : th.signal);
                if (m.editing)
                    c.arc(ctr, rr, angleOf(m.editLo), angleOf(m.editHi), kTrackW, th.accent);
            }
            if (m.liveValid && !m.inert)
            {
                const Canvas::Scope s(c, tags::modDot, true);
                const Point p = onCircle(ctr, rr, dirOf(angleOf(m.live)));
                c.disc(p.x, p.y, kDotR, th.signal);
            }
        }

        if (focusRing)
            drawFocusRing(c, boxOf(geom_), th.accent);
    }
}

#include <funkgui/widgets/HintLine.h>

#include <funkgui/canvas/Canvas.h>
#include <funkgui/canvas/Tags.h>
#include <funkgui/core/Col.h>
#include <funkgui/core/Theme.h>
#include <funkgui/core/TypeScale.h>
#include <funkgui/text/FontService.h>
#include <funkgui/text/TextFit.h>

#include <cstddef>

// HintLine (02 §5.8, §6.6): HR's first-run hint and footer spec line (BgfxEditor.cpp:1053-1054 countdown, :1637-1650
// draw, :1743-1747 the move that cuts it, :1567 always-chrome). Choices where 02 is silent are listed in the header.

namespace funkgui
{
    namespace
    {
        constexpr size_t kLine = 512;                    // the fitted line's buffer (a footer line is far shorter)
    }

    HintLine::HintLine(const char* firstRun, float seconds)
        : firstRun_(firstRun != nullptr ? firstRun : ""), left_(seconds > 0.0f ? seconds : 0.0f)
    {
    }

    void HintLine::tick(float dt)
    {
        if (dt > 0.0f && left_ > 0.0f)
            left_ = left_ > dt ? left_ - dt : 0.0f;
    }

    void HintLine::pointerMoved()
    {
        noteMove();
        if (left_ > kCut)
            left_ = kCut;                                // the hint has done its job once the pointer shows up (HR)
    }

    void HintLine::noteDown()
    {
        downSeen_ = true;
    }

    void HintLine::noteMove()
    {
        moved_ = true;
    }

    void HintLine::skip()
    {
        left_ = 0.0f;
    }

    bool HintLine::active() const
    {
        return left_ > 0.0f;
    }

    bool HintLine::wantsFullRate() const
    {
        return active();
    }

    bool HintLine::alwaysChrome() const
    {
        return downSeen_ && !moved_;                     // some host overlay eats move events (HR :1564-1567)
    }

    float HintLine::alpha() const noexcept
    {
        const float a = left_ / kCut;
        return a >= 1.0f ? 1.0f : (a > 0.0f ? a : 0.0f);
    }

    void HintLine::draw(Canvas& c, const Theme& th, float x, float y, float maxW, const char* spec) const
    {
        const char* line = active() ? firstRun_ : spec;
        if (line == nullptr || line[0] == '\0')
            return;
        char fitted[kLine];
        if (text::fitEllipsis(FontService::get().atlas(), line, type::kLabel, maxW, fitted, sizeof fitted) < 0)
            return;
        const Canvas::Scope s(c, tags::hint, false);
        c.text(fitted, x, y, type::kLabel, active() ? fade(th.ink32, alpha()) : th.ink32);
    }
}

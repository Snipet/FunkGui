#include <funkgui/live/LiveFeed.h>

// LiveState, the staleness rule under LiveFeed<Frame> (02 §5.10; HR BgfxEditor.cpp:966-980, BgfxEditor.h:176). The
// rules are listed in the header.

namespace funkgui
{
    LiveState::LiveState(float staleAfter) noexcept : staleAfter_(staleAfter) {}

    void LiveState::observe(bool readOk, uint32_t publishCount, float dt) noexcept
    {
        if (readOk && publishCount != last_)
        {
            last_ = publishCount;
            age_ = 0.0f;
            seen_ = true;
            fresh_ = true;
            return;
        }
        fresh_ = false;
        if (dt > 0.0f)
            age_ += dt;                                  // a failed read ages the stream like a repeated count (HR)
    }

    bool LiveState::live() const noexcept
    {
        return seen_ && age_ < staleAfter_;
    }

    bool LiveState::fresh() const noexcept
    {
        return fresh_;
    }

    float LiveState::staleSeconds() const noexcept
    {
        return age_;
    }
}

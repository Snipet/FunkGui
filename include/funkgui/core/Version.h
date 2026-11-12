#pragma once

// F1: FunkGui's version, for a product's about or debug page. Core: no JUCE, no bgfx, every platform.

namespace funkgui
{
    // project(FunkGui VERSION ...) of the FunkGui compiled into this binary, as "MAJOR.MINOR.PATCH" ("0.26.1"). The
    // pointer is to a string literal: valid for the life of the process.
    const char* version() noexcept;
}

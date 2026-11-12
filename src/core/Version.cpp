#include <funkgui/core/Version.h>

// FunkGui::core defines FUNKGUI_VERSION_STRING from project(FunkGui VERSION ...) (cmake/FunkGuiTargets.cmake).
#ifndef FUNKGUI_VERSION_STRING
  #error "FUNKGUI_VERSION_STRING is not defined: link FunkGui::core"
#endif

namespace funkgui
{
    const char* version() noexcept
    {
        return FUNKGUI_VERSION_STRING;
    }
}

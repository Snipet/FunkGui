#pragma once

// Environment access for FunkGui and its consumers (02 §1.8). Library code reads the environment only through
// funkgui::env(); there is no getenv in drawing code (C §3.3.3). Values are read once, into configuration objects such
// as CaptureConfig at EditorHost construction, never per frame.

#include <string_view>

namespace funkgui
{
    // The value of the environment variable FUNKGUI_ENV_PREFIX + name (env("PREFS_DIR") reads FCMP_PREFS_DIR in
    // FCompressor), or nullptr when it is unset. The first call for a name reads the environment and caches the
    // result; later calls return the cached value, even if the environment has changed since. The pointer stays valid
    // for the life of the process. Thread-safe.
    const char* env(std::string_view name);

    // Forget every cached value, so the next env() call for each name reads the environment again. For tests and
    // tools that change a variable after it was first read (funkgui::test::setEnv, then envReload()); never called by
    // drawing code. Pointers returned before the call stay valid and keep their old values.
    void envReload();
}

#pragma once

// UiPreferences in a small text file (v0.28.0): the store a desktop host without JUCE gives the preferences, where a
// JUCE build has its properties file (src/juce/PrefsFileBackend.cpp) and a browser its localStorage (web/WebPrefs.h).
// Without it such a build keeps the theme and the zoom in memory only, for the life of the process. Plain C++: no
// SDL and no JUCE (DesktopHost names the file, with SDL's preferences folder; a test names its own).
//
// The file is UTF-8 text, one preference a line, "<key>=<value>", in the keys' byte order, under a first line that
// names the format:
//
//   # FunkGui preferences 1
//   theme=1
//   uiZoom=125
//
// - A line that is empty or starts with '#' is skipped; a line without '=' is skipped; a key is what stands before
//   the first '='. A key's later line replaces its earlier one.
// - In a value, a backslash escapes: "\\" is a backslash, "\n" a line feed, "\r" a carriage return; a backslash
//   before anything else stands for itself. (No value FunkGui writes holds any: they are decimal integers.)
// - A key that is empty or holds '=', a line break or a leading '#' cannot be stored: write() ignores it.
//
// write() makes the change durable before it returns: the file is written whole beside its place
// ("<file>.partial") and renamed onto it, so a reader, or a crash, never sees half a file; the folder is made when it
// is missing. A file that cannot be read is an empty store, and one that cannot be written keeps the values in
// memory for this process (the next write tries again). reload() reads the file again (another process may have
// written it) and reports whether any key or value changed.
//
// Main thread, like UiPreferences.

#include <funkgui/prefs/UiPreferences.h>

#include <memory>
#include <string>

namespace funkgui::desktop
{
    // The store over the file at `path` (UTF-8; on Windows too).
    std::unique_ptr<UiPreferences::Backend> prefsFileBackend(std::string path);

    // The name DesktopHost gives the file inside its folder.
    inline constexpr char kPrefsFileName[] = "preferences.txt";
}

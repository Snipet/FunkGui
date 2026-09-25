#pragma once

// FunkPresets-private. The SQLite C API. On macOS it is the system library (find_package(SQLite3), 02 §1.2). On
// Windows it is the copy Windows itself ships, winsqlite3 (Windows 10 and later; the header and import library are in
// the Windows SDK), when the consumer's build defines FUNKGUI_WINSQLITE; a build that found a separate SQLite (vcpkg and
// the like) leaves it undefined. Both expose the same functions; winsqlite3 marks them __stdcall, which on x64 is the
// one calling convention anyway. (HardwareReverb Source/presets/Sqlite.h; HRVB_WINSQLITE renamed.)
#if defined(FUNKGUI_WINSQLITE)
  #include <winsqlite/winsqlite3.h>
#else
  #include <sqlite3.h>
#endif

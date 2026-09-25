#pragma once

// The SQLite C API. On macOS it is the system library. On Windows it is the
// copy Windows itself ships, winsqlite3 (Windows 10 and later; the header and
// import library are in the Windows SDK), unless the build found a separate
// SQLite (vcpkg and the like), in which case CMakeLists.txt leaves
// HRVB_WINSQLITE undefined. Both expose the same functions; winsqlite3 marks
// them __stdcall, which on x64 is the one calling convention anyway.
#if defined(HRVB_WINSQLITE)
  #include <winsqlite/winsqlite3.h>
#else
  #include <sqlite3.h>
#endif

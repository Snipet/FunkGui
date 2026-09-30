# FunkGui: an SDF/bgfx GUI library for JUCE plug-ins, consumed by FetchContent at a pinned tag. The design is
# FCompressor's docs/design/02-funkgui-and-ui.md Part 1 and 03-build-verify-process.md §1.2, §3.2. This file and
# cmake/ are written once (task G1, Sprint 0) and are not edited afterwards: sources, tools and tests are found by
# per-directory globs and self-registration, so later work adds files only (SPRINTS.md D18).
cmake_minimum_required(VERSION 3.30)

# Top-level builds only (FunkGui's own tools and tests): the deployment target must precede project(), and a consumer
# sets its own (HR CMakeLists.txt:133, 03 §2.1). Guarded so that a consumer's cache is never written from here.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS")
  # Linux (v0.11.0): Clang, as on macOS, unless the caller chose a compiler (-DCMAKE_CXX_COMPILER, or CC/CXX).
  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND NOT DEFINED CMAKE_CXX_COMPILER AND NOT DEFINED ENV{CXX})
    set(CMAKE_C_COMPILER clang)
    set(CMAKE_CXX_COMPILER clang++)
  endif()
endif()

project(FunkGui VERSION 0.11.0 LANGUAGES C CXX)       # VERSION changes only in tagging commits (02 §1.10)

# project() variables do not reach the parent scope; the consumer reads this (02 §1.7).
set(FUNKGUI_VERSION ${PROJECT_VERSION} CACHE INTERNAL "FunkGui version")

if(PROJECT_IS_TOP_LEVEL)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  endif()
  set(FUNKGUI_SHADERC "" CACHE FILEPATH "Prebuilt shaderc (empty: the stamped one in FUNKGUI_DEPS_DIR, or build it)")
  # No C++20 modules anywhere: without this, Ninja and a Clang with clang-scan-deps (Linux) scan every source twice.
  set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
  include(${PROJECT_SOURCE_DIR}/cmake/FunkGuiPlatform.cmake)
  funkgui_juce_compile_workarounds()                 # before any target: JUCE's modules compile inside FunkGui's
endif()

# ---- Options (02 §1.7). A consumer sets them as NORMAL variables before FetchContent_MakeAvailable (CMP0077). --------
option(FUNKGUI_WITH_BGFX        "FunkGui::gpu, shaders and bgfx (OFF: headless, FunkGui = FunkGuiCore)" ON)
option(FUNKGUI_WITH_PRESETS     "FunkGui::presets (FunkPresets; SQLite once it has sources)"                 ON)
option(FUNKGUI_HARNESS_ONLY     "Define only FunkGui::harness and never require JUCE (DSP-only consumers)"   OFF)
option(FUNKGUI_BUILD_TOOLS      "Tool targets (EXCLUDE_FROM_ALL; FCompressor sets ON, K2 #26d)"    ${PROJECT_IS_TOP_LEVEL})
option(FUNKGUI_BUILD_TESTS      "FunkGui's own fg.* CTest suite (02 §3.11)"                        ${PROJECT_IS_TOP_LEVEL})
option(FUNKGUI_ALLOW_OTHER_JUCE "Downgrade the JUCE 8.0.4 check to a warning"                                OFF)
option(FUNKGUI_WERROR           "-Werror on FunkGui's own sources in FunkGui's own tools and tests"          ON)
set(FUNKGUI_TRANSIENT_VB_MIB 32 CACHE STRING "bgfx transient vertex buffer, MiB (FUNKGUI_TRANSIENT_VB_BYTES, 02 §4.5)")
# FUNKGUI_SHADERC (02 §1.7): path to a prebuilt shaderc; a consumer sets it as a normal variable (03 §2.5).

if(NOT FUNKGUI_TRANSIENT_VB_MIB MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR "FunkGui: FUNKGUI_TRANSIENT_VB_MIB must be a positive integer, not '${FUNKGUI_TRANSIENT_VB_MIB}'")
endif()
if(FUNKGUI_HARNESS_ONLY)
  set(FUNKGUI_BUILD_TOOLS OFF)                       # tools need JUCE
endif()
if(FUNKGUI_BUILD_TESTS AND NOT FUNKGUI_HARNESS_ONLY AND NOT FUNKGUI_BUILD_TOOLS)
  message(FATAL_ERROR "FunkGui: FUNKGUI_BUILD_TESTS needs FUNKGUI_BUILD_TOOLS (fg.font.probe and fg.prefs.check run "
                      "tools)")
endif()

include(${PROJECT_SOURCE_DIR}/cmake/FunkGuiDeps.cmake)
include(${PROJECT_SOURCE_DIR}/cmake/FunkGuiTargets.cmake)

if(FUNKGUI_BUILD_TESTS)
  enable_testing()
  add_subdirectory(test)
endif()

set(_fg_summary "FunkGui::harness")
foreach(_t core gpu presets)
  if(TARGET FunkGui::${_t})
    string(APPEND _fg_summary ", FunkGui::${_t}")
  endif()
endforeach()
if(TARGET FunkPresets)
  get_target_property(_fg_presets_sources FunkPresets INTERFACE_SOURCES)
  if(NOT _fg_presets_sources)
    string(APPEND _fg_summary " (presets: empty placeholder)")
  endif()
endif()
if(FUNKGUI_WITH_BGFX AND NOT FUNKGUI_HARNESS_ONLY)
  if(FUNKGUI_SHADERC)
    string(APPEND _fg_summary "; shaderc: ${FUNKGUI_SHADERC}")
  else()
    string(APPEND _fg_summary "; shaderc: built from bgfx.cmake")
  endif()
endif()
message(STATUS "FunkGui ${FUNKGUI_VERSION}: ${_fg_summary}")

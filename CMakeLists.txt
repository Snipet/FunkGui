# FunkGui v0.0.1: bootstrap CMake (lead pre-work G0; FCompressor docs/design/02-funkgui-and-ui.md §2.1 step 3,
# SPRINTS.md S0.L3). Only FunkGui::harness is real. FunkGui::core, ::gpu and ::presets are EMPTY placeholders and the
# three consumer functions only print, so FCompressor's final link lines configure against v0.0.1 without JUCE or bgfx.
# G1 (v0.1.0) replaces this file with cmake/FunkGuiDeps.cmake + cmake/FunkGuiTargets.cmake; the target, option and
# function names below are frozen from FZ0 (02 §1.2, §1.7, §1.8; 03 §1.2).
cmake_minimum_required(VERSION 3.30)
project(FunkGui VERSION 0.0.1 LANGUAGES C CXX)       # VERSION changes only in tagging commits (02 §1.10)

# project() variables do not reach the parent scope; the consumer reads this (02 §1.7).
set(FUNKGUI_VERSION ${PROJECT_VERSION} CACHE INTERNAL "FunkGui version")

# Options (02 §1.7). Accepted now so consumers can set them as normal variables (CMP0077); only
# FUNKGUI_HARNESS_ONLY and the two target switches change anything at v0.0.1.
option(FUNKGUI_WITH_BGFX    "GPU target (FunkGui::gpu), shaders, bgfx"                  ON)
option(FUNKGUI_WITH_PRESETS "FunkPresets (FunkGui::presets)"                             ON)
option(FUNKGUI_HARNESS_ONLY "Define only FunkGui::harness and never require JUCE"        OFF)
option(FUNKGUI_BUILD_TOOLS  "Tool targets (none exist at v0.0.1)"                        ${PROJECT_IS_TOP_LEVEL})

# FunkGuiHarness (alias FunkGui::harness): header-only, JUCE-free Harness v2, available in every configuration.
add_library(FunkGuiHarness INTERFACE)
add_library(FunkGui::harness ALIAS FunkGuiHarness)
target_include_directories(FunkGuiHarness INTERFACE ${PROJECT_SOURCE_DIR}/include)
target_compile_features(FunkGuiHarness INTERFACE cxx_std_20)

# Empty placeholders, defined under the same conditions as the real targets (02 §1.2).
if(NOT FUNKGUI_HARNESS_ONLY)
  add_library(FunkGuiCore INTERFACE)
  add_library(FunkGui::core ALIAS FunkGuiCore)
  if(FUNKGUI_WITH_BGFX)
    add_library(FunkGuiGpu INTERFACE)
    add_library(FunkGui::gpu ALIAS FunkGuiGpu)
  endif()
  if(FUNKGUI_WITH_PRESETS)
    add_library(FunkPresets INTERFACE)
    add_library(FunkGui::presets ALIAS FunkPresets)
  endif()
endif()

# Placeholder consumer functions (02 §1.5, §1.6, §1.8): same signatures as v0.1.0, they only print.
function(funkgui_configure_product target)
  cmake_parse_arguments(PARSE_ARGV 1 _fg "" "PRODUCT;OBJC_PREFIX;ENV_PREFIX;PREFS_FOLDER" "")
  message(STATUS "FunkGui ${FUNKGUI_VERSION} placeholder: funkgui_configure_product(${target} PRODUCT ${_fg_PRODUCT}"
                 " OBJC_PREFIX ${_fg_OBJC_PREFIX} ENV_PREFIX ${_fg_ENV_PREFIX} PREFS_FOLDER ${_fg_PREFS_FOLDER})"
                 " defines nothing before v0.1.0")
endfunction()
function(funkgui_compile_shaders target)
  message(STATUS "FunkGui ${FUNKGUI_VERSION} placeholder: funkgui_compile_shaders(${target}) compiles no shaders"
                 " before v0.1.0")
endfunction()
function(funkgui_add_font target)
  message(STATUS "FunkGui ${FUNKGUI_VERSION} placeholder: funkgui_add_font(${target}) bundles no font or licences"
                 " before v0.1.0")
endfunction()

set(_fg_targets "FunkGui::harness")
foreach(_t core gpu presets)
  if(TARGET FunkGui::${_t})
    string(APPEND _fg_targets ", FunkGui::${_t} (empty placeholder)")
  endif()
endforeach()
message(STATUS "FunkGui ${FUNKGUI_VERSION} (bootstrap): ${_fg_targets}")
if(PROJECT_IS_TOP_LEVEL)                             # exercise the placeholder signatures on a standalone configure
  funkgui_configure_product(FunkGuiHarness PRODUCT FunkGui OBJC_PREFIX FunkGui ENV_PREFIX FUNKGUI_ PREFS_FOLDER FunkGui)
  funkgui_compile_shaders(FunkGuiHarness)
  funkgui_add_font(FunkGuiHarness)
endif()

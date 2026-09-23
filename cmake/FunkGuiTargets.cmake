# FunkGui targets and consumer functions (FCompressor docs/design/02-funkgui-and-ui.md §1.2, §1.5–§1.9; 03 §1.2).
# Included once from FunkGui's top-level CMakeLists.txt, after cmake/FunkGuiDeps.cmake.
#
# Frozen from FZ0 (add, never rename or remove): the targets FunkGui::{harness,core,gpu,presets} (FunkGuiHarness,
# FunkGuiCore, FunkGuiGpu, FunkPresets), FunkGui, FunkGuiFonts, FunkGuiShaders, the tool targets, FUNKGUI_VERSION,
# FUNKGUI_GENERATED_DIR, and the functions funkgui_configure_product, funkgui_compile_shaders, funkgui_add_font and
# funkgui_add_tool.
#
# Every source list is a per-directory glob with CONFIGURE_DEPENDS, so later cards add files, never CMake edits
# (03 §1.1, K3 #2):
#   src/{core,text,canvas,panel,params,widgets,a11y,live,prefs,juce}/**   -> FunkGuiCore  (C++ only, no bgfx, no ObjC)
#   src/gpu/**                                                           -> FunkGuiGpu   (.cpp and .mm)
#   src/presets/**                                                       -> FunkPresets  (empty placeholder until then)
#   tools/*.cpp                                                          -> tool FunkGui<Stem> (FrameRender.cpp ->
#                                                                           funkgui_framerender); tools/*/CMakeLists.txt
#                                                                           are added as subdirectories

set(FUNKGUI_GENERATED_DIR ${FunkGui_BINARY_DIR}/generated CACHE INTERNAL "FunkGui generated headers (shaders)")

# State the consumer-facing functions need. They run in the consumer's scope, which cannot see FunkGui's normal
# variables, so it lives in global properties.
set_property(GLOBAL PROPERTY FUNKGUI_SOURCE_DIR "${PROJECT_SOURCE_DIR}")
if(FUNKGUI_WITH_BGFX AND NOT FUNKGUI_HARNESS_ONLY)
  set_property(GLOBAL PROPERTY FUNKGUI_GPU ON)
else()
  set_property(GLOBAL PROPERTY FUNKGUI_GPU OFF)
endif()

# Warnings for FunkGui's own sources in FunkGui's own targets (tools, tests): the FunkGui test rule of 02 §3.11 and
# CLAUDE.md. Applied per source file, so JUCE's and bgfx's sources compiled into the same targets never get them.
set(FUNKGUI_STRICT_WARNINGS -Wall -Wextra -Wshadow -Wpedantic)
if(FUNKGUI_WERROR)
  list(APPEND FUNKGUI_STRICT_WARNINGS -Werror)
endif()
set_property(GLOBAL PROPERTY FUNKGUI_STRICT_WARNINGS "${FUNKGUI_STRICT_WARNINGS}")

#=======================================================================================================================
# FunkGuiHarness (FunkGui::harness): header-only, JUCE-free Harness v2 (03 §3.2). Available in every configuration.
#=======================================================================================================================
add_library(FunkGuiHarness INTERFACE)
add_library(FunkGui::harness ALIAS FunkGuiHarness)
target_include_directories(FunkGuiHarness INTERFACE ${PROJECT_SOURCE_DIR}/include)
target_compile_features(FunkGuiHarness INTERFACE cxx_std_20)

#=======================================================================================================================
# Consumer functions (02 §1.5, §1.6, §1.8; 03 §1.2). Defined in every configuration.
#=======================================================================================================================

# funkgui_configure_product(<target> PRODUCT <name> OBJC_PREFIX <identifier> ENV_PREFIX <prefix> PREFS_FOLDER <folder>)
#
# Product identity as PRIVATE definitions on <target> (02 §1.8), read by include/funkgui/core/Config.h, which fails to
# compile when any is missing. Also gives FunkGui's sources compiled in <target> their own module's public header
# directory for the snapshot's same-stem includes (src/<m>/X.cpp: #include "X.h" -> include/funkgui/<m>/X.h), set per
# source file so that no flat include directory reaches the consumer's own code.
function(funkgui_configure_product target)
  cmake_parse_arguments(PARSE_ARGV 1 _fg "" "PRODUCT;OBJC_PREFIX;ENV_PREFIX;PREFS_FOLDER" "")
  if(NOT TARGET ${target})
    message(FATAL_ERROR "funkgui_configure_product: '${target}' is not a target")
  endif()
  if(_fg_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "funkgui_configure_product(${target}): unexpected arguments '${_fg_UNPARSED_ARGUMENTS}'")
  endif()
  foreach(_k PRODUCT OBJC_PREFIX ENV_PREFIX PREFS_FOLDER)
    if("${_fg_${_k}}" STREQUAL "")
      message(FATAL_ERROR "funkgui_configure_product(${target}): ${_k} is required "
                          "(PRODUCT <name> OBJC_PREFIX <identifier> ENV_PREFIX <prefix> PREFS_FOLDER <folder>)")
    endif()
  endforeach()
  if(NOT _fg_PRODUCT MATCHES "^[^\"\\\\]+$")
    message(FATAL_ERROR "funkgui_configure_product(${target}): PRODUCT '${_fg_PRODUCT}' may not contain quotes or "
                        "backslashes")
  endif()
  if(NOT _fg_OBJC_PREFIX MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
    message(FATAL_ERROR "funkgui_configure_product(${target}): OBJC_PREFIX '${_fg_OBJC_PREFIX}' is not an identifier")
  endif()
  if(NOT _fg_ENV_PREFIX MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
    message(FATAL_ERROR "funkgui_configure_product(${target}): ENV_PREFIX '${_fg_ENV_PREFIX}' must be letters, "
                        "digits and underscores (e.g. FCMP_)")
  endif()
  if(NOT _fg_PREFS_FOLDER MATCHES "^[^\"\\\\/]+$" OR _fg_PREFS_FOLDER MATCHES "^\\.\\.?$")
    message(FATAL_ERROR "funkgui_configure_product(${target}): PREFS_FOLDER '${_fg_PREFS_FOLDER}' must be one folder "
                        "name")
  endif()

  target_compile_definitions(${target} PRIVATE
      FUNKGUI_PRODUCT_NAME="${_fg_PRODUCT}"
      FUNKGUI_OBJC_PREFIX=${_fg_OBJC_PREFIX}
      FUNKGUI_ENV_PREFIX="${_fg_ENV_PREFIX}"
      FUNKGUI_PREFS_FOLDER="${_fg_PREFS_FOLDER}")
  set_target_properties(${target} PROPERTIES
      FUNKGUI_PRODUCT "${_fg_PRODUCT}" FUNKGUI_OBJC_PREFIX "${_fg_OBJC_PREFIX}"
      FUNKGUI_ENV_PREFIX "${_fg_ENV_PREFIX}" FUNKGUI_PREFS_FOLDER "${_fg_PREFS_FOLDER}")

  get_property(_modules GLOBAL PROPERTY FUNKGUI_SOURCE_MODULES)
  get_property(_root GLOBAL PROPERTY FUNKGUI_SOURCE_DIR)
  foreach(_m IN LISTS _modules)
    get_property(_srcs GLOBAL PROPERTY FUNKGUI_SOURCES_${_m})
    if(_srcs AND IS_DIRECTORY "${_root}/include/funkgui/${_m}")
      set_source_files_properties(${_srcs} TARGET_DIRECTORY ${target}
                                  PROPERTIES INCLUDE_DIRECTORIES "${_root}/include/funkgui/${_m}")
    endif()
  endforeach()
endfunction()

# funkgui_compile_shaders(<target>): build FunkGuiShaders (the embedded Metal headers) before <target> (02 §1.5). An
# INTERFACE library cannot carry a build-order dependency, so every GPU consumer calls it. In a configuration without
# bgfx there are no shaders and the call does nothing.
function(funkgui_compile_shaders target)
  if(NOT TARGET ${target})
    message(FATAL_ERROR "funkgui_compile_shaders: '${target}' is not a target")
  endif()
  get_property(_gpu GLOBAL PROPERTY FUNKGUI_GPU)
  if(NOT _gpu)
    message(STATUS "FunkGui: funkgui_compile_shaders(${target}): FUNKGUI_WITH_BGFX is OFF, no shaders")
    return()
  endif()
  add_dependencies(${target} FunkGuiShaders)
endfunction()

# funkgui_add_font(<target>): the OFL licence of the bundled face (-> Resources/) and the bgfx, bx, bimg and bgfx.cmake
# licences taken from ${bgfx_SOURCE_DIR} at configure time (-> Resources/licences/), as MACOSX_PACKAGE_LOCATION sources
# of each of <target>_AU, <target>_VST3 and <target>_Standalone that exists, or of <target> itself when it is an app
# bundle (02 §1.6). Resources, never a POST_BUILD copy: a copy after JUCE's ad-hoc codesign broke the seal (HR
# CMakeLists.txt:379-400). GPU configurations only: a headless consumer has no GPU code and no bundled font to credit.
function(funkgui_add_font target)
  if(NOT TARGET ${target})
    message(FATAL_ERROR "funkgui_add_font: '${target}' is not a target")
  endif()
  get_property(_gpu GLOBAL PROPERTY FUNKGUI_GPU)
  if(NOT _gpu)
    message(STATUS "FunkGui: funkgui_add_font(${target}): FUNKGUI_WITH_BGFX is OFF, no font licences to bundle")
    return()
  endif()
  get_property(_root GLOBAL PROPERTY FUNKGUI_SOURCE_DIR)
  get_property(_licences GLOBAL PROPERTY FUNKGUI_LICENCE_FILES)
  set(_font_licence "${_root}/fonts/JetBrainsMono-LICENSE.txt")

  set(_bundles "")
  foreach(_fmt AU VST3 Standalone)
    if(TARGET ${target}_${_fmt})
      list(APPEND _bundles ${target}_${_fmt})
    endif()
  endforeach()
  if(NOT _bundles)
    get_target_property(_is_bundle ${target} MACOSX_BUNDLE)
    if(_is_bundle)
      set(_bundles ${target})
    else()
      message(FATAL_ERROR "funkgui_add_font(${target}): neither ${target}_{AU,VST3,Standalone} nor ${target} is a "
                          "bundle")
    endif()
  endif()
  foreach(_b IN LISTS _bundles)
    target_sources(${_b} PRIVATE "${_font_licence}" ${_licences})
    set_source_files_properties("${_font_licence}" TARGET_DIRECTORY ${_b}
                                PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_source_files_properties(${_licences} TARGET_DIRECTORY ${_b}
                                PROPERTIES MACOSX_PACKAGE_LOCATION Resources/licences)
  endforeach()
endfunction()

# Internal: FunkGui's own tools and tests. They link the JUCE modules precompiled once into FunkGuiJuce (below)
# instead of compiling every module again per executable; they build with -ffp-contract=off (arch-neutral goldens,
# K2 #26g) and FunkGui's own sources, including SOURCES given here, get FUNKGUI_STRICT_WARNINGS.
function(_funkgui_internal_target target)
  set_target_properties(${target} PROPERTIES FUNKGUI_PRECOMPILED_JUCE ON)
  target_compile_options(${target} PRIVATE -ffp-contract=off)
  target_compile_features(${target} PRIVATE cxx_std_20)
  get_property(_warn GLOBAL PROPERTY FUNKGUI_STRICT_WARNINGS)
  get_property(_lib GLOBAL PROPERTY FUNKGUI_ALL_SOURCES)
  if(ARGN OR _lib)
    set_source_files_properties(${ARGN} ${_lib} TARGET_DIRECTORY ${target} PROPERTIES COMPILE_OPTIONS "${_warn}")
  endif()
endfunction()

# funkgui_add_tool(<name> SOURCES <files>... [GPU])
#
# A FunkGui tool (02 §1.2): a JUCE console app, EXCLUDE_FROM_ALL, linking FunkGui::core (+ ::gpu with GPU) and
# FunkGui::harness, with FunkGui's own product identity (environment prefix FUNKGUI_). Used for tools/*.cpp and by
# tools/<Name>/CMakeLists.txt. Built only when FUNKGUI_BUILD_TOOLS is ON; `funkgui_tools` builds them all.
function(funkgui_add_tool name)
  cmake_parse_arguments(PARSE_ARGV 1 _fg "GPU" "" "SOURCES")
  if(NOT _fg_SOURCES OR _fg_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "funkgui_add_tool(${name} SOURCES <files>... [GPU])")
  endif()
  get_property(_gpu GLOBAL PROPERTY FUNKGUI_GPU)
  if(_fg_GPU AND NOT _gpu)
    message(STATUS "FunkGui: tool ${name} needs FUNKGUI_WITH_BGFX; not defined")
    return()
  endif()
  juce_add_console_app(${name} PRODUCT_NAME ${name})
  set_target_properties(${name} PROPERTIES EXCLUDE_FROM_ALL TRUE)
  target_sources(${name} PRIVATE ${_fg_SOURCES})
  target_link_libraries(${name} PRIVATE FunkGui::core FunkGui::harness)
  funkgui_configure_product(${name} PRODUCT FunkGui OBJC_PREFIX FunkGuiTool ENV_PREFIX FUNKGUI_
                            PREFS_FOLDER FunkGui)
  if(_fg_GPU)
    target_link_libraries(${name} PRIVATE FunkGui::gpu)
    funkgui_compile_shaders(${name})
  endif()
  _funkgui_internal_target(${name} ${_fg_SOURCES})
  set_property(GLOBAL APPEND PROPERTY FUNKGUI_TOOL_TARGETS ${name})
endfunction()

if(FUNKGUI_HARNESS_ONLY)
  return()                                   # DSP-only consumers: FunkGui::harness and the functions, nothing else
endif()

#=======================================================================================================================
# Source globs, per directory (02 §1.1: anything that includes bgfx or Objective-C lives under gpu/)
#=======================================================================================================================
set(_fg_core_modules core text canvas panel params widgets a11y live prefs juce)
set(_fg_core_sources "")
set(_fg_all_sources "")
foreach(_m IN LISTS _fg_core_modules ITEMS gpu presets)
  file(GLOB_RECURSE _cpp CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/src/${_m}/*.cpp)
  file(GLOB_RECURSE _mm CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/src/${_m}/*.mm)
  if(_mm AND NOT _m STREQUAL "gpu")
    message(FATAL_ERROR "FunkGui: Objective-C++ belongs under src/gpu/ (02 §1.1): ${_mm}")
  endif()
  set(_fg_sources_${_m} ${_cpp} ${_mm})
  set_property(GLOBAL PROPERTY FUNKGUI_SOURCES_${_m} "${_fg_sources_${_m}}")
  set_property(GLOBAL APPEND PROPERTY FUNKGUI_SOURCE_MODULES ${_m})
  list(APPEND _fg_all_sources ${_fg_sources_${_m}})
  if(_m IN_LIST _fg_core_modules)
    list(APPEND _fg_core_sources ${_fg_sources_${_m}})
  endif()
endforeach()
set_property(GLOBAL PROPERTY FUNKGUI_ALL_SOURCES "${_fg_all_sources}")

# JUCE module links of the library targets. A consumer gets the modules themselves (so FunkGui compiles with the
# consumer's JUCE configuration, 02 §1.3); FunkGui's own tools and tests (FUNKGUI_PRECOMPILED_JUCE) get FunkGuiJuce.
set(_fg_precompiled "$<BOOL:$<TARGET_PROPERTY:FUNKGUI_PRECOMPILED_JUCE>>")
function(_funkgui_link_juce target)
  foreach(_mod IN LISTS ARGN)
    target_link_libraries(${target} INTERFACE $<$<NOT:${_fg_precompiled}>:juce::${_mod}>)
  endforeach()
  target_link_libraries(${target} INTERFACE $<${_fg_precompiled}:FunkGuiJuce>)
endfunction()

#=======================================================================================================================
# FunkGuiFonts: the bundled face and its licence as binary data (02 §1.6). BundledFont.h names the symbols.
#=======================================================================================================================
juce_add_binary_data(FunkGuiFonts
    HEADER_NAME FunkGuiFonts.h
    NAMESPACE   funkguifonts
    SOURCES     ${PROJECT_SOURCE_DIR}/fonts/JetBrainsMono-Regular-subset.ttf
                ${PROJECT_SOURCE_DIR}/fonts/JetBrainsMono-LICENSE.txt)

#=======================================================================================================================
# FunkGuiJuce: FunkGui's own tools and tests share one compile of the JUCE modules FunkGuiCore/FunkGuiGpu need (the
# machine budget of 03 §2.11: one JUCE compile per build directory, not one per executable). Internal, never linked
# by a consumer's product; EXCLUDE_FROM_ALL.
#=======================================================================================================================
if(FUNKGUI_BUILD_TOOLS OR FUNKGUI_BUILD_TESTS)
  add_library(FunkGuiJuce STATIC EXCLUDE_FROM_ALL)
  target_link_libraries(FunkGuiJuce PRIVATE juce::juce_gui_basics juce::juce_data_structures juce::juce_audio_processors)
  target_compile_definitions(FunkGuiJuce PRIVATE JUCE_STANDALONE_APPLICATION=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
  target_compile_options(FunkGuiJuce PRIVATE -ffp-contract=off)
  target_compile_features(FunkGuiJuce PUBLIC cxx_std_20)
  # Consumers compile against exactly the module configuration the archive was built with. SYSTEM: the re-exported
  # JUCE include directories stay system headers for FunkGui's -Werror sources.
  target_include_directories(FunkGuiJuce INTERFACE $<TARGET_PROPERTY:FunkGuiJuce,INCLUDE_DIRECTORIES>)
  target_compile_definitions(FunkGuiJuce INTERFACE $<TARGET_PROPERTY:FunkGuiJuce,COMPILE_DEFINITIONS>)
  set_target_properties(FunkGuiJuce PROPERTIES SYSTEM TRUE)
endif()

#=======================================================================================================================
# FunkGuiCore (FunkGui::core): CPU only, no bgfx, no Objective-C (02 §1.2)
#=======================================================================================================================
add_library(FunkGuiCore INTERFACE)
add_library(FunkGui::core ALIAS FunkGuiCore)
target_sources(FunkGuiCore INTERFACE ${_fg_core_sources})
target_include_directories(FunkGuiCore INTERFACE ${PROJECT_SOURCE_DIR}/include)
target_compile_features(FunkGuiCore INTERFACE cxx_std_20)
_funkgui_link_juce(FunkGuiCore juce_gui_basics juce_data_structures juce_audio_processors)
target_link_libraries(FunkGuiCore INTERFACE FunkGuiFonts)

#=======================================================================================================================
# FunkGuiGpu (FunkGui::gpu), FunkGuiShaders, licences: only with bgfx (02 §1.2, §1.5, §1.6)
#=======================================================================================================================
if(FUNKGUI_WITH_BGFX)
  set(_fg_out ${FUNKGUI_GENERATED_DIR}/funkgui/shaders)
  file(MAKE_DIRECTORY ${_fg_out})
  if(FUNKGUI_SHADERC)
    set(_fg_shaderc "${FUNKGUI_SHADERC}")
    set(_fg_shaderc_dep "${FUNKGUI_SHADERC}")     # a replaced prebuilt binary recompiles the shaders
  else()
    set(_fg_shaderc "$<TARGET_FILE:shaderc>")
    set(_fg_shaderc_dep shaderc)
  endif()
  # HR's hrvb_compile_shader (HR CMakeLists.txt:328-344), generalised: Metal only, embedded with --bin2c.
  function(_funkgui_compile_shader name type)
    add_custom_command(OUTPUT ${_fg_out}/${name}.mtl.h
      COMMAND ${_fg_shaderc} -f ${PROJECT_SOURCE_DIR}/shaders/${name}.sc -o ${_fg_out}/${name}.mtl.h
              --type ${type} --platform osx -p metal
              --varyingdef ${PROJECT_SOURCE_DIR}/shaders/varying.def.sc
              -i ${_fg_bgfx_src}/bgfx/src --bin2c ${name}_mtl
      DEPENDS ${_fg_shaderc_dep} ${PROJECT_SOURCE_DIR}/shaders/${name}.sc ${PROJECT_SOURCE_DIR}/shaders/varying.def.sc
      COMMENT "FunkGui shaderc: ${name}.sc -> metal"
      VERBATIM)
  endfunction()
  _funkgui_compile_shader(vs_ui vertex)
  _funkgui_compile_shader(fs_ui fragment)
  add_custom_target(FunkGuiShaders DEPENDS ${_fg_out}/vs_ui.mtl.h ${_fg_out}/fs_ui.mtl.h)

  # Third-party licences, staged from the bgfx sources in use (never copied from HR; K1 #16).
  set(_fg_licences "")
  foreach(_pair bgfx=bgfx/LICENSE bx=bx/LICENSE bimg=bimg/LICENSE bgfx.cmake=LICENSE)
    string(REPLACE "=" ";" _pair "${_pair}")
    list(GET _pair 0 _name)
    list(GET _pair 1 _rel)
    if(NOT EXISTS "${_fg_bgfx_src}/${_rel}")
      message(FATAL_ERROR "FunkGui: missing licence ${_fg_bgfx_src}/${_rel}")
    endif()
    configure_file("${_fg_bgfx_src}/${_rel}" "${FUNKGUI_GENERATED_DIR}/licences/${_name}-LICENSE.txt" COPYONLY)
    list(APPEND _fg_licences "${FUNKGUI_GENERATED_DIR}/licences/${_name}-LICENSE.txt")
  endforeach()
  set_property(GLOBAL PROPERTY FUNKGUI_LICENCE_FILES "${_fg_licences}")

  add_library(FunkGuiGpu INTERFACE)
  add_library(FunkGui::gpu ALIAS FunkGuiGpu)
  target_sources(FunkGuiGpu INTERFACE ${_fg_sources_gpu})
  target_include_directories(FunkGuiGpu INTERFACE ${FUNKGUI_GENERATED_DIR})
  target_link_libraries(FunkGuiGpu INTERFACE FunkGuiCore bgfx bx bimg)
  _funkgui_link_juce(FunkGuiGpu juce_audio_processors)
  target_compile_definitions(FunkGuiGpu INTERFACE
      FUNKGUI_HAS_BGFX=1
      "FUNKGUI_TRANSIENT_VB_BYTES=(${FUNKGUI_TRANSIENT_VB_MIB}<<20)")
endif()

#=======================================================================================================================
# FunkPresets (FunkGui::presets): the placeholder until src/presets/ has sources (G8), then the real target, the only
# one that links SQLite (02 §1.2)
#=======================================================================================================================
if(FUNKGUI_WITH_PRESETS)
  add_library(FunkPresets INTERFACE)
  add_library(FunkGui::presets ALIAS FunkPresets)
  if(_fg_sources_presets)
    find_package(SQLite3 REQUIRED)
    target_sources(FunkPresets INTERFACE ${_fg_sources_presets})
    target_include_directories(FunkPresets INTERFACE ${PROJECT_SOURCE_DIR}/include)
    target_compile_features(FunkPresets INTERFACE cxx_std_20)
    _funkgui_link_juce(FunkPresets juce_audio_processors juce_data_structures)
    target_link_libraries(FunkPresets INTERFACE SQLite3::SQLite3)
  endif()
endif()

# FunkGui (umbrella): FunkGuiCore, plus FunkGuiGpu when enabled.
add_library(FunkGui INTERFACE)
target_link_libraries(FunkGui INTERFACE FunkGuiCore)
if(TARGET FunkGuiGpu)
  target_link_libraries(FunkGui INTERFACE FunkGuiGpu)
endif()

#=======================================================================================================================
# Tools (02 §1.2; K2 #26d): every tools/*.cpp is a tool target, every tools/*/CMakeLists.txt a subdirectory
#=======================================================================================================================
if(FUNKGUI_BUILD_TOOLS)
  file(GLOB _fg_tool_sources CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/tools/*.cpp)
  foreach(_src IN LISTS _fg_tool_sources)
    get_filename_component(_stem "${_src}" NAME_WE)
    if(_stem STREQUAL "FrameRender")
      set(_tool funkgui_framerender)                        # 03's name (verify-gui-live, the DoD's PNGs)
    else()
      set(_tool FunkGui${_stem})
    endif()
    set(_extra "")
    if(_stem STREQUAL "GalleryProbe")                       # the gallery sections live under test/gallery (G3)
      file(GLOB _extra CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/test/gallery/*.cpp)
    endif()
    funkgui_add_tool(${_tool} SOURCES ${_src} ${_extra})
  endforeach()
  file(GLOB _fg_tool_dirs CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/tools/*/CMakeLists.txt)
  foreach(_cml IN LISTS _fg_tool_dirs)
    get_filename_component(_dir "${_cml}" DIRECTORY)
    add_subdirectory(${_dir})
  endforeach()
  get_property(_fg_tools GLOBAL PROPERTY FUNKGUI_TOOL_TARGETS)
  add_custom_target(funkgui_tools)
  if(_fg_tools)
    add_dependencies(funkgui_tools ${_fg_tools})
  endif()
endif()

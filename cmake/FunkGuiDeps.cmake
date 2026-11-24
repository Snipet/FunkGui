# FunkGui dependencies (FCompressor docs/design/02-funkgui-and-ui.md §1.3–§1.4; 03-build-verify-process.md §1.2,
# §2.3–§2.5). Included once from FunkGui's top-level CMakeLists.txt.
#
# Consumed (FetchContent from FCompressor): FunkGui never fetches JUCE. It checks that the consumer made the JUCE
# targets available and that they are 8.0.4. In a GPU configuration it uses the consumer's bgfx when a `bgfx` target
# exists and otherwise fetches it itself (the guarded fallback, with NORMAL variables only: a CACHE … FORCE write would
# outlive this file in the consumer's cache, K2 #17). The shaderc check accepts either a prebuilt FUNKGUI_SHADERC or a
# `shaderc` target. A prebuilt FUNKGUI_SHADERC must carry a shaderc.stamp naming the pinned bgfx.cmake SHA (FATAL at
# top level, a WARNING in a consumer, which checks its own pin).
#
# Top level (FunkGui's own tools and tests): the same ~/audio/.deps defaults, pins and SHA asserts as FCompressor's
# cmake/FcmpDeps.cmake (03 §2.3), the stamped prebuilt shaderc when it matches the pin (03 §2.5), then the checks above.
#
# Output for FunkGuiTargets.cmake: FUNKGUI_SHADERC_STAMP, the stamp line of the shaderc that compiles the shaders (the
# prebuilt one's shaderc.stamp, or "bgfx.cmake <HEAD>" of the sources a shaderc target is built from); fg.shader.hash
# asserts it against the pin with a spec row (S0 review R-G1 #9).
#
# FUNKGUI_WITH_JUCE=OFF (v0.12.0): JUCE is neither fetched, nor looked for, nor version-checked. _fg_juce below is "this
# configuration has JUCE". bgfx is fetched and checked with or without it (v0.28.0; before, bgfx went with JUCE).
#
# FUNKGUI_WITH_DESKTOP (v0.28.0): SDL3, which FunkGui fetches itself, in a consumer too, unless the consumer already
# defined SDL3::SDL3: by git at the pinned release, its commit asserted, as a static library with the subsystems the
# desktop host and an application on it use (video, events, joystick, gamepad, haptic, audio, dialogs) and no others.
# NORMAL variables only, as for bgfx. On Linux only its X11 video driver is built: FunkGui's Vulkan surface is an X11
# window's, and a Wayland desktop runs it through XWayland (README, "Desktop": the system packages).

include(FetchContent)

if(FUNKGUI_WITH_JUCE AND NOT FUNKGUI_HARNESS_ONLY)
  set(_fg_juce ON)
else()
  set(_fg_juce OFF)
endif()

# ---- Pins (03 §1.3). The consumer pins its own copies; top-level FunkGui builds assert these. ------------------------
set(FUNKGUI_JUCE_TAG  8.0.4)
set(FUNKGUI_JUCE_SHA  51d11a2be6d5c97ccf12b4e5e827006e19f0555a)
set(FUNKGUI_BGFX_TAG  v1.153.9385-561)
set(FUNKGUI_BGFX_SHA  99752df38e40179cf998bb880fe4c16c0b3d60ca)
set(FUNKGUI_BGFX_API  153)
set(FUNKGUI_BGFX_SUB_SHAS bgfx=c7684e20da1e385edc439ef39cdb42b8c661016f bx=0b001f5f36579e8aea07efa5af139ca18dad9505
                          bimg=3b4baab0128ac499c5c3bc37202781bf54084049)
set(FUNKGUI_SDL_TAG   release-3.4.20)                # v0.28.0: FunkGui::desktop (zlib licence)
set(FUNKGUI_SDL_SHA   f00dc919ca35b9756316e86c4777392de442f9b2)

# ---- Helpers ---------------------------------------------------------------------------------------------------------

# HEAD of DIR when DIR is its own git repository root ("(no HEAD)" when it has none), else "". Read-only git commands
# only (.deps is chmod a-w).
function(_funkgui_git_head dir out_var)
  set(${out_var} "" PARENT_SCOPE)
  execute_process(COMMAND git -C "${dir}" rev-parse --show-toplevel
                  OUTPUT_VARIABLE _top OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _rc ERROR_QUIET)
  if(NOT _rc EQUAL 0)
    return()
  endif()
  file(REAL_PATH "${_top}" _top)
  file(REAL_PATH "${dir}" _dir)
  if(NOT _top STREQUAL _dir)
    return()
  endif()
  execute_process(COMMAND git -C "${dir}" rev-parse --verify HEAD
                  OUTPUT_VARIABLE _head OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _rc ERROR_QUIET)
  if(NOT _rc EQUAL 0 OR _head STREQUAL "")
    set(_head "(no HEAD)")
  endif()
  set(${out_var} "${_head}" PARENT_SCOPE)
endfunction()

# DIR must be its own git repository root and its HEAD must equal SHA. A SHA mismatch is FATAL; a directory that is not
# a git checkout gets a WARNING (the version checks still apply).
function(_funkgui_assert_git dir sha name)
  _funkgui_git_head("${dir}" _head)
  if(_head STREQUAL "")
    message(WARNING "FunkGui: ${name} at ${dir} is not a git checkout of its own; its SHA cannot be checked")
    return()
  endif()
  if(NOT _head STREQUAL sha)
    message(FATAL_ERROR "FunkGui: ${name} at ${dir} is ${_head}, the pin is ${sha}")
  endif()
endfunction()

# The first line of <directory of SHADERC>/shaderc.stamp ("bgfx.cmake <sha>", written last by deps.sh), or "" when
# there is none. The stamp file becomes a configure dependency, so a re-stamped tool re-runs these checks.
function(_funkgui_shaderc_stamp shaderc out_var)
  get_filename_component(_dir "${shaderc}" DIRECTORY)
  set(_line "")
  if(EXISTS "${_dir}/shaderc.stamp")
    file(STRINGS "${_dir}/shaderc.stamp" _line LIMIT_COUNT 1)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_dir}/shaderc.stamp")
  endif()
  set(${out_var} "${_line}" PARENT_SCOPE)
endfunction()

# Parse JUCE's version from juce_core/system/juce_StandardHeader.h under the modules directory.
function(_funkgui_juce_version modules_dir out_var)
  set(_hdr "${modules_dir}/juce_core/system/juce_StandardHeader.h")
  if(NOT EXISTS "${_hdr}")
    message(FATAL_ERROR "FunkGui: cannot find ${_hdr} to check the JUCE version")
  endif()
  file(STRINGS "${_hdr}" _lines REGEX "^#define JUCE_(MAJOR_VERSION|MINOR_VERSION|BUILDNUMBER) +[0-9]+")
  set(_v "")
  foreach(_part MAJOR_VERSION MINOR_VERSION BUILDNUMBER)
    foreach(_l IN LISTS _lines)
      if(_l MATCHES "^#define JUCE_${_part} +([0-9]+)")
        list(APPEND _v ${CMAKE_MATCH_1})
      endif()
    endforeach()
  endforeach()
  list(JOIN _v "." _v)
  set(${out_var} "${_v}" PARENT_SCOPE)
endfunction()

# ---- A given prebuilt shaderc fails first, before JUCE or bgfx is configured (02 §1.4, 03 §2.5) ---------------------
# It must exist, and its stamp must name the pinned bgfx.cmake: a shaderc built from another bgfx would otherwise show
# only as a DRIFT row of fg.shader.hash, which passes CTest (R-G1 #9).
if(FUNKGUI_WITH_BGFX AND NOT FUNKGUI_HARNESS_ONLY AND FUNKGUI_SHADERC)
  if(NOT EXISTS "${FUNKGUI_SHADERC}")
    message(FATAL_ERROR "FunkGui: FUNKGUI_SHADERC=${FUNKGUI_SHADERC} does not exist")
  endif()
  _funkgui_shaderc_stamp("${FUNKGUI_SHADERC}" _fg_given_stamp)
  if(NOT _fg_given_stamp STREQUAL "bgfx.cmake ${FUNKGUI_BGFX_SHA}")
    if(_fg_given_stamp STREQUAL "")
      set(_fg_why "has no shaderc.stamp beside it")
    else()
      set(_fg_why "is stamped '${_fg_given_stamp}'")
    endif()
    if(PROJECT_IS_TOP_LEVEL)
      message(FATAL_ERROR "FunkGui: FUNKGUI_SHADERC=${FUNKGUI_SHADERC} ${_fg_why}; FunkGui needs a shaderc stamped "
                          "'bgfx.cmake ${FUNKGUI_BGFX_SHA}' (FCompressor's Scripts/deps.sh builds one; or leave "
                          "FUNKGUI_SHADERC empty to use the stamped one in FUNKGUI_DEPS_DIR or build it, 03 §2.5)")
    else()
      message(WARNING "FunkGui: FUNKGUI_SHADERC=${FUNKGUI_SHADERC} ${_fg_why}; FunkGui is pinned to bgfx.cmake "
                      "${FUNKGUI_BGFX_SHA} (03 §2.5)")
    endif()
  endif()
endif()

# ---- Top level: machine-cache defaults, declarations, prebuilt shaderc -----------------------------------------------
if(PROJECT_IS_TOP_LEVEL)
  # v0.28.0: the environment's FUNKGUI_DEPS_DIR names it where there is no ~/audio (a cloud container, CI).
  if(DEFINED ENV{FUNKGUI_DEPS_DIR} AND NOT "$ENV{FUNKGUI_DEPS_DIR}" STREQUAL "")
    set(_fg_deps_default "$ENV{FUNKGUI_DEPS_DIR}")
  else()
    set(_fg_deps_default "$ENV{HOME}/audio/.deps")
  endif()
  set(FUNKGUI_DEPS_DIR "${_fg_deps_default}" CACHE PATH
      "Machine dependency cache written by FCompressor's Scripts/deps.sh (03 §2.4)")

  # A NORMAL variable (never cached), and only when the user gave no override (03 §2.3 step 1).
  macro(_funkgui_default_source_dir NAME SUBDIR)
    if(NOT FETCHCONTENT_SOURCE_DIR_${NAME} AND EXISTS "${FUNKGUI_DEPS_DIR}/${SUBDIR}/CMakeLists.txt")
      set(FETCHCONTENT_SOURCE_DIR_${NAME} "${FUNKGUI_DEPS_DIR}/${SUBDIR}")
    endif()
  endmacro()

  if(_fg_juce)
    _funkgui_default_source_dir(JUCE JUCE-${FUNKGUI_JUCE_TAG})
    # SYSTEM: JUCE's headers are third-party code to FunkGui's -Werror sources.
    FetchContent_Declare(JUCE GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG ${FUNKGUI_JUCE_TAG}
                              GIT_SHALLOW TRUE SYSTEM)
  endif()

  if(FUNKGUI_WITH_BGFX AND NOT FUNKGUI_HARNESS_ONLY)
    _funkgui_default_source_dir(BGFX bgfx.cmake-${FUNKGUI_BGFX_TAG})
    if(FUNKGUI_WITH_DESKTOP)
      _funkgui_default_source_dir(SDL3 SDL-${FUNKGUI_SDL_TAG})
    endif()
    # The prebuilt shaderc (03 §2.5): used only when its stamp names the pinned bgfx.cmake SHA; otherwise the guarded
    # fetch below builds shaderc from the pinned sources (HR's tool settings).
    if(NOT FUNKGUI_SHADERC)
      set(_fg_prebuilt "${FUNKGUI_DEPS_DIR}/tools/shaderc-${FUNKGUI_BGFX_TAG}/shaderc")
      if(CMAKE_HOST_WIN32)                             # v0.28.0: FunkGui's own build on Windows
        string(APPEND _fg_prebuilt ".exe")
      endif()
      set(_fg_stamp "${FUNKGUI_DEPS_DIR}/tools/shaderc-${FUNKGUI_BGFX_TAG}/shaderc.stamp")
      set(_fg_stamp_line "")
      if(EXISTS "${_fg_stamp}")
        file(STRINGS "${_fg_stamp}" _fg_stamp_line LIMIT_COUNT 1)
      endif()
      if(EXISTS "${_fg_prebuilt}" AND _fg_stamp_line STREQUAL "bgfx.cmake ${FUNKGUI_BGFX_SHA}")
        set(FUNKGUI_SHADERC "${_fg_prebuilt}")
        message(STATUS "FunkGui: prebuilt shaderc ${FUNKGUI_SHADERC} (stamp: ${_fg_stamp_line})")
      else()
        message(STATUS "FunkGui: no prebuilt shaderc stamped '${FUNKGUI_BGFX_SHA}' in ${FUNKGUI_DEPS_DIR}/tools; "
                       "building shaderc from bgfx.cmake (~2,000 CPU-s, 03 §2.5)")
      endif()
    endif()
  endif()

  if(_fg_juce)
    FetchContent_MakeAvailable(JUCE)
    _funkgui_assert_git("${juce_SOURCE_DIR}" ${FUNKGUI_JUCE_SHA} JUCE)
  endif()
endif()

# ---- JUCE: provided by the consumer (02 §1.3) ------------------------------------------------------------------------
if(_fg_juce)
  if(NOT TARGET juce::juce_gui_basics OR NOT COMMAND juce_add_binary_data)
    message(FATAL_ERROR "FunkGui: call FetchContent_MakeAvailable(JUCE) before FunkGui "
                        "(or set FUNKGUI_HARNESS_ONLY=ON for a JUCE-free build with FunkGui::harness only, or "
                        "FUNKGUI_WITH_JUCE=OFF for the JUCE-free FunkGui::core)")
  endif()
  set(_fg_juce_modules "${JUCE_MODULES_DIR}")
  if(NOT _fg_juce_modules)
    FetchContent_GetProperties(JUCE SOURCE_DIR _fg_juce_src)
    set(_fg_juce_modules "${_fg_juce_src}/modules")
  endif()
  _funkgui_juce_version("${_fg_juce_modules}" _fg_juce_version)
  if(NOT _fg_juce_version VERSION_EQUAL FUNKGUI_JUCE_TAG)
    if(FUNKGUI_ALLOW_OTHER_JUCE)
      message(WARNING "FunkGui: JUCE ${_fg_juce_version} at ${_fg_juce_modules}; FunkGui is pinned to "
                      "${FUNKGUI_JUCE_TAG} (FUNKGUI_ALLOW_OTHER_JUCE=ON)")
    else()
      message(FATAL_ERROR "FunkGui: JUCE ${_fg_juce_version} at ${_fg_juce_modules}; FunkGui needs "
                          "${FUNKGUI_JUCE_TAG} (set FUNKGUI_ALLOW_OTHER_JUCE=ON to downgrade this to a warning)")
    endif()
  endif()
endif()

# ---- bgfx and shaderc: only when FUNKGUI_WITH_BGFX (02 §1.4) ---------------------------------------------------------
if(FUNKGUI_WITH_BGFX AND NOT FUNKGUI_HARNESS_ONLY)
  if(NOT APPLE AND NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "FunkGui: FUNKGUI_WITH_BGFX needs macOS (Metal, Objective-C++ views), Linux (Vulkan on an X11 "
                        "child window) or Windows (Direct3D 11 on a child window); this is ${CMAKE_SYSTEM_NAME}")
  endif()
  if(NOT TARGET bgfx)
    # HR CMakeLists.txt:307-313 settings as NORMAL variables (CMP0077; bgfx.cmake requires CMake 3.20, so its
    # option() calls honour them). With a prebuilt shaderc the tool chain (tint, spirv-*, glslang: ~97 % of a cold
    # bgfx build, 03 §2.5) is not configured at all.
    if(FUNKGUI_SHADERC)
      set(BGFX_BUILD_TOOLS OFF)
    else()
      set(BGFX_BUILD_TOOLS ON)
      set(BGFX_BUILD_TOOLS_SHADER ON)
      set(BGFX_BUILD_TOOLS_TEXTURE OFF)
      set(BGFX_BUILD_TOOLS_GEOMETRY OFF)
      set(BGFX_BUILD_TOOLS_BIN2C OFF)
    endif()
    set(BGFX_BUILD_EXAMPLES OFF)
    set(BGFX_INSTALL OFF)
    # Linux: JUCE's peers are X11 windows (a Wayland desktop runs them through XWayland), so bgfx never sees a
    # wl_surface, and with its Wayland backend on bgfx links libwayland-egl into every plug-in binary (v0.11.0).
    set(BGFX_WITH_WAYLAND OFF)
    # First declaration wins: a no-op if the consumer declared bgfx. SYSTEM: bgfx headers are third-party code.
    # EXCLUDE_FROM_ALL: only what FunkGui::gpu links (bgfx, bx, bimg) is built, not bimg_encode and the like.
    FetchContent_Declare(bgfx GIT_REPOSITORY https://github.com/bkaradzic/bgfx.cmake.git GIT_TAG ${FUNKGUI_BGFX_TAG}
                              GIT_SHALLOW TRUE SYSTEM EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(bgfx)
    foreach(_t bgfx bx bimg)                                            # hidden symbols (K2 #26f)
      set_target_properties(${_t} PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
    endforeach()
    # Linux (v0.11.0): the pinned bgfx warns under Clang 22 in its own sources (renderer_gl.cpp's
    # -Wtautological-constant-compare, bimg's miniz #pragma message). Third-party code FunkGui does not edit. Windows
    # (v0.24.0) too: -w is cl's /w for clang-cl and MSVC.
    if(NOT APPLE)
      foreach(_t bgfx bx bimg bimg_decode bimg_encode)
        if(TARGET ${_t})
          target_compile_options(${_t} PRIVATE -w)
        endif()
      endforeach()
    endif()
  endif()

  if(NOT FUNKGUI_SHADERC AND NOT TARGET shaderc)
    message(FATAL_ERROR "FunkGui: no shaderc (set FUNKGUI_SHADERC to a prebuilt shaderc, or build bgfx with "
                        "BGFX_BUILD_TOOLS=ON and BGFX_BUILD_TOOLS_SHADER=ON)")
  endif()

  # bgfx's source directory: project(bgfx) caches bgfx_SOURCE_DIR; FetchContent's property is the fallback.
  if(bgfx_SOURCE_DIR)
    set(_fg_bgfx_src "${bgfx_SOURCE_DIR}")
  else()
    FetchContent_GetProperties(bgfx SOURCE_DIR _fg_bgfx_src)
  endif()
  if(NOT EXISTS "${_fg_bgfx_src}/bgfx/include/bgfx/defines.h")
    message(FATAL_ERROR "FunkGui: cannot find bgfx's sources (bgfx_SOURCE_DIR='${_fg_bgfx_src}')")
  endif()
  # FETCHCONTENT_SOURCE_DIR_BGFX bypasses GIT_TAG (B §7.2), so the API version is checked on every configure.
  file(STRINGS "${_fg_bgfx_src}/bgfx/include/bgfx/defines.h" _fg_api
       REGEX "define BGFX_API_VERSION UINT32_C\\(${FUNKGUI_BGFX_API}\\)")
  if(NOT _fg_api)
    message(FATAL_ERROR "FunkGui: bgfx API ${FUNKGUI_BGFX_API} required (bgfx.cmake ${FUNKGUI_BGFX_TAG}) at "
                        "${_fg_bgfx_src}")
  endif()
  if(PROJECT_IS_TOP_LEVEL)
    _funkgui_assert_git("${_fg_bgfx_src}" ${FUNKGUI_BGFX_SHA} bgfx.cmake)
    foreach(_pair IN LISTS FUNKGUI_BGFX_SUB_SHAS)
      string(REPLACE "=" ";" _pair "${_pair}")
      list(GET _pair 0 _sub)
      list(GET _pair 1 _sha)
      _funkgui_assert_git("${_fg_bgfx_src}/${_sub}" ${_sha} bgfx.cmake/${_sub})
    endforeach()
  endif()

  # FUNKGUI_SHADERC_STAMP (see the top of this file): what the shaders are compiled with.
  if(FUNKGUI_SHADERC)
    _funkgui_shaderc_stamp("${FUNKGUI_SHADERC}" FUNKGUI_SHADERC_STAMP)
  else()
    _funkgui_git_head("${_fg_bgfx_src}" _fg_head)
    if(NOT _fg_head STREQUAL "")
      set(FUNKGUI_SHADERC_STAMP "bgfx.cmake ${_fg_head}")
    else()
      set(FUNKGUI_SHADERC_STAMP "bgfx.cmake (unknown: ${_fg_bgfx_src} is not a git checkout)")
    endif()
  endif()
endif()

# ---- SDL3: only when FUNKGUI_WITH_DESKTOP (v0.28.0; the top of this file) --------------------------------------------
if(FUNKGUI_WITH_DESKTOP AND NOT FUNKGUI_HARNESS_ONLY)
  if(TARGET SDL3::SDL3)
    message(STATUS "FunkGui: FunkGui::desktop on the consumer's SDL3::SDL3 (FunkGui is pinned to ${FUNKGUI_SDL_TAG})")
  else()
    set(SDL_SHARED OFF)
    set(SDL_STATIC ON)
    set(SDL_TEST_LIBRARY OFF)
    set(SDL_TESTS OFF)
    set(SDL_EXAMPLES OFF)
    set(SDL_INSTALL OFF)
    set(SDL_UNINSTALL OFF)
    # The subsystems nothing here uses. Events, threads, timers and the rest of SDL's core are always built.
    foreach(_fg_sdl_off GPU RENDER CAMERA SENSOR POWER TRAY)
      set(SDL_${_fg_sdl_off} OFF)
    endforeach()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      # X11 only (the top of this file), and no OpenGL, GLES or Vulkan of SDL's own: bgfx makes the Vulkan surface.
      set(SDL_X11 ON)
      foreach(_fg_sdl_off WAYLAND KMSDRM OPENGL OPENGLES VULKAN)
        set(SDL_${_fg_sdl_off} OFF)
      endforeach()
    endif()
    # First declaration wins: a no-op if the consumer declared SDL3. SYSTEM: SDL's headers are third-party code.
    # EXCLUDE_FROM_ALL: only what FunkGui::desktop links is built.
    FetchContent_Declare(SDL3 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG ${FUNKGUI_SDL_TAG}
                              GIT_SHALLOW TRUE SYSTEM EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(SDL3)
    if(NOT TARGET SDL3::SDL3)
      message(FATAL_ERROR "FunkGui: SDL ${FUNKGUI_SDL_TAG} defined no SDL3::SDL3")
    endif()
    FetchContent_GetProperties(SDL3 SOURCE_DIR _fg_sdl_src)
    _funkgui_assert_git("${_fg_sdl_src}" ${FUNKGUI_SDL_SHA} SDL)
    # Third-party code FunkGui does not edit: its warnings are not FunkGui's (as bgfx's, above).
    if(TARGET SDL3-static)
      target_compile_options(SDL3-static PRIVATE -w)
    endif()
    set(FUNKGUI_SDL_SOURCE_DIR "${_fg_sdl_src}")
  endif()
endif()

# FunkGui's own tests (FCompressor docs/design/02-funkgui-and-ui.md §3.11; SPRINTS.md D18). Written once in Sprint 0:
# later cards add test files, never edit this one.
#
# Self-registration (mirrors FCompressor's FCMP_PROBE, 03 §2.9). Any test/**/*.cpp or test/**/*.mm whose FIRST line is
#
#   // FUNKGUI_TEST name=fg.<x> timeout=<s> gpu=<0|1>[ links=<harness|core|gpu|presets>][ exe=<tool>[ args="<text>"]][ labels=<l>[,<l>…]]
#
# (keys in that order, single spaces) is one CTest test named fg.<x>:
# - Without exe=, the file is compiled into its own executable, run as `<exe> fg.<x> <harness flags>`. links= picks
#   what it links: harness (FunkGui::harness only, no JUCE), core (+ FunkGui::core; the default when gpu=0), gpu
#   (+ FunkGui::gpu and the shaders; the default when gpu=1) or presets (core + FunkGui::presets and SQLite; G8, S11;
#   skipped without FUNKGUI_WITH_PRESETS). Every test executable gets ${FUNKGUI_GENERATED_DIR} on its include path and,
#   when gpu=1, builds after FunkGuiShaders.
# - With exe=<tool target>, the file is a registration stub and is not compiled: the test runs
#   `<tool> fg.<x> <args> <harness flags>`. With exe=<path of a script in this repository> it runs `<script> <args>`
#   (a plain test: its exit code is its result). In args, @SOURCE_DIR@, @BINARY_DIR@ and @SANDBOX@ expand to the
#   FunkGui source directory, the build directory and the test's sandbox directory.
# - gpu=1 tests exist only with FUNKGUI_WITH_BGFX. labels=live marks a test that needs a window server: it is never in
#   `verify`. Other labels are added to the standard ones.
# A file under test/ without a FUNKGUI_TEST first line is not a test (gallery sections, helpers); a FUNKGUI_TEST line
# anywhere but the first line, or a malformed one, is a configure error, as is a duplicate name.
#
# Harness flags (03 §3.2.2): --golden-root <src>/test/golden --arch ${FUNKGUI_RUN_ARCH} --bless-to
# <build>/golden-candidates --results <build>/probe-results. The probe name is the test name, so goldens live at
# test/golden/{base,arm64,x86_64}/global/fg.<x>.txt (03 §3.3; FunkGui tests are global-scope probes).
#
# Labels: verify, fg, plus gpu (needs FUNKGUI_WITH_BGFX), or live (not in verify). A harness test passes CTest when its
# own RESULT line reports pass, golden_drift or golden_missing: drift and missing goldens are candidates, not failures
# (03 §3.2.4), and tools/verify.sh reports them from probe-results/*.json. spec_fail, harness_error, a crash or a
# timeout fail the test. Every test runs in <build>/sandbox/<name> with FUNKGUI_PREFS_DIR pointing inside it, so no
# test touches the real ~/Library/Application Support (C §6.6).

set(FUNKGUI_GOLDEN_ROOT ${PROJECT_SOURCE_DIR}/test/golden)
set(_fg_results_dir ${CMAKE_BINARY_DIR}/probe-results)
set(_fg_candidates_dir ${CMAKE_BINARY_DIR}/golden-candidates)

# ---- The architecture the tests run as (selects the golden overlay, 03 §3.3) -----------------------------------------
list(LENGTH CMAKE_OSX_ARCHITECTURES _fg_arch_count)
if(_fg_arch_count EQUAL 1)
  set(FUNKGUI_RUN_ARCH ${CMAKE_OSX_ARCHITECTURES})
elseif(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
  set(FUNKGUI_RUN_ARCH arm64)
else()
  set(FUNKGUI_RUN_ARCH x86_64)
endif()
if(NOT FUNKGUI_RUN_ARCH MATCHES "^(arm64|x86_64)$")
  message(FATAL_ERROR "FunkGui tests: unsupported architecture '${FUNKGUI_RUN_ARCH}' (arm64 or x86_64)")
endif()
if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
  set(_fg_host_arch arm64)
else()
  set(_fg_host_arch x86_64)
endif()
set(_fg_can_run ON)
if(NOT FUNKGUI_RUN_ARCH STREQUAL _fg_host_arch)     # e.g. x86_64 on Apple silicon: needs Rosetta 2
  try_run(_fg_run_rc _fg_compile_ok SOURCE_FROM_CONTENT fg_can_run.c "int main(void) { return 0; }"
          CMAKE_FLAGS -DCMAKE_OSX_ARCHITECTURES=${FUNKGUI_RUN_ARCH})
  if(NOT _fg_compile_ok OR NOT _fg_run_rc EQUAL 0)
    set(_fg_can_run OFF)
    message(WARNING "FunkGui tests: this host cannot run ${FUNKGUI_RUN_ARCH} executables; every test is DISABLED "
                    "(Rosetta 2: softwareupdate --install-rosetta --agree-to-license)")
  endif()
endif()

add_custom_target(funkgui_tests)
if(TARGET FunkGuiFonts)
  add_dependencies(funkgui_tests FunkGuiFonts)        # FunkGuiFonts.h is generated at build time; fg.headers reads it
endif()
if(TARGET FunkGuiShaders)
  add_dependencies(funkgui_tests FunkGuiShaders)
endif()

# ---- Registration ----------------------------------------------------------------------------------------------------
function(_fg_add_test name timeout gpu links exe args labels source)
  get_property(_known GLOBAL PROPERTY FUNKGUI_TEST_NAMES)
  if(name IN_LIST _known)
    message(FATAL_ERROR "${source}: duplicate FunkGui test name ${name}")
  endif()
  set_property(GLOBAL APPEND PROPERTY FUNKGUI_TEST_NAMES ${name})

  if(gpu AND NOT FUNKGUI_WITH_BGFX)
    message(STATUS "FunkGui tests: ${name} skipped (gpu=1, FUNKGUI_WITH_BGFX is OFF)")
    return()
  endif()
  if(FUNKGUI_HARNESS_ONLY AND (exe OR NOT links STREQUAL "harness"))
    message(STATUS "FunkGui tests: ${name} skipped (FUNKGUI_HARNESS_ONLY)")
    return()
  endif()
  if(links STREQUAL "presets" AND NOT TARGET FunkPresets)
    message(STATUS "FunkGui tests: ${name} skipped (FUNKGUI_WITH_PRESETS is OFF)")
    return()
  endif()

  set(_sandbox ${CMAKE_BINARY_DIR}/sandbox/${name})
  file(MAKE_DIRECTORY ${_sandbox})
  set(_harness_flags --golden-root ${FUNKGUI_GOLDEN_ROOT} --arch ${FUNKGUI_RUN_ARCH}
                     --bless-to ${_fg_candidates_dir} --results ${_fg_results_dir})
  string(REPLACE "@SOURCE_DIR@" "${PROJECT_SOURCE_DIR}" args "${args}")
  string(REPLACE "@BINARY_DIR@" "${CMAKE_BINARY_DIR}" args "${args}")
  string(REPLACE "@SANDBOX@" "${_sandbox}" args "${args}")
  separate_arguments(_args UNIX_COMMAND "${args}")

  set(_harness ON)
  if(exe)
    if(TARGET ${exe})
      set(_command $<TARGET_FILE:${exe}> ${name} ${_args} ${_harness_flags})
      add_dependencies(funkgui_tests ${exe})
    elseif(EXISTS "${PROJECT_SOURCE_DIR}/${exe}" AND NOT IS_DIRECTORY "${PROJECT_SOURCE_DIR}/${exe}")
      set(_command ${PROJECT_SOURCE_DIR}/${exe} ${_args})
      set(_harness OFF)
    else()
      message(FATAL_ERROR "${source}: exe=${exe} is neither a target nor a file in ${PROJECT_SOURCE_DIR}")
    endif()
  else()
    string(REGEX REPLACE "[^A-Za-z0-9]" "_" _target "${name}")
    if(links STREQUAL "harness")
      add_executable(${_target} ${source})
      target_link_libraries(${_target} PRIVATE FunkGui::harness)
    else()
      juce_add_console_app(${_target} PRODUCT_NAME ${_target})
      target_sources(${_target} PRIVATE ${source})
      target_link_libraries(${_target} PRIVATE FunkGui::core FunkGui::harness)
      if(links STREQUAL "gpu")
        target_link_libraries(${_target} PRIVATE FunkGui::gpu)
      elseif(links STREQUAL "presets")
        target_link_libraries(${_target} PRIVATE FunkGui::presets)
      endif()
      funkgui_configure_product(${_target} PRODUCT FunkGui OBJC_PREFIX FunkGui ENV_PREFIX FUNKGUI_
                                PREFS_FOLDER FunkGui)
    endif()
    target_include_directories(${_target} PRIVATE ${FUNKGUI_GENERATED_DIR})
    if(gpu)
      funkgui_compile_shaders(${_target})
    endif()
    set_target_properties(${_target} PROPERTIES EXCLUDE_FROM_ALL TRUE)
    _funkgui_internal_target(${_target} ${source})
    add_dependencies(funkgui_tests ${_target})
    set(_command $<TARGET_FILE:${_target}> ${name} ${_harness_flags})
  endif()

  add_test(NAME ${name} COMMAND ${_command} WORKING_DIRECTORY ${_sandbox})
  set(_labels verify fg)
  if(gpu)
    list(APPEND _labels gpu)
  endif()
  if(labels)
    string(REPLACE "," ";" labels "${labels}")
    list(APPEND _labels ${labels})
    if("live" IN_LIST labels)
      list(REMOVE_ITEM _labels verify)                # needs a window server: never in verify
    endif()
  endif()
  list(REMOVE_DUPLICATES _labels)
  set_tests_properties(${name} PROPERTIES
      LABELS "${_labels}"
      TIMEOUT ${timeout}
      ENVIRONMENT "FUNKGUI_PREFS_DIR=${_sandbox}/prefs")
  if(_harness)
    # The test's own RESULT line (03 §3.2.4); anchored on the probe name so a probe that runs inner probes (the
    # harness self-test) is judged by its own verdict.
    string(REPLACE "." "\\." _name_re "${name}")
    set_tests_properties(${name} PROPERTIES PASS_REGULAR_EXPRESSION
        "RESULT {\"probe\":\"${_name_re}\",\"mode\":\"[^\"]*\",\"arch\":\"[^\"]*\",\"status\":\"(pass|golden_drift|golden_missing)\"")
  endif()
  if(NOT _fg_can_run)
    set_tests_properties(${name} PROPERTIES DISABLED TRUE)
  endif()
endfunction()

file(GLOB_RECURSE _fg_test_files CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/*.mm)
list(SORT _fg_test_files)
foreach(_f IN LISTS _fg_test_files)
  file(READ ${_f} _head LIMIT 1024)
  string(FIND "${_head}" "\n" _nl)
  if(_nl GREATER_EQUAL 0)
    string(SUBSTRING "${_head}" 0 ${_nl} _first)
  else()
    set(_first "${_head}")
  endif()
  string(REGEX REPLACE "\r$" "" _first "${_first}")
  if(NOT _first MATCHES "^// FUNKGUI_TEST ")
    file(STRINGS ${_f} _misplaced REGEX "^// FUNKGUI_TEST ")
    if(_misplaced)
      message(FATAL_ERROR "${_f}: the FUNKGUI_TEST line must be the file's first line")
    endif()
    continue()                                          # not a test: gallery sections, helpers
  endif()
  if(NOT _first MATCHES "^// FUNKGUI_TEST name=(fg\\.[a-z0-9][a-z0-9._-]*) timeout=([1-9][0-9]*) gpu=([01])(.*)$")
    message(FATAL_ERROR "${_f}: malformed FUNKGUI_TEST line: ${_first}")
  endif()
  set(_name ${CMAKE_MATCH_1})
  set(_timeout ${CMAKE_MATCH_2})
  set(_gpu ${CMAKE_MATCH_3})
  set(_rest "${CMAKE_MATCH_4}")
  set(_links "")
  set(_exe "")
  set(_args "")
  set(_labels "")
  if(_rest MATCHES "^ links=(harness|core|gpu|presets)(.*)$")
    set(_links ${CMAKE_MATCH_1})
    set(_rest "${CMAKE_MATCH_2}")
  endif()
  if(_rest MATCHES "^ exe=([A-Za-z0-9_./+-]+)(.*)$")
    set(_exe ${CMAKE_MATCH_1})
    set(_rest "${CMAKE_MATCH_2}")
    if(_rest MATCHES "^ args=\"([^\"]*)\"(.*)$")
      set(_args "${CMAKE_MATCH_1}")
      set(_rest "${CMAKE_MATCH_2}")
    endif()
  endif()
  if(_rest MATCHES "^ labels=([a-z0-9_:-]+(,[a-z0-9_:-]+)*)(.*)$")
    set(_labels ${CMAKE_MATCH_1})
    set(_rest "${CMAKE_MATCH_3}")
  endif()
  if(NOT _rest STREQUAL "")
    message(FATAL_ERROR "${_f}: malformed FUNKGUI_TEST line (unexpected '${_rest}'): ${_first}")
  endif()
  if(_exe AND _links)
    message(FATAL_ERROR "${_f}: links= applies to compiled tests, not to exe=${_exe}")
  endif()
  if(NOT _links)
    if(_gpu)
      set(_links gpu)
    else()
      set(_links core)
    endif()
  endif()
  if(_links STREQUAL "gpu" AND NOT _gpu)
    message(FATAL_ERROR "${_f}: links=gpu needs gpu=1")
  endif()
  _fg_add_test(${_name} ${_timeout} ${_gpu} "${_links}" "${_exe}" "${_args}" "${_labels}" ${_f})
endforeach()

# ---- fg.headers: every public header compiles standalone (tools/check-headers.sh; gpu/ only with bgfx) ---------------
# The compile flags are a real consumer's: those of an OBJECT target that links FunkGui::core (+ ::gpu) and ::harness
# and calls funkgui_configure_product (it is never built). The script reads them from fg-headers.flags.
if(NOT FUNKGUI_HARNESS_ONLY)
  set_property(GLOBAL APPEND PROPERTY FUNKGUI_TEST_NAMES fg.headers)
  add_library(fg_header_flags OBJECT EXCLUDE_FROM_ALL)
  target_link_libraries(fg_header_flags PRIVATE FunkGui::core FunkGui::harness)
  set(_fg_headers_args --cxx ${CMAKE_CXX_COMPILER} --flags ${CMAKE_BINARY_DIR}/fg-headers.flags
                       --root ${PROJECT_SOURCE_DIR})
  if(CMAKE_OSX_SYSROOT)
    list(APPEND _fg_headers_args --sdk ${CMAKE_OSX_SYSROOT})
  endif()
  if(CMAKE_OSX_DEPLOYMENT_TARGET)
    list(APPEND _fg_headers_args --min-macos ${CMAKE_OSX_DEPLOYMENT_TARGET})
  endif()
  if(FUNKGUI_WITH_BGFX)
    target_link_libraries(fg_header_flags PRIVATE FunkGui::gpu)
    list(APPEND _fg_headers_args --bgfx)
  endif()
  target_include_directories(fg_header_flags PRIVATE ${FUNKGUI_GENERATED_DIR})
  funkgui_configure_product(fg_header_flags PRODUCT FunkGui OBJC_PREFIX FunkGui ENV_PREFIX FUNKGUI_
                            PREFS_FOLDER FunkGui)
  _funkgui_internal_target(fg_header_flags)
  file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/fg-headers.flags
       CONTENT "I $<JOIN:$<TARGET_PROPERTY:fg_header_flags,INCLUDE_DIRECTORIES>,\nI >\nD $<JOIN:$<TARGET_PROPERTY:fg_header_flags,COMPILE_DEFINITIONS>,\nD >\n")
  add_test(NAME fg.headers COMMAND ${PROJECT_SOURCE_DIR}/tools/check-headers.sh ${_fg_headers_args}
           WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(fg.headers PROPERTIES LABELS "verify;fg;lint" TIMEOUT 1800)
  if(NOT _fg_can_run)
    set_tests_properties(fg.headers PROPERTIES DISABLED TRUE)
  endif()
endif()

get_property(_fg_names GLOBAL PROPERTY FUNKGUI_TEST_NAMES)
list(LENGTH _fg_names _fg_count)
message(STATUS "FunkGui tests (${_fg_count} declared, run as ${FUNKGUI_RUN_ARCH}): ${_fg_names}")

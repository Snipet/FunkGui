# tools/GalleryApp: FunkGui's gallery Standalone and its live-parity test (G7; FCompressor docs/design/
# 02-funkgui-and-ui.md §1.1, §3.9-§3.10, §5.1; SPRINTS.md S8.3). Added by cmake/FunkGuiTargets.cmake's tools/*/
# CMakeLists.txt glob whenever FUNKGUI_BUILD_TOOLS is ON, so a consumer (FCompressor sets it ON) gets these targets too:
# both are EXCLUDE_FROM_ALL and exist only with FUNKGUI_WITH_BGFX.
#
#   FunkGuiGalleryApp   a GUI app bundle: a no-op AudioProcessor whose EditorHost runs one GalleryPanel section live on
#                       the GPU (GalleryApp.cpp). The section is --section <name>, else FUNKGUI_GALLERY_SECTION, else
#                       "primitives"; its Section menu switches. Product identity FunkGuiGallery / ENV_PREFIX FUNKGUI_.
#   FunkGuiGalleryLive  the parity driver (GalleryLive.cpp): per case, the section headless (HeadlessHost, dpi 2, theme 0)
#                       against a capture of the app by tools/capture-frame.sh; fingerprints, static primitives bit
#                       for bit, a11y lines and the dump's clock and overflow count must agree.
#   fg.gallery.live     the CTest test (labels fg;gpu;live: needs a window server and Metal, never in `verify`). It is
#                       registered here and not by a FUNKGUI_TEST line, because test/CMakeLists.txt only compiles
#                       test/** and the driver needs this directory's app; it follows test/CMakeLists.txt's conventions
#                       (sandbox, FUNKGUI_PREFS_DIR, harness flags, RESULT-line pass rule, duplicate-name check). Run it
#                       serialised: lockf -t 900 /tmp/fcmp-gui.lock ctest --test-dir <build> -L live
get_property(_fg_gallery_gpu GLOBAL PROPERTY FUNKGUI_GPU)
if(NOT _fg_gallery_gpu)
  message(STATUS "FunkGui: FunkGuiGalleryApp and fg.gallery.live need FUNKGUI_WITH_BGFX; not defined")
  return()
endif()

file(GLOB _fg_gallery_sections CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/test/gallery/*.cpp)

# ---- FunkGuiGalleryApp -----------------------------------------------------------------------------------------------
juce_add_gui_app(FunkGuiGalleryApp
    PRODUCT_NAME FunkGuiGalleryApp
    BUNDLE_ID    org.funkgui.galleryapp
    VERSION      ${PROJECT_VERSION})
set_target_properties(FunkGuiGalleryApp PROPERTIES EXCLUDE_FROM_ALL TRUE)
target_sources(FunkGuiGalleryApp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/GalleryApp.cpp ${_fg_gallery_sections})
target_link_libraries(FunkGuiGalleryApp PRIVATE FunkGui::core FunkGui::gpu FunkGui::harness)
target_include_directories(FunkGuiGalleryApp PRIVATE ${FUNKGUI_GENERATED_DIR})
funkgui_configure_product(FunkGuiGalleryApp PRODUCT FunkGuiGallery OBJC_PREFIX FunkGuiGallery ENV_PREFIX FUNKGUI_
                          PREFS_FOLDER FunkGui)
funkgui_compile_shaders(FunkGuiGalleryApp)
funkgui_add_font(FunkGuiGalleryApp)                  # the OFL and bgfx licences as bundle resources (02 §1.6)
_funkgui_internal_target(FunkGuiGalleryApp ${CMAKE_CURRENT_SOURCE_DIR}/GalleryApp.cpp ${_fg_gallery_sections})
set_property(GLOBAL APPEND PROPERTY FUNKGUI_TOOL_TARGETS FunkGuiGalleryApp)   # built by funkgui_tools

# ---- FunkGuiGalleryLive (headless side: FunkGui::core only) ---------------------------------------------------------
funkgui_add_tool(FunkGuiGalleryLive SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/GalleryLive.cpp ${_fg_gallery_sections})
add_dependencies(FunkGuiGalleryLive FunkGuiGalleryApp)

# ---- fg.gallery.live -------------------------------------------------------------------------------------------------
if(FUNKGUI_BUILD_TESTS)
  # This directory is added before the top-level enable_testing() (cmake/FunkGuiTargets.cmake runs first), so it turns
  # testing on for itself; CTest then reaches it through the top level's subdirs().
  enable_testing()

  set(_fg_live_name fg.gallery.live)
  get_property(_fg_live_known GLOBAL PROPERTY FUNKGUI_TEST_NAMES)
  if(_fg_live_name IN_LIST _fg_live_known)
    message(FATAL_ERROR "tools/GalleryApp: duplicate FunkGui test name ${_fg_live_name}")
  endif()
  set_property(GLOBAL APPEND PROPERTY FUNKGUI_TEST_NAMES ${_fg_live_name})

  # The architecture the harness runs as (test/CMakeLists.txt's FUNKGUI_RUN_ARCH rule; that file runs later).
  list(LENGTH CMAKE_OSX_ARCHITECTURES _fg_live_arch_count)
  if(_fg_live_arch_count EQUAL 1)
    set(_fg_live_arch ${CMAKE_OSX_ARCHITECTURES})
  elseif(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    set(_fg_live_arch arm64)
  else()
    set(_fg_live_arch x86_64)
  endif()

  set(_fg_live_sandbox ${CMAKE_BINARY_DIR}/sandbox/${_fg_live_name})
  file(MAKE_DIRECTORY ${_fg_live_sandbox})
  add_test(NAME ${_fg_live_name}
           COMMAND $<TARGET_FILE:FunkGuiGalleryLive> ${_fg_live_name}
                   --app $<TARGET_FILE:FunkGuiGalleryApp>
                   --capture ${PROJECT_SOURCE_DIR}/tools/capture-frame.sh
                   --out-dir ${_fg_live_sandbox}/frames
                   --golden-root ${PROJECT_SOURCE_DIR}/test/golden --arch ${_fg_live_arch}
                   --bless-to ${CMAKE_BINARY_DIR}/golden-candidates --results ${CMAKE_BINARY_DIR}/probe-results
           WORKING_DIRECTORY ${_fg_live_sandbox})
  set_tests_properties(${_fg_live_name} PROPERTIES
      LABELS "fg;gpu;live"
      TIMEOUT 900
      ENVIRONMENT "FUNKGUI_PREFS_DIR=${_fg_live_sandbox}/prefs"
      PASS_REGULAR_EXPRESSION
          "RESULT {\"probe\":\"fg\\.gallery\\.live\",\"mode\":\"[^\"]*\",\"arch\":\"[^\"]*\",\"status\":\"(pass|golden_drift|golden_missing)\"")
endif()

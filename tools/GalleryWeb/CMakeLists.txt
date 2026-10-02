# tools/GalleryWeb: FunkGui's gallery as a web page (v0.13.0; FCompressor ADR-93, docs/sprints/web-c.md G-D): the
# browser's counterpart of tools/GalleryApp. Added by cmake/FunkGuiTargets.cmake's tools/*/CMakeLists.txt glob whenever
# FUNKGUI_BUILD_TOOLS is ON; the target exists only where FunkGui::web does (Emscripten: FunkGui's `web` preset).
#
#   funkgui_gallery_web   GalleryWeb.cpp and every test/gallery/*.cpp over FunkGui::web and FunkGui::core, linked for a
#                         browser: this directory's build tree then holds index.html, funkgui_gallery_web.js and
#                         funkgui_gallery_web.wasm. One GalleryPanel section runs live in a WebHost on the page's
#                         canvas; the page's links reload it on another section. EXCLUDE_FROM_ALL, built by
#                         `funkgui_tools` (so the web preset builds it). To look at it:
#                           python3 -m http.server 8138 --bind 127.0.0.1 --directory <build-web>/tools/GalleryWeb
#                           http://127.0.0.1:8138/index.html?section=ruleslider
if(NOT TARGET FunkGuiWeb)
  return()
endif()

file(GLOB _fg_gallery_web_sections CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/test/gallery/*.cpp)
set(_fg_gallery_web_sources ${CMAKE_CURRENT_SOURCE_DIR}/GalleryWeb.cpp ${_fg_gallery_web_sections})
add_executable(funkgui_gallery_web EXCLUDE_FROM_ALL ${_fg_gallery_web_sources})
# A browser's executable: no node file system, and a runtime that outlives main(): the host runs from the page's events.
set_target_properties(funkgui_gallery_web PROPERTIES FUNKGUI_BROWSER ON)
target_link_libraries(funkgui_gallery_web PRIVATE FunkGui::core FunkGui::web)
target_link_options(funkgui_gallery_web PRIVATE -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH=1)
funkgui_configure_product(funkgui_gallery_web PRODUCT FunkGuiGallery OBJC_PREFIX FunkGuiGallery ENV_PREFIX FUNKGUI_
                          PREFS_FOLDER FunkGui)
_funkgui_internal_target(funkgui_gallery_web ${_fg_gallery_web_sources})
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/index.html ${CMAKE_CURRENT_BINARY_DIR}/index.html COPYONLY)
set_property(GLOBAL APPEND PROPERTY FUNKGUI_TOOL_TARGETS funkgui_gallery_web)   # built by funkgui_tools

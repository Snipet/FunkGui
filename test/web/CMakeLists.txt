# test/web: the browser check of FunkGui::web (v0.12.0; FCompressor ADR-93, docs/sprints/web-b.md G-C). Added by
# test/CMakeLists.txt only where FunkGui::web exists (Emscripten: the `web` preset).
#
#   funkgui_web_page   page.cpp over FunkGui::web and FunkGui::core, linked for a browser: this directory's build tree
#                      then holds index.html, funkgui_web_page.js and funkgui_web_page.wasm, which any static server on
#                      127.0.0.1 serves. Built by `funkgui_tests` (so the web preset's build proves the sink and the
#                      page compile and link warning-free), run by nothing under `verify`: node has no WebGL.
#   fg.web.page        the CTest test that runs the page in headless Chrome (tools/web/check-page.mjs). Labels fg;live,
#                      like fg.gallery.live: it needs a browser with a GPU path, so it is never in `verify`.
#                      Registered here and not by a FUNKGUI_TEST line, which would register it in every configuration.
#                        ctest --test-dir <build-web> -L live --output-on-failure
set(_fg_page_sources ${CMAKE_CURRENT_SOURCE_DIR}/page.cpp)
add_executable(funkgui_web_page EXCLUDE_FROM_ALL ${_fg_page_sources})
# A browser's executable: no node file system, and a runtime that outlives main() (the page goes on from timers).
set_target_properties(funkgui_web_page PROPERTIES FUNKGUI_BROWSER ON)
target_link_libraries(funkgui_web_page PRIVATE FunkGui::core FunkGui::web)
target_link_options(funkgui_web_page PRIVATE -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH=1)
funkgui_configure_product(funkgui_web_page PRODUCT FunkGui OBJC_PREFIX FunkGui ENV_PREFIX FUNKGUI_ PREFS_FOLDER FunkGui)
_funkgui_internal_target(funkgui_web_page ${_fg_page_sources})
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/index.html ${CMAKE_CURRENT_BINARY_DIR}/index.html COPYONLY)
add_dependencies(funkgui_tests funkgui_web_page)

get_property(_fg_known GLOBAL PROPERTY FUNKGUI_TEST_NAMES)
if("fg.web.page" IN_LIST _fg_known)
  message(FATAL_ERROR "${CMAKE_CURRENT_LIST_FILE}: duplicate FunkGui test name fg.web.page")
endif()
set_property(GLOBAL APPEND PROPERTY FUNKGUI_TEST_NAMES fg.web.page)
add_test(NAME fg.web.page
         COMMAND ${PROJECT_SOURCE_DIR}/tools/web/check-page.mjs ${CMAKE_CURRENT_BINARY_DIR}
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(fg.web.page PROPERTIES LABELS "fg;live" TIMEOUT 180)

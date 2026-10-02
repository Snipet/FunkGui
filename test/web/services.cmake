# test/web/services.cmake: the browser check of FunkGui::web's services (v0.13.0; FCompressor ADR-93,
# docs/sprints/web-c.md G-E): the DOM menu and the clipboard (src/web/WebServices.h) and the localStorage preferences
# (web/WebPrefs.h). Included by test/web/CMakeLists.txt, so only where FunkGui::web exists (Emscripten: the `web`
# preset):
#
#   include(${CMAKE_CURRENT_SOURCE_DIR}/services.cmake)
#
#   funkgui_web_services   services.cpp over FunkGui::web and FunkGui::core, linked for a browser like funkgui_web_page:
#                          the build directory of the including CMakeLists.txt then holds services.html,
#                          funkgui_web_services.js and funkgui_web_services.wasm. Built by `funkgui_tests`, run by
#                          nothing under `verify`: node has no document.
#   fg.web.services        the CTest test that runs the page in headless Chrome (tools/web/check-page.mjs --page
#                          services). Labels fg;live, like fg.web.page. It needs no GPU: the page draws nothing.
#                            ctest --test-dir <build-web> -L live -R fg.web.services --output-on-failure
#
# Every path is this file's own directory or the including directory's build directory, so the file can also be
# included from the top-level directory of a `web` build tree that test/web/CMakeLists.txt does not name it in yet.
if(TARGET funkgui_web_services)
  return()                                            # named twice (an include line and a glob of this directory)
endif()
set(_fg_services_sources ${CMAKE_CURRENT_LIST_DIR}/services.cpp)
add_executable(funkgui_web_services EXCLUDE_FROM_ALL ${_fg_services_sources})
set_target_properties(funkgui_web_services PROPERTIES FUNKGUI_BROWSER ON)
target_link_libraries(funkgui_web_services PRIVATE FunkGui::core FunkGui::web)
target_link_options(funkgui_web_services PRIVATE -sENVIRONMENT=web -sALLOW_MEMORY_GROWTH=1)
funkgui_configure_product(funkgui_web_services PRODUCT FunkGui OBJC_PREFIX FunkGui ENV_PREFIX FUNKGUI_
                          PREFS_FOLDER FunkGui)
_funkgui_internal_target(funkgui_web_services ${_fg_services_sources})
configure_file(${CMAKE_CURRENT_LIST_DIR}/services.html ${CMAKE_CURRENT_BINARY_DIR}/services.html COPYONLY)
add_dependencies(funkgui_tests funkgui_web_services)

get_property(_fg_known GLOBAL PROPERTY FUNKGUI_TEST_NAMES)
if("fg.web.services" IN_LIST _fg_known)
  message(FATAL_ERROR "${CMAKE_CURRENT_LIST_FILE}: duplicate FunkGui test name fg.web.services")
endif()
set_property(GLOBAL APPEND PROPERTY FUNKGUI_TEST_NAMES fg.web.services)
add_test(NAME fg.web.services
         COMMAND ${PROJECT_SOURCE_DIR}/tools/web/check-page.mjs ${CMAKE_CURRENT_BINARY_DIR} --page services
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(fg.web.services PROPERTIES LABELS "fg;live" TIMEOUT 180)

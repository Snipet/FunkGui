# tools/GalleryDesktop: FunkGui's gallery in a desktop window with no JUCE (v0.28.0): the counterpart of
# tools/GalleryApp (JUCE) and tools/GalleryWeb (a browser). Added by cmake/FunkGuiTargets.cmake's tools/*/CMakeLists.txt
# glob whenever FUNKGUI_BUILD_TOOLS is ON; the target exists only where FunkGui::desktop does (FUNKGUI_WITH_DESKTOP:
# FunkGui's `desktop` preset).
#
#   funkgui_gallery_desktop   GalleryDesktop.cpp and every test/gallery/*.cpp over FunkGui::desktop: one GalleryPanel
#                             section live in a DesktopHost's window (Metal, Direct3D 11 or Vulkan). A plain
#                             executable on every platform (on macOS too: no bundle), with the licences of what it
#                             holds in licences/ beside it. EXCLUDE_FROM_ALL, built by `funkgui_tools`.
#                               funkgui_gallery_desktop [--section <name>] [--theme <0|1>] [--frames <n>]
#                                                       [--shot <file.png>] [--info] [--list]
if(NOT TARGET FunkGuiDesktop)
  return()
endif()

file(GLOB _fg_gallery_desktop_sections CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/test/gallery/*.cpp)
set(_fg_gallery_desktop_sources ${CMAKE_CURRENT_SOURCE_DIR}/GalleryDesktop.cpp ${_fg_gallery_desktop_sections})
add_executable(funkgui_gallery_desktop EXCLUDE_FROM_ALL ${_fg_gallery_desktop_sources})
target_link_libraries(funkgui_gallery_desktop PRIVATE FunkGui::core FunkGui::gpu FunkGui::desktop)
funkgui_configure_product(funkgui_gallery_desktop PRODUCT FunkGuiGallery OBJC_PREFIX FunkGuiGallery
                          ENV_PREFIX FUNKGUI_ PREFS_FOLDER FunkGui)
funkgui_compile_shaders(funkgui_gallery_desktop)
_funkgui_internal_target(funkgui_gallery_desktop ${_fg_gallery_desktop_sources})
set_property(GLOBAL APPEND PROPERTY FUNKGUI_TOOL_TARGETS funkgui_gallery_desktop)   # built by funkgui_tools

# The licences of what the executable holds, beside it: the bundled face's, bgfx's, bx's, bimg's and bgfx.cmake's
# (funkgui_add_font's files; it puts them in an app bundle on macOS, and this is none), and SDL's when FunkGui fetched it.
get_property(_fg_gallery_desktop_licences GLOBAL PROPERTY FUNKGUI_LICENCE_FILES)
list(APPEND _fg_gallery_desktop_licences ${PROJECT_SOURCE_DIR}/fonts/JetBrainsMono-LICENSE.txt)
set(_fg_gallery_desktop_copy
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:funkgui_gallery_desktop>/licences
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_fg_gallery_desktop_licences}
            $<TARGET_FILE_DIR:funkgui_gallery_desktop>/licences)
if(FUNKGUI_SDL_SOURCE_DIR AND EXISTS "${FUNKGUI_SDL_SOURCE_DIR}/LICENSE.txt")
  list(APPEND _fg_gallery_desktop_copy
       COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FUNKGUI_SDL_SOURCE_DIR}/LICENSE.txt
               $<TARGET_FILE_DIR:funkgui_gallery_desktop>/licences/SDL-LICENSE.txt)
endif()
add_custom_command(TARGET funkgui_gallery_desktop POST_BUILD ${_fg_gallery_desktop_copy} VERBATIM)

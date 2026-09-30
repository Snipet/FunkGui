# FunkGui platform support (v0.11.0: Linux beside macOS). Included by the top-level CMakeLists.txt for FunkGui's own
# builds; a consumer applies the same workaround in its own scope (FCompressor: cmake/FcmpPlatform.cmake), because
# JUCE's module sources compile inside the consumer's targets.
#
#   funkgui_juce_compile_workarounds()   compile options JUCE 8.0.4 needs on this compiler, for every target created
#                                        after the call in this directory and below; FUNKGUI_JUCE804_WORKAROUND (the
#                                        flags, empty when none) in the caller's scope, for tools/check-headers.sh
#
# JUCE 8.0.4 declares, in juce_audio_processors/processors/juce_AudioPluginInstance.h:173,
#
#   template <size_t numLayouts>
#   AudioPluginInstance (const short channelLayoutList[numLayouts][2]) : AudioProcessor (channelLayoutList) {}
#
# The parameter decays to the non-dependent `const short (*)[2]`, so a compiler may resolve the base constructor call
# where the template is defined, and AudioProcessor has no such constructor any more. Upstream Clang does (Clang 22 on
# Linux: "no matching constructor for initialization of 'AudioProcessor'" in every translation unit that includes
# juce_audio_processors.h); AppleClang and GCC do not. JUCE removed the constructor after 8.0.4, and the JUCE pin is not
# FunkGui's to move, so where the compiler rejects exactly that pattern, -fdelayed-template-parsing defers the body to
# an instantiation that never happens. Clang calls the flag deprecated in C++20, a driver warning that -Werror turns
# into an error, so -Wno-delayed-template-parsing-in-cxx20 goes with it. Everywhere else nothing is added.
include_guard(GLOBAL)
include(CheckCXXSourceCompiles)

function(funkgui_juce_compile_workarounds)
  set(FUNKGUI_JUCE804_WORKAROUND "" PARENT_SCOPE)
  set(_src [=[
#include <cstddef>
#include <initializer_list>
struct B { B(); B(const std::initializer_list<const short[2]>&); };
struct D : B { template <std::size_t n> D(const short l[n][2]) : B(l) {} };
int main() { return 0; }
]=])
  check_cxx_source_compiles("${_src}" FUNKGUI_CXX_ACCEPTS_JUCE804_LAYOUT_CTOR)
  if(FUNKGUI_CXX_ACCEPTS_JUCE804_LAYOUT_CTOR)
    return()
  endif()
  set(_flags -fdelayed-template-parsing -Wno-delayed-template-parsing-in-cxx20)
  list(JOIN _flags " " CMAKE_REQUIRED_FLAGS)
  set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -Werror")
  check_cxx_source_compiles("${_src}" FUNKGUI_CXX_DELAYED_TEMPLATES_ACCEPT_JUCE804)
  unset(CMAKE_REQUIRED_FLAGS)
  if(NOT FUNKGUI_CXX_DELAYED_TEMPLATES_ACCEPT_JUCE804)
    message(FATAL_ERROR "FunkGui: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} rejects JUCE 8.0.4's "
                        "AudioPluginInstance layout constructor, even with -fdelayed-template-parsing")
  endif()
  foreach(_f IN LISTS _flags)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:${_f}>)
  endforeach()
  set(FUNKGUI_JUCE804_WORKAROUND "${_flags}" PARENT_SCOPE)
  message(STATUS "FunkGui: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} rejects JUCE 8.0.4's "
                 "AudioPluginInstance layout constructor: -fdelayed-template-parsing")
endfunction()

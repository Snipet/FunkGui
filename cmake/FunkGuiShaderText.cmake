# FunkGui's shaders as GLSL ES 3.00 text (v0.12.0), for the WebGL2 sink (include/funkgui/web/WebGlSink.h). A script, run
# at build time by the FunkGuiShaderText target of cmake/FunkGuiTargets.cmake:
#
#   cmake -DFUNKGUI_SHADER_DIR=<FunkGui>/shaders -DFUNKGUI_SHADER_OUT=<file>.h -P cmake/FunkGuiShaderText.cmake
#
# shaders/vs_ui.sc and fs_ui.sc stay the one shader source: bgfx's shaderc compiles them for Metal and Vulkan, and this
# turns the same two files into the text a browser compiles. Plain CMake string work: no shaderc, no bgfx and no JUCE,
# so every host and every configuration generates the same bytes, and fg.shader.web pins them everywhere.
#
# The transformation (FCompressor docs/sprints/web/map-render.md: it compiled and linked in Chromium's WebGL2 and drew
# the same pixels as shaderc's own 300_es output):
# - the `$input` and `$output` lines and `#include <bgfx_shader.sh>` are removed (whole lines; the body is otherwise
#   byte for byte the .sc file, comments included, with CRLF read as LF);
# - a preamble goes in front: `#version 300 es`; highp for float, int and sampler2D, so no value is narrower than on
#   the native renderers (shaderc's own ES output marks colour temporaries lowp); the three bgfx_shader.sh macros the
#   sources use, defined as what they mean in ES 3.00 (vec2_splat, texture2DLod, SAMPLER2D); and the declarations the
#   directives stood for, with their types read from shaders/varying.def.sc: a vertex `$input` is an attribute
#   (`layout(location = <its position in the list>) in`), a vertex `$output` an `out`, a fragment `$input` an `in`;
# - the fragment stage declares `out vec4 fg_FragColor` and `gl_FragColor` is renamed to it.
# Anything else a browser could not compile is an error here, not there: another #include, another `$` directive, a
# name varying.def.sc does not declare, gl_FragData.
#
# The header holds the two texts as NUL-terminated char arrays, funkgui::shaders::vs_ui_es300 and fs_ui_es300 (raw
# string literals: the bytes between the delimiters are the text; sizeof - 1 is its length).
cmake_minimum_required(VERSION 3.30)

foreach(_v FUNKGUI_SHADER_DIR FUNKGUI_SHADER_OUT)
  if("${${_v}}" STREQUAL "")
    message(FATAL_ERROR "FunkGuiShaderText.cmake: -D${_v}=… is required")
  endif()
endforeach()

set(_fg_delim "FUNKGUI_GLSL")                        # the raw string delimiter: R"FUNKGUI_GLSL( … )FUNKGUI_GLSL"

# The texts hold semicolons and brackets, which a CMake list would split or swallow: each stays one quoted string, and
# only identifier lists (names, types) are ever lists.
file(READ "${FUNKGUI_SHADER_DIR}/varying.def.sc" _fg_varyings)
string(REPLACE "\r\n" "\n" _fg_varyings "${_fg_varyings}")
set(_fg_varyings "\n${_fg_varyings}")

# _fg_declarations(<out> <prefix> <numbered> <names>...): one "<prefix> <type> <name>;" line per name, the type from
# varying.def.sc ("<type> <name> : <SEMANTIC>;"); with <numbered>, "layout(location = <index>) " goes in front.
function(_fg_declarations out_var prefix numbered)
  set(_text "")
  set(_index 0)
  foreach(_name IN LISTS ARGN)
    if(NOT _fg_varyings MATCHES "\n[ \t]*([A-Za-z0-9_]+)[ \t]+${_name}[ \t]*:")
      message(FATAL_ERROR "FunkGuiShaderText.cmake: shaders/varying.def.sc does not declare '${_name}'")
    endif()
    if(numbered)
      string(APPEND _text "layout(location = ${_index}) ")
    endif()
    string(APPEND _text "${prefix} ${CMAKE_MATCH_1} ${_name};\n")
    math(EXPR _index "${_index} + 1")
  endforeach()
  set(${out_var} "${_text}" PARENT_SCOPE)
endfunction()

# _fg_es300(<out> <vertex|fragment> <name>): shaders/<name>.sc as GLSL ES 3.00.
function(_fg_es300 out_var stage name)
  file(READ "${FUNKGUI_SHADER_DIR}/${name}.sc" _body)
  string(REPLACE "\r\n" "\n" _body "${_body}")
  set(_body "\n${_body}")                            # every line, the first included, now follows a newline

  set(_inputs "")
  set(_outputs "")
  if(_body MATCHES "\n\\$input[ \t]+([^\n]*)")
    string(REGEX MATCHALL "[A-Za-z_][A-Za-z0-9_]*" _inputs "${CMAKE_MATCH_1}")
  endif()
  if(_body MATCHES "\n\\$output[ \t]+([^\n]*)")
    string(REGEX MATCHALL "[A-Za-z_][A-Za-z0-9_]*" _outputs "${CMAKE_MATCH_1}")
  endif()
  string(REGEX REPLACE "\n\\$(input|output)[ \t][^\n]*" "" _body "${_body}")
  string(REGEX REPLACE "\n#include[ \t]*<bgfx_shader\\.sh>[^\n]*" "" _body "${_body}")
  string(SUBSTRING "${_body}" 1 -1 _body)          # the newline put in front (or the one a removed first line left)

  set(_preamble "#version 300 es\n")
  string(APPEND _preamble "precision highp float;\nprecision highp int;\nprecision highp sampler2D;\n")
  string(APPEND _preamble "#define vec2_splat(x) vec2(x)\n")
  string(APPEND _preamble "#define texture2DLod textureLod\n")
  string(APPEND _preamble "#define SAMPLER2D(n, s) uniform sampler2D n\n")
  if(stage STREQUAL "vertex")
    if(NOT _inputs OR NOT _outputs)
      message(FATAL_ERROR "FunkGuiShaderText.cmake: shaders/${name}.sc needs an $input and an $output line")
    endif()
    _fg_declarations(_in "in" ON ${_inputs})
    _fg_declarations(_out "out" OFF ${_outputs})
    string(APPEND _preamble "${_in}${_out}")
  else()
    if(NOT _inputs OR _outputs)
      message(FATAL_ERROR "FunkGuiShaderText.cmake: shaders/${name}.sc needs an $input line and no $output line")
    endif()
    _fg_declarations(_in "in" OFF ${_inputs})
    string(APPEND _preamble "${_in}out vec4 fg_FragColor;\n")
    string(REPLACE "gl_FragColor" "fg_FragColor" _body "${_body}")
  endif()

  set(_text "${_preamble}${_body}")
  foreach(_bad "$" "#include" "gl_FragData" ")${_fg_delim}\"")
    string(FIND "${_text}" "${_bad}" _at)
    if(NOT _at EQUAL -1)
      message(FATAL_ERROR "FunkGuiShaderText.cmake: shaders/${name}.sc holds '${_bad}', which this script cannot turn "
                          "into GLSL ES 3.00")
    endif()
  endforeach()
  set(${out_var} "${_text}" PARENT_SCOPE)
endfunction()

_fg_es300(_fg_vs vertex vs_ui)
_fg_es300(_fg_fs fragment fs_ui)

set(_fg_h "#pragma once\n\n")
string(APPEND _fg_h "// Generated by FunkGui's cmake/FunkGuiShaderText.cmake from "
                    "shaders/{vs_ui,fs_ui,varying.def}.sc: do not edit.\n")
string(APPEND _fg_h "// The UI program as GLSL ES 3.00 text, for WebGL2 (funkgui/web/WebGlSink.h).\n\n")
string(APPEND _fg_h "namespace funkgui::shaders\n{\n")
string(APPEND _fg_h "    inline constexpr char vs_ui_es300[] = R\"${_fg_delim}(${_fg_vs})${_fg_delim}\";\n\n")
string(APPEND _fg_h "    inline constexpr char fs_ui_es300[] = R\"${_fg_delim}(${_fg_fs})${_fg_delim}\";\n")
string(APPEND _fg_h "}\n")

file(WRITE "${FUNKGUI_SHADER_OUT}" "${_fg_h}")

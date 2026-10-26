$input v_uv, v_px

// FunkGui custom fragment shader (gpu/ShaderLibrary.h). Write main() below and set gl_FragColor: straight
// (not premultiplied) RGBA, blended over what is under the rect as the UI's primitives are (src * a + dst * (1 - a)).
//   v_uv        vec2  0..1 across the rect: (0, 0) its top-left corner, (1, 1) its bottom-right (y down)
//   v_px        vec2  logical px from the rect's top-left corner: 0..u_rect.z, 0..u_rect.w
//   u_viewSize  vec4  xy the view's logical size, z one physical px in logical px, w seconds since the editor opened
//   u_rect      vec4  the rect: x, y, w, h in logical px
//   u_params    vec4[4]  the 16 params of Canvas::shader: param i is u_params[i / 4][i % 4] (unset ones are 0)
// No textures, and no other uniforms: FunkGui sets only these.

#include <bgfx_shader.sh>

uniform vec4 u_viewSize;
uniform vec4 u_rect;
uniform vec4 u_params[4];


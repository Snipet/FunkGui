$input a_position, a_texcoord0
$output v_uv, v_px

// The vertex shader of every custom program (v0.19.0; include/funkgui/gpu/ShaderLibrary.h): FunkGui's half of the
// program a host's fragment shader is linked into. It draws one shader draw's rect (Canvas::shader) as a quad, from
// the six vertices canvas/Expand.h's expandShader writes: a_position is a corner in logical px, as for the UI
// primitives (u_viewSize.xy is the logical view size), and a_texcoord0 = (u, v, px, py) at that corner. The fragment
// shader gets v_uv (0..1 across the rect, y down) and v_px (logical px from the rect's top-left corner), interpolated.
//
// Compiled with shaders/varying_custom.def.sc, the varyings a host's fragment shader is compiled with too
// (shaders::varyingDef()). bgfx links the two only when the fragment shader's $input names exactly this $output's set
// (shaderc hashes the sorted names): v_px and v_uv, whichever the fragment shader reads.

#include <bgfx_shader.sh>

uniform vec4 u_viewSize;

void main()
{
    vec2 ndc = vec2(a_position.x / u_viewSize.x * 2.0 - 1.0,
                    1.0 - a_position.y / u_viewSize.y * 2.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
    v_uv = a_texcoord0.xy;
    v_px = a_texcoord0.zw;
}

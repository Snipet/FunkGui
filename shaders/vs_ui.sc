$input a_position, a_color0, a_color1, a_texcoord0, a_texcoord1, a_texcoord2
$output v_color0, v_color1, v_data0, v_data1, v_data2

// Shared vertex shader for every UI primitive. Vertices arrive in logical
// pixels; u_viewSize.xy is the logical viewport size. The fragment shader
// receives per-primitive SDF parameters through v_data0..2 untouched and
// branches on the primitive kind in v_data2.z.

#include <bgfx_shader.sh>

uniform vec4 u_viewSize;

void main()
{
    vec2 ndc = vec2(a_position.x / u_viewSize.x * 2.0 - 1.0,
                    1.0 - a_position.y / u_viewSize.y * 2.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
    v_color0 = a_color0;
    v_color1 = a_color1;
    v_data0  = a_texcoord0;
    v_data1  = a_texcoord1;
    v_data2  = a_texcoord2;
}

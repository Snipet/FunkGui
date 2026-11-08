$input a_position, i_data0, i_data1, i_data2, i_data3, i_data4
$output v_color0, v_color1, v_data0, v_data1, v_data2

// The instanced vertex shader of the UI program (v0.26.0; canvas/Expand.h Inst, gpu/BgfxSink.h): every primitive is
// one instance of a unit quad, whose corners a_position holds ((0, 0), (1, 0), (1, 1), (0, 1): a, b, c, d, drawn as
// the triangles a, b, c and a, c, d, expand()'s), and the primitive itself is 80 bytes of instance data, i_data0..
// i_data4 (canvas/Expand.h Inst). From them this shader makes the vertex expand() writes for that corner, bit for bit,
// and outputs it as vs_ui.sc does, so fs_ui.sc (unchanged, linked with both) draws the same pixels:
// - the position and the local x, y are picked, never computed, from the record's corners;
// - the colours come from three packed fields, integers below 2^24, by steps that are all exact in binary floating
//   point (a product by a power of two, a floor, a difference of integers), so any reordering a compiler makes gives
//   the same bytes; each byte becomes the value in u_unorm8 (canvas/Expand.h unorm8Values: c / 255 rounded to the
//   nearest float, as the vertex fetch converts Color0 and Color1 for vs_ui.sc);
// - the kind is the third byte of the third field, every other value is passed on as it is.
// canvas/Expand.cpp instanceCorner() takes the same steps on the CPU (fg.canvas.instance); fg.gpu.instancing compares
// the pixels of both programs. Compiled with shaders/varying_inst.def.sc: the same varyings as vs_ui.sc's, so the two
// vertex shaders share fs_ui.sc.

#include <bgfx_shader.sh>

uniform vec4 u_viewSize;
uniform vec4 u_unorm8[64];

// The three bytes of a packed field, low first.
vec3 bytes3(float p)
{
    float hi  = floor(p * (1.0 / 65536.0));
    float lo  = p - hi * 65536.0;
    float mid = floor(lo * (1.0 / 256.0));
    return vec3(lo - mid * 256.0, mid, hi);
}

// A byte's normalised value: entry c of the table, four to a vec4.
float unorm8(float c)
{
    float q = floor(c * 0.25);
    float r = c - q * 4.0;
    vec4  t = u_unorm8[int(q)];
    return r < 0.5 ? t.x : (r < 1.5 ? t.y : (r < 2.5 ? t.z : t.w));
}

void main()
{
    bool right  = a_position.x > 0.5;
    bool bottom = a_position.y > 0.5;
    vec2 pos = vec2(right ? i_data0.z : i_data0.x, bottom ? i_data0.w : i_data0.y);

    // vs_ui.sc's transform, as it is written there.
    vec2 ndc = vec2(pos.x / u_viewSize.x * 2.0 - 1.0,
                    1.0 - pos.y / u_viewSize.y * 2.0);
    gl_Position = vec4(ndc, 0.0, 1.0);

    vec3 a = bytes3(i_data4.x);                  // c0's r, g, b
    vec3 b = bytes3(i_data4.y);                  // c0's a, c1's r, g
    vec3 c = bytes3(i_data4.z);                  // c1's b, a, the kind
    v_color0 = vec4(unorm8(a.x), unorm8(a.y), unorm8(a.z), unorm8(b.x));
    v_color1 = vec4(unorm8(b.y), unorm8(b.z), unorm8(c.x), unorm8(c.y));
    v_data0  = vec4(right ? i_data2.x : i_data1.x, bottom ? i_data2.y : i_data1.y, i_data1.z, i_data1.w);
    v_data1  = i_data3;
    v_data2  = vec4(i_data2.z, i_data2.w, c.z, i_data4.w);
}

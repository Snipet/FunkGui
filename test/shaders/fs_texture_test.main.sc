// The texture test program's main() (fg.shader.texture): built as fs_custom_test.main.sc is (shaders/
// fs_custom_prelude.sc in front of it, compiled at build time with a host's shaderc and varyings), with two samplers,
// at stages 0 and 2. u_params[0]: x what it reads, y the scale and zw the offset of the point it reads at
// (uv = v_uv * y + zw):
//   x 0   texture2D(s_tex, uv)
//   x 1   texture2D(s_mask, uv)
//   x 2   texelFetch(s_tex, the texel under v_px), level 0
// and that colour is the output, alpha included.

SAMPLER2D(s_tex, 0);
SAMPLER2D(s_mask, 2);

void main()
{
    vec2 uv = v_uv * u_params[0].y + u_params[0].zw;
    vec4 c = texture2D(s_tex, uv);
    if (u_params[0].x > 1.5)
        c = texelFetch(s_tex, ivec2(v_px), 0);
    else if (u_params[0].x > 0.5)
        c = texture2D(s_mask, uv);
    gl_FragColor = c;
}

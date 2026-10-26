// The test program's main() (fg.shader.library, fg.shader.draw): FunkGui's own build puts shaders/fs_custom_prelude.sc
// in front of it, as a host would, and compiles the whole with the shaderc and the varyings a host uses
// (cmake/FunkGuiTargets.cmake). Every pixel says where it is: r, g = v_uv (or v_px / the rect's size when
// u_params[1].x > 0.5, which must be the same), b = u_params[0].z, a = u_params[0].w.

void main()
{
    vec2 uv = u_params[1].x > 0.5 ? v_px / u_rect.zw : v_uv;
    gl_FragColor = vec4(uv.x, uv.y, u_params[0].z, u_params[0].w);
}

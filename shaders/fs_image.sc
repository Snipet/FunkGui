$input v_uv, v_px

// FunkGui's image fragment shader (v0.20.0; Canvas::image, canvas/PrimList.h ImageDraw has the rule). Linked with
// shaders/vs_custom.sc, the custom programs' vertex shader, over the six vertices canvas/Expand.h's expandImage
// writes: the quad is dst grown by kImageApron, v_px the logical px from dst's top-left corner (negative in the apron)
// and v_uv = v_px / dst's size (0..1 across dst). SoftRaster (src/canvas/SoftRaster.cpp) mirrors this line for line,
// and the two change together.
//
// The texture is the picture's premultiplied RGBA8 level chain (canvas/Images.h); one level per draw, chosen on the
// CPU (imageLevel) and read with an explicit LOD, so nothing here takes a derivative. The colour is unpremultiplied
// before it leaves, so the UI's blend (src * a + dst * (1 - a)) composites it exactly as a premultiplied "over".

#include <bgfx_shader.sh>

SAMPLER2D(s_image, 0);

uniform vec4 u_viewSize;     // as the UI's: z is one physical px in logical px (the antialiasing half-width)
uniform vec4 u_imageRect;    // dst: x, y, w, h in logical px
uniform vec4 u_imageUv;      // the picture's sub-rectangle stretched over dst: u0, v0, du, dv (0..1)
uniform vec4 u_imageClamp;   // the sample point's bounds: lo u, lo v, hi u, hi v (imageSampling)
uniform vec4 u_imageMask;    // x corner radius (logical px), y opacity, z the level, w unused

void main()
{
    // The mask: the rounded rectangle dst, with an rrect's signed distance and edge (fs_ui.sc), centre-relative.
    vec2  b = u_imageRect.zw * 0.5;
    vec2  p = v_px - b;
    float r = min(u_imageMask.x, min(b.x, b.y));
    vec2  q = abs(p) - b + vec2_splat(r);
    float d = length(max(q, vec2_splat(0.0))) + min(max(q.x, q.y), 0.0) - r;
    float aa = u_viewSize.z;
    float cover = (1.0 - smoothstep(-aa, aa, d)) * u_imageMask.y;

    // The picture: the point of uv that this place in dst maps to, kept inside the texels of uv.
    vec2 uv = clamp(u_imageUv.xy + v_uv * u_imageUv.zw, u_imageClamp.xy, u_imageClamp.zw);
    vec4 pm = texture2DLod(s_image, uv, u_imageMask.z);

    float a = clamp(pm.a * cover, 0.0, 1.0);
    vec3  rgb = min(pm.rgb / max(pm.a, 1e-5), vec3(1.0, 1.0, 1.0));
    gl_FragColor = vec4(rgb, a);
}

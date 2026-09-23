$input v_color0, v_color1, v_data0, v_data1, v_data2

// One fragment shader for every UI primitive, selected by v_data2.z.
//
// Why one program rather than three: the canvas used to submit all rounded
// rects, then all arcs, then all text, as three separate batches. Submission
// order was therefore meaningless and z-order impossible — no shape could sit
// over another kind, and no label could ever be covered. With a single program
// the whole frame is one draw call in exact submission order, so layering is
// just the order the caller draws in.
//
// Divergence costs nothing: all six vertices of a primitive carry the same
// kind, and a 2x2 fragment quad never spans two primitives, so control flow is
// uniform across every SIMD quad. No derivative is taken anywhere — the
// antialiasing half-width is the exact physical-pixel footprint passed in
// u_viewSize.z. That is well defined regardless of branching, and unlike
// fwidth() it cannot fatten a stroke where the field is steep.
//
// Local coordinates are logical pixels relative to the primitive's centre,
// with +y pointing DOWN.

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);

// xy = logical viewport size, z = one physical pixel expressed in logical
// pixels (the AA half-width), w = elapsed seconds.
uniform vec4 u_viewSize;

#define KIND_RRECT   0.0
#define KIND_TEXT    1.0
#define KIND_SEGMENT 2.0
#define KIND_AREA    3.0

void main()
{
    float aa = u_viewSize.z;
    vec4  col;

    // The kinds are classified by range, in order (02 §4.3): under the old
    // "> 1.5 is a segment" test a fourth kind would have drawn as a capsule.
    // The rrect, text and segment bodies are unchanged; SoftRaster
    // (src/canvas/SoftRaster.cpp) mirrors this chain and every branch, and
    // the two change together (A §2.6 #4).
    float kind = v_data2.z;
    if (kind < 0.5)
    {
        // ---- rounded rect: per-corner radii, inside border, optional glow --
        vec2 p = v_data0.xy;
        vec2 b = v_data0.zw;

        // v_data1 = (topLeft, topRight, bottomRight, bottomLeft)
        float rTop = (p.x > 0.0) ? v_data1.y : v_data1.x;
        float rBot = (p.x > 0.0) ? v_data1.z : v_data1.w;
        float r    = (p.y > 0.0) ? rBot : rTop;

        // Clamped: an unclamped radius larger than the half extent produces a
        // shape bigger than the rect, so the common "radius 9999 makes a pill"
        // idiom used to render malformed.
        r = min(r, min(b.x, b.y));

        vec2  q = abs(p) - b + vec2_splat(r);
        float d = length(max(q, vec2_splat(0.0))) + min(max(q.x, q.y), 0.0) - r;

        float bw   = v_data2.x;
        float soft = v_data2.y;
        float fw   = max(aa, soft);

        float fa = (1.0 - smoothstep(-fw, fw, d)) * v_color0.a;
        col = vec4(v_color0.rgb, fa);

        if (bw > 0.0)
        {
            // Border band occupies d in [-bw, 0].
            float bd = abs(d + bw * 0.5) - bw * 0.5;
            float ba = (1.0 - smoothstep(-aa, aa, bd)) * v_color1.a;

            // Proper source-over. Mixing rgb while taking max() of alpha
            // blended toward a fill colour that had no coverage, which fringed
            // every border drawn over a translucent panel.
            float outA = ba + fa * (1.0 - ba);
            vec3  rgb  = (v_color1.rgb * ba + v_color0.rgb * fa * (1.0 - ba))
                       / max(outA, 1e-5);
            col = vec4(rgb, outA);
        }
    }
    else if (kind < 1.5)
    {
        // ---- SDF text ------------------------------------------------------
        // Explicit LOD: the atlas has no mips, and an explicit level needs no
        // derivatives, so the fetch is safe inside a branch.
        float d = texture2DLod(s_texColor, v_data0.xy, 0.0).x;

        float weight = v_data1.x;   // threshold shift; >0 dilates toward semibold
        float taa    = v_data1.y;   // feather half-width, in field units
        float a = smoothstep(0.5 - weight - taa, 0.5 - weight + taa, d);

        // The backbuffer is not sRGB, so coverage composites in gamma-encoded
        // space. Light text on a dark ground therefore lands thinner and
        // dimmer than its true coverage — the usual reason custom GPU UIs look
        // worse than native ones at 10-13px. v_data1.z carries the exponent so
        // the light theme, which is dark-on-light, can correct the other way.
        col = vec4(v_color0.rgb, v_color0.a * pow(a, v_data1.z));
    }
    else if (kind < 2.5)
    {
        // ---- segment: a capsule from a to b, half-width v_data2.x ---------
        // The distance from p to the segment is the distance to its nearest
        // point, found by clamping the projection onto ab. Same feathering
        // as a rect, so a curve drawn as short segments joins seamlessly
        // where consecutive capsules overlap by a radius.
        vec2 p  = v_data0.xy;
        vec2 a  = v_data1.xy;
        vec2 ba = v_data1.zw - a;
        float t = clamp(dot(p - a, ba) / max(dot(ba, ba), 1e-6), 0.0, 1.0);
        float d = length(p - a - ba * t) - v_data2.x;
        float fw = max(aa, v_data2.y);
        col = vec4(v_color0.rgb, (1.0 - smoothstep(-fw, fw, d)) * v_color0.a);
    }
    else
    {
        // ---- area: one column between two straight edges (02 §4.1) --------
        // The quad is the column [x0, x1] with no x apron, so neighbouring
        // columns tile with no gap and no overlap. v_data1 holds the top
        // edge's y at the left and right (x, y) and the bottom edge's (z, w),
        // centre-relative. The fill (v_color0) is the span between the edges,
        // antialiased along each edge's normal; the stroke (v_color1, half
        // width v_data2.x) follows the top edge, the bottom one (flags bit 1)
        // or both (bit 2). Bit 0 (live) is the CPU's and changes nothing here.
        vec2  p  = v_data0.xy;
        float hw = max(v_data0.z, 1e-4);
        float t  = clamp(p.x / (2.0 * hw) + 0.5, 0.0, 1.0);
        float yT = mix(v_data1.x, v_data1.y, t);
        float yB = mix(v_data1.z, v_data1.w, t);
        float sT = (v_data1.y - v_data1.x) / (2.0 * hw);
        float sB = (v_data1.w - v_data1.z) / (2.0 * hw);
        float dT = (yT - p.y) * inversesqrt(1.0 + sT * sT);   // > 0 above the top edge
        float dB = (p.y - yB) * inversesqrt(1.0 + sB * sB);   // > 0 below the bottom edge
        float fa = (1.0 - smoothstep(-aa, aa, max(dT, dB))) * v_color0.a;
        float fl     = v_data2.w;
        float bottom = mod(floor(fl * 0.5  + 0.01), 2.0);     // flags bit 1
        float both   = mod(floor(fl * 0.25 + 0.01), 2.0);     // flags bit 2
        float dS = both > 0.5 ? min(abs(dT), abs(dB)) : (bottom > 0.5 ? abs(dB) : abs(dT));
        float hs = v_data2.x;
        float sa = hs > 0.0 ? (1.0 - smoothstep(hs - aa, hs + aa, dS)) * v_color1.a : 0.0;

        // Source-over of the stroke on the fill, as the rrect border.
        float oA = sa + fa * (1.0 - sa);
        col = vec4((v_color1.rgb * sa + v_color0.rgb * fa * (1.0 - sa)) / max(oA, 1e-5), oA);
    }

    gl_FragColor = col;
}

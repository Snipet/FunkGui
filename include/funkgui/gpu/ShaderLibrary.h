#pragma once

// Custom shaders (v0.19.0): a process-wide registry of programs a host builds from its own fragment shaders, compiled
// at run time by bgfx's shaderc, which BgfxSink draws wherever a Canvas recorded Canvas::shader(id, rect, params, n)
// (canvas/PrimList.h ShaderDraw), in order with the primitives. Each program is FunkGui's own vertex shader
// (shaders/vs_custom.sc, embedded for the renderer in use) linked with the host's fragment shader. Message thread only,
// like everything that touches bgfx; one registry for every editor in the process, keyed by the host's ids.
//
// ---- The shader a host writes ---------------------------------------------------------------------------------------
// A bgfx .sc fragment shader whose first line is `$input v_uv, v_px` (both names: bgfx links a program only when the
// fragment shader's $input set is the vertex shader's $output set; reading only one of them is fine), which includes
// <bgfx_shader.sh>, declares the uniforms it reads from the list below, and sets gl_FragColor. fragmentPrelude() is
// that head as text: a host writes fragmentPrelude() followed by a main() (and any helper functions) and compiles it.
//
//   Varyings (shaders/varying_custom.def.sc; varyingDef() is the file's text, passed to shaderc as --varyingdef):
//     vec2 v_uv : TEXCOORD0   0..1 across the shader draw's rect: (0, 0) at its top-left corner, (1, 1) at its
//                             bottom-right; y runs down, as everywhere in FunkGui
//     vec2 v_px : TEXCOORD1   logical px from the rect's top-left corner: 0..rect.w across, 0..rect.h down
//   Uniforms (all vec4; FunkGui sets them before each shader draw):
//     u_viewSize       xy the view's logical size (FrameInfo::logicalW, logicalH), z one physical px in logical px
//                      (1 / dpi: an antialiasing width), w FrameInfo::seconds (the editor's clock: animate with it;
//                      it moves only while frames are drawn, so a Panel whose shader animates keeps
//                      wantsFullRate() true while it should move)
//     u_rect           the shader draw's rect: x, y, w, h in logical px (Canvas::shader's r, before any clip)
//     u_params[4]      the 16 params of Canvas::shader: param i is u_params[i / 4][i % 4]; those not given are 0
//   Output: gl_FragColor, straight (not premultiplied) RGBA, blended over what is beneath as the primitives are
//   (BGFX_STATE_BLEND_ALPHA: rgb = src.rgb * a + dst.rgb * (1 - a)). No depth, no culling, no textures: set() refuses
//   a shader that samples a texture or reads a uniform neither FunkGui nor bgfx sets (bgfx_shader.sh's predefined
//   ones, u_viewRect and the rest, are bgfx's and allowed), since nothing would ever be bound or set for it.
//   The rect is drawn whole and the clip in force when it was recorded is a scissor, so v_uv never depends on a clip.
//
// ---- Compiling it (the host's side) ---------------------------------------------------------------------------------
// With the shaderc of the bgfx FunkGui is built against (bgfx.cmake v1.153.9385-561: the prebuilt
// ~/audio/.deps/tools/shaderc-v1.153.9385-561/shaderc), into a directory holding varying.def.sc = varyingDef() and
// bgfx_shader.sh = bgfxShaderSh() (or point -i at that bgfx's src/):
//
//   shaderc -f fs.sc -o fs.bin --type fragment --platform <target().platform> -p <target().profile>
//           --varyingdef varying.def.sc -i <dir>
//
// and hand fs.bin's bytes to set(). shaderc's errors (stderr, exit code) are the host's to show; set() adds what
// shaderc cannot know (the varyings, the uniforms, and on macOS whether Metal's own compiler takes the result).
//
// ---- Lifetime -------------------------------------------------------------------------------------------------------
// The registry keeps each id's binary. bgfx itself comes and goes with the editors (BgfxContext shuts it down when the
// last one closes, and rebuilds it when the primary one closes): its programs are destroyed with it, and each is built
// again from the kept binary the first time it is drawn after bgfx starts again, so a host sets an id once. set()
// while bgfx is not running checks everything it can without bgfx (all of the above, Metal's compile included) and
// stores the binary for the next start. Nothing is destroyed at process exit (bgfx is gone by then).
//
// On Linux (Vulkan): the same registry and the same checks, except that the code is checked only as SPIR-V words (the
// magic number and the size), not by the driver: bgfx creates the module at the next frame (the stated fallback).
// Without bgfx (FUNKGUI_WITH_BGFX=OFF, the web) there is no registry: shader draws are a placeholder in SoftRaster and
// nothing in WebGlSink (canvas/PrimList.h).

#include <cstdint>
#include <string>

namespace funkgui::shaders
{
    // What to pass shaderc's --platform and -p for the renderer bgfx runs on: {"osx", "metal"} on macOS (Metal),
    // {"linux", "spirv"} on Linux (Vulkan). Empty strings while bgfx is not initialised (no editor has attached yet,
    // or the last one closed): FunkGui's renderer is fixed per platform, but this answers for the one running.
    struct Target
    {
        std::string platform, profile;
    };
    Target target();

    // Registers (or replaces) id's program: FunkGui's vertex shader + this shaderc fragment-shader binary for target().
    // The bytes are copied; the caller's may go. Returns false, with why set to a reason a host can show (or hand back
    // to whoever wrote the shader: one sentence, followed by Metal's compile log when Metal refused the code), when the
    // binary is empty or not a bgfx shader, is not a fragment shader, is of a shader format this bgfx does not read,
    // does not take `$input v_uv, v_px` with varyingDef()'s varyings, samples a texture, reads a uniform neither
    // FunkGui nor bgfx sets (or one of FunkGui's with another type or size), carries code for another renderer, does
    // not compile (macOS: Metal's own compiler, as bgfx will call it), or bgfx refuses to create the shader or link the
    // program (only checked while bgfx runs). On false the id keeps what it had (its previous program, or nothing). On
    // true, why is cleared and the next frame draws the new program.
    bool set(uint32_t id, const void* fragmentBinary, uint32_t size, std::string& why);

    // Unregisters id and destroys its program; shader draws of it then draw nothing. An unknown id is ignored.
    void remove(uint32_t id);

    // Whether a program is registered under id (set() succeeded and remove() has not been called since).
    bool has(uint32_t id);

    // The text of shaders/varying_custom.def.sc, exactly: what a custom fragment shader is compiled with
    // (--varyingdef).
    const char* varyingDef();

    // The head of a custom fragment shader as text (shaders/fs_custom_prelude.sc): the `$input v_uv, v_px` line, a
    // comment listing the varyings and uniforms above, `#include <bgfx_shader.sh>` and the three uniform declarations.
    // A host writes it, then its main(), into the .sc file it compiles.
    const char* fragmentPrelude();

    // The pinned bgfx's src/bgfx_shader.sh, exactly: the header every .sc file includes, for a host that compiles where
    // bgfx's sources are not (write it beside the shader and pass that directory as -i).
    const char* bgfxShaderSh();
}

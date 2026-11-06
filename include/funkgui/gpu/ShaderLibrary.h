#pragma once

// Custom shaders (v0.19.0): a process-wide registry of programs a host builds from its own fragment shaders, compiled
// at run time by bgfx's shaderc, which BgfxSink draws wherever a Canvas recorded Canvas::shader(id, rect, params, n)
// (canvas/PrimList.h ShaderDraw), in order with the primitives. Each program is FunkGui's own vertex shader
// (shaders/vs_custom.sc, embedded for the renderer in use) linked with the host's fragment shader. Message thread only,
// like everything that touches bgfx, except check() (0.25.0), which any thread may call at any time; one registry for
// every editor in the process, keyed by the host's ids.
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
//   Samplers (0.25.0): up to four SAMPLER2D(name, stage), stage 0 to 3, each reading the texture a host bound to that
//   name for this id ("Textures" below).
//   Output: gl_FragColor, straight (not premultiplied) RGBA, blended over what is beneath as the primitives are
//   (BGFX_STATE_BLEND_ALPHA: rgb = src.rgb * a + dst.rgb * (1 - a)). No depth, no culling: set() refuses a shader that
//   reads a uniform neither FunkGui nor bgfx sets (bgfx_shader.sh's predefined ones, u_viewRect and the rest, are
//   bgfx's and allowed), or a texture other than a 2D sampler as above: nothing would ever be set or bound for it.
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
// ---- Checking it off the message thread (0.25.0) --------------------------------------------------------------------
// On macOS set() runs Metal's compiler on the code, milliseconds for a small shader and more for a large one, on the
// message thread. A host that builds its shaders on a worker calls check() there after shaderc, and set() on the
// message thread afterwards: check() and set() remember the codes Metal accepted (the exact text: the last 256 used, up
// to 2 MiB of text, the newest always kept), and neither compiles a remembered code again. Past those bounds the code
// used least lately goes, and set() of it then compiles it again, as without check(); a host that checks many codes
// long before it sets them can check() each again on a worker shortly before its set() (no compile while it is
// remembered). A refused code is not remembered: set() of it compiles it again and refuses it with check()'s reason.
// bgfx compiles each new shader's code itself too, when it creates it at the next frame; Metal keeps what it compiled
// in this process, so in practice that compile is then quick (fg.shader.check and fg.shader.draw print the times), but
// it is Metal's cache, not FunkGui's. target() stays message-thread only: a worker that needs it for shaderc's
// arguments gets it there first. On Linux and Windows check() runs set()'s quick checks, on any thread too: no driver
// compile there (the Linux and Windows paragraph below).
//
// ---- Textures (0.25.0) ----------------------------------------------------------------------------------------------
// A host registers texels under a texture id (setTexture: RGBA8 or R8, from memory) and binds that id to a sampler's
// name for one shader id (bindTexture); every draw of that shader then samples it, on Metal, Direct3D 11 and Vulkan
// alike: bgfx binds it at the sampler's stage with the texture's own sampling (linear or nearest, clamped or
// repeated; one level, no mipmaps). Message thread only, as set().
//   - RGBA8 reads as (r, g, b, a) / 255, R8 as (r / 255, 0, 0, 1): the texels as given (FunkGui converts nothing, so a
//     host that stores premultiplied colour reads premultiplied colour). Row 0 is the top: v = 0, as v_uv's.
//   - A sampler with no binding, or bound to an id with no texture (or one bgfx refused), reads (0, 0, 0, 0): FunkGui
//     binds a 1 x 1 texture of zeros there, never nothing.
//   - A binding belongs to (shader id, sampler name), not to a binary: set() replacing id's shader keeps it, remove(id)
//     drops id's. Many shaders may bind one texture id. removeTexture() keeps the bindings to it, which read zeros
//     until a texture is set under that id again; setTexture() of a registered id replaces its texels.
//   - The GPU textures are process-wide, one per texture id for every editor, made at their first draw after
//     setTexture() (and after every bgfx start) and destroyed by removeTexture(), a replacement or bgfx's shutdown; the
//     texels stay in the registry, as the shaders' binaries do.
//   - set() and check() take a sampler that is a 2D sampler of floats (SAMPLER2D: not 3D, cube, array, shadow or
//     integer) at a stage 0 to 3, one a stage, named otherwise than FunkGui's and bgfx's uniforms. The stage is the
//     Metal code's [[texture(n)]] on macOS, the SPIR-V binding less bgfx's shift of 2 on Linux, and the sampler's
//     register on Windows. Direct3D's reflection does not say a sampler's kind, so Windows checks the stage only; and
//     it lists sampler states, not textures, so there a texture read with texelFetch alone is not in the binary and
//     reads zeros: read it with texture2D or texture2DLod at least once (the stated fallback).
//   - Where shaders are not drawn (SoftRaster's placeholder, the web) textures change nothing.
//
// ---- Lifetime -------------------------------------------------------------------------------------------------------
// The registry keeps each id's binary. bgfx itself comes and goes with the editors (BgfxContext shuts it down when the
// last one closes, and rebuilds it when the primary one closes): its programs are destroyed with it, and each is built
// again from the kept binary the first time it is drawn after bgfx starts again, so a host sets an id once. set()
// while bgfx is not running checks everything it can without bgfx (all of the above, Metal's compile included) and
// stores the binary for the next start. Nothing is destroyed at process exit (bgfx is gone by then).
//
// On Linux (Vulkan): the same registry and the same checks, except that the code is checked only as SPIR-V words (the
// magic number and the size), not by the driver: bgfx creates the module at the next frame (the stated fallback). On
// Windows (Direct3D 11, v0.24.0) likewise, as Direct3D 11 bytecode (the DXBC container's magic): shaderc compiles HLSL
// there with the system's d3dcompiler_47.dll, which every Windows 10 and 11 has.
// Without bgfx (FUNKGUI_WITH_BGFX=OFF, the web) there is no registry: shader draws are a placeholder in SoftRaster and
// nothing in WebGlSink (canvas/PrimList.h).

#include <cstdint>
#include <string>
#include <string_view>

namespace funkgui::shaders
{
    // What to pass shaderc's --platform and -p for the renderer bgfx runs on: {"osx", "metal"} on macOS (Metal),
    // {"linux", "spirv"} on Linux (Vulkan), {"windows", "s_5_0"} on Windows (Direct3D 11, v0.24.0). Empty strings while
    // bgfx is not initialised (no editor has attached yet, or the last one closed): FunkGui's renderer is fixed per
    // platform, but this answers for the one running.
    struct Target
    {
        std::string platform, profile;
    };
    Target target();

    // Registers (or replaces) id's program: FunkGui's vertex shader + this shaderc fragment-shader binary for target().
    // The bytes are copied; the caller's may go. Returns false, with why set to a reason a host can show (or hand back
    // to whoever wrote the shader: one sentence, followed by Metal's compile log when Metal refused the code), when the
    // binary is empty or not a bgfx shader, is not a fragment shader, is of a shader format this bgfx does not read,
    // does not take `$input v_uv, v_px` with varyingDef()'s varyings, samples a texture otherwise than "Textures"
    // above allows, reads a uniform neither FunkGui nor bgfx sets (or one of FunkGui's with another type or size),
    // carries code for another renderer, does not compile (macOS: Metal's own compiler, as bgfx will call it, unless
    // check() or set() accepted the same code lately: "Checking it off the message thread" above), or bgfx refuses to
    // create the shader or link the program (only checked while bgfx runs). On false the id keeps what it had (its
    // previous program, or nothing). On true, why is cleared and the next frame draws the new program; id's texture
    // bindings are kept.
    bool set(uint32_t id, const void* fragmentBinary, uint32_t size, std::string& why);

    // set()'s checks without registering anything (0.25.0): on any thread, at any time, alongside anything (set() and
    // the frames included), with or without bgfx running. True, with why cleared, when set() would take the binary,
    // bgfx's own create and link aside (set() checks those while bgfx runs); else false with the reason set() would
    // give. On macOS this runs Metal's compiler, the slow part of set(), and remembers a code it accepted, so set() of
    // that binary afterwards does not compile it again while it is remembered ("Checking it off the message thread"
    // above: the last 256 codes used).
    bool check(const void* fragmentBinary, uint32_t size, std::string& why);

    // Unregisters id, destroys its program and drops its texture bindings; shader draws of it then draw nothing. An
    // unknown id is ignored.
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

    // ---- Textures (0.25.0; "Textures" above) ------------------------------------------------------------------------
    enum class TextureFormat : uint8_t
    {
        rgba8,                                   // 4 bytes a texel (r, g, b, a), read as (r, g, b, a) / 255
        r8,                                      // 1 byte a texel (r), read as (r / 255, 0, 0, 1)
    };

    struct TextureSampling
    {
        bool nearest = false;                    // the nearest texel; else bilinear between the four nearest
        bool repeat = false;                     // u and v wrap outside 0..1; else clamped to the edge texels
    };

    inline constexpr int kMaxTextureSide = 4096;     // the largest width and height setTexture() takes
    inline constexpr int kMaxSamplerStages = 4;      // a sampler's stage is 0 to kMaxSamplerStages - 1

    // Registers (or replaces) textureId's texels: w x h of format, row y at texels + y * strideBytes (0 = tightly
    // packed: w * 4 for rgba8, w for r8), the first row the top. The texels are copied; the caller's may go. Returns
    // false, and changes nothing, when w or h is outside 1..kMaxTextureSide, texels is null, or strideBytes is neither
    // 0 nor at least a row's bytes. The next frame draws the new texels wherever a shader binds textureId.
    bool setTexture(uint32_t textureId, int w, int h, TextureFormat format, const void* texels, int strideBytes = 0,
                    TextureSampling sampling = {});

    // Unregisters textureId and destroys its GPU texture; the samplers bound to it read (0, 0, 0, 0). Its bindings
    // stay. An unknown id is ignored.
    void removeTexture(uint32_t textureId);

    // Whether texels are registered under textureId (setTexture() succeeded and removeTexture() has not been called
    // since).
    bool hasTexture(uint32_t textureId);

    // Binds textureId to the sampler named `sampler` (as in SAMPLER2D(sampler, stage)) of shaderId's shader, replacing
    // what was bound there. Kept whether or not shaderId or textureId is registered yet (so a host may bind before
    // set()) until unbindTexture() or remove(shaderId). Returns whether shaderId's shader, as registered now, has a
    // sampler of that name: false for a name it does not declare (a binding no draw reads, until a shader that does
    // is set under shaderId).
    bool bindTexture(uint32_t shaderId, std::string_view sampler, uint32_t textureId);

    // Drops the binding of shaderId's sampler `sampler`: it reads (0, 0, 0, 0). An unknown binding is ignored.
    void unbindTexture(uint32_t shaderId, std::string_view sampler);
}

#pragma once

// Images (v0.20.0): a process-wide registry of pictures by id, which every sink draws wherever a Canvas recorded
// Canvas::image(id, dst, radius, opacity, uv) (canvas/PrimList.h ImageDraw), in order with the primitives: SoftRaster
// (HeadlessHost::writePng and every probe PNG) samples the pixels on the CPU, BgfxSink uploads a texture per id and
// WebGlSink one per id and sink. Plain C++ with no GPU, no JUCE and no Emscripten: the same in every configuration.
//
// A host decodes its PNG, JPEG or WebP files itself (FunkGui has no decoder) and hands the pixels to set(): straight
// (not premultiplied) RGBA8, rows from the top. set() copies them, so the caller's buffer may go at once.
//
// ---- Threads --------------------------------------------------------------------------------------------------------
// set() and remove() are meant for the message thread (they allocate, and set() does the work below). Every function
// is safe from any thread: one mutex guards the table, and a stored picture is immutable and shared (picture() hands
// out a std::shared_ptr to it), so a sink drawing on another thread keeps drawing the picture it took even while
// set() replaces it or remove() drops it. The registry is never destroyed (a plug-in is unloaded while statics are torn
// down in an order nothing here controls), as FunkGui's other registries.
//
// ---- What set() stores ----------------------------------------------------------------------------------------------
// The picture as premultiplied RGBA8 (each colour channel c becomes (c * a + 127) / 255 in integers: c * a / 255 to
// the nearest, so an opaque texel is unchanged) and a full chain of box-filtered levels below it, laid out as a GPU's
// mipmaps are: level l is max(1, w >> l) x max(1, h >> l), down to 1 x 1, each texel the rounded mean of the 2 x 2
// (or 2 x 1, 1 x 2) texels of the level above at 2x, 2x + 1 and 2y, 2y + 1 (clamped to that level: an odd last row or
// column of it is left out, as halving drops it). Premultiplied, so that filtering never bleeds the colour of a
// transparent texel into its neighbours. The chain costs a third more memory than the picture and is built once, by
// set(); sinks pick one level per draw (canvas/Expand.h imageLevel), so a thumbnail far smaller than its picture is
// not a sparse, aliased sample of it.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace funkgui::images
{
    inline constexpr int kMaxSide = 4096;            // the largest width and height set() takes

    // Registers id's picture: w x h straight RGBA8 texels, row y at rgba + y * strideBytes (0 = tightly packed,
    // w * 4). The pixels are copied. A picture already under id is replaced (a new revision: the GPU sinks upload the
    // new one at their next draw of it, and destroy the old texture). Returns false, and changes nothing, when w or h
    // is outside 1..kMaxSide, rgba is null, or strideBytes is neither 0 nor at least w * 4.
    bool set(uint32_t id, int w, int h, const uint8_t* rgba, int strideBytes = 0);

    // Unregisters id: image draws of it then draw nothing, and the GPU sinks destroy its texture at their next frame
    // (or when bgfx shuts down, or the web sink is destroyed). An unknown id is ignored.
    void remove(uint32_t id);

    bool has(uint32_t id);                           // whether a picture is registered under id
    int  width(uint32_t id);                         // its size in texels; 0 when absent
    int  height(uint32_t id);

    // ---- For sinks and tests: the stored picture ----------------------------------------------------------------
    // One level of a Picture: w x h premultiplied RGBA8 texels, tightly packed rows from the top, starting `offset`
    // bytes into Picture::pixels.
    struct Level
    {
        int         w = 0, h = 0;
        std::size_t offset = 0;
    };

    // What set() stored for one id, immutable once published.
    struct Picture
    {
        uint32_t id = 0;
        uint64_t revision = 0;                       // unique over the process's life (every set() takes the next)
        int      w = 0, h = 0;                       // levels[0]'s size: the picture as set
        std::vector<Level>   levels;                 // levelCount(w, h) of them, the largest first
        std::vector<uint8_t> pixels;                 // every level in order, back to back: bgfx's mip layout

        const uint8_t* level(int l) const noexcept
        {
            return pixels.data() + levels[static_cast<std::size_t>(l)].offset;
        }
    };

    // The picture registered under id now, or nullptr. Holding the pointer keeps that revision alive and unchanged.
    std::shared_ptr<const Picture> picture(uint32_t id);

    // A counter that changes with every set() that stored a picture and every remove() that dropped one, so a sink
    // can skip looking for stale textures while it is unchanged. Never 0.
    uint64_t generation();

    // The levels of a w x h picture: 1 + floor(log2(max(w, h))) (1 for 1 x 1; 13 for 4096); 0 when w or h < 1.
    int levelCount(int w, int h) noexcept;
}

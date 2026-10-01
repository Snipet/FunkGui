#pragma once

// A recorded frame into a browser canvas (v0.12.0; FunkGui::web, Emscripten only; FCompressor ADR-93): BgfxSink's
// contract (gpu/BgfxSink.h) on WebGL2, with what BgfxContext holds for the native sink folded in, since a canvas has
// one context and one sink. The PrimList a Canvas recorded is expanded by funkgui::expand (canvas/Expand.h: six
// 64-byte vertices per primitive, attributes at offsets 0, 8, 12, 16, 32, 48) and drawn as ONE draw call with one
// program, whose text is generated from the same shaders/*.sc the native renderers compile
// (cmake/FunkGuiShaderText.cmake, <funkgui/shaders/ui.es300.h>), u_viewSize = (logicalW, logicalH, 1 / dpi, seconds)
// and one R8 texture holding FontService's atlas (linear, clamped, no mips). Blending is SRC_ALPHA /
// ONE_MINUS_SRC_ALPHA; there is no depth test and no culling.
//
// The context is the sink's own: WebGL2 (never WebGL1), created on the canvas a CSS selector names with alpha,
// antialias, depth and stencil off, so the canvas is opaque like the native layer and a pixel is one sample. A canvas
// takes one sink. Nothing here aborts: with no such canvas, or a browser that gives no WebGL2 context, ok() is false,
// error() says why and submit() draws nothing, so a host can show a fallback.
//
// Context loss (the browser may take the context at any time: a GPU reset, too many contexts, a backgrounded tab): the
// sink listens for webglcontextlost and webglcontextrestored on its canvas. While the context is lost, lost() is true
// and submit() returns Result::lost; on the restore it builds the program, the buffer and the texture again, so the
// next submit() draws. The loss event is answered with preventDefault(), without which a browser never restores.
//
// This header is plain C++ (it compiles on every host); the class is defined only in FunkGui::web. Main thread only.
// No pacing, no input and no canvas sizing policy here: those are the web host's.

#include <funkgui/canvas/Expand.h>
#include <funkgui/canvas/PrimList.h>
#include <funkgui/canvas/SoftRaster.h>

#include <cstdint>
#include <string>
#include <vector>

namespace funkgui
{
    class WebGlSink
    {
    public:
        // submitted: cleared and drawn. empty: cleared, the list has no primitive (or the size is below 1 x 1: nothing
        // at all). lost: the context is lost, nothing was drawn. unavailable: there is no context or its resources
        // could not be built (error()), nothing was drawn.
        enum class Result : uint8_t { submitted, empty, lost, unavailable };

        // Creates the WebGL2 context on the canvas `canvasSelector` names (document.querySelector: "#editor") and
        // builds the program, the vertex buffer and the atlas texture (FontService's atlas: baked or loaded on first
        // use; an atlas that is not baked gives a 1 x 1 blank texture, text draws nothing and shapes still draw, as
        // on the native sink). Never throws and never aborts: see ok().
        explicit WebGlSink(const char* canvasSelector);

        // Deletes the resources and destroys the context. Emscripten's context teardown also removes every html5
        // event callback registered on that canvas.
        ~WebGlSink();

        WebGlSink(const WebGlSink&) = delete;
        WebGlSink& operator=(const WebGlSink&) = delete;

        // The context exists, is not lost, and the program, buffer and texture are built: submit() will draw.
        bool ok() const noexcept;

        // Why ok() is false after construction or a restore ("" when nothing failed; a lost context is not an error):
        // the canvas was not found, the browser gave no WebGL2 context, or a shader did not compile or link (with the
        // browser's log). For a host's fallback screen and diagnostics.
        const char* error() const noexcept { return error_.c_str(); }

        // One frame. Gives the canvas a physW x physH drawing buffer when it has another size (its width and height
        // attributes, never its CSS size), sets the viewport to it, clears to list.info.clear with alpha 1 and, unless
        // the list is empty, expands it into one buffer and draws it with one call. u_viewSize is taken from
        // list.info (dpi <= 0.05 counts as 1, as on the native sink). The browser presents the canvas when the caller
        // returns to its event loop.
        Result submit(const PrimList& list, int physW, int physH);

        // Diagnostics for a host. frameCount: submit() calls that cleared the canvas (submitted or empty), over the
        // sink's life. lost: the context is lost now. restoreCount: restores survived (each rebuilt the resources).
        // lastVertexCount: the vertices of the last submit() (0 unless it returned submitted).
        uint32_t frameCount() const noexcept { return frames_; }
        bool     lost() const noexcept;
        uint32_t restoreCount() const noexcept { return restores_; }
        uint32_t lastVertexCount() const noexcept { return lastVertices_; }

        // For tests: the drawing buffer as an Image (canvas/SoftRaster.h: RGBA8 rows from the top-left; alpha is 255,
        // the canvas is opaque) at the size of the last submit(). Call it in the task that called submit(): the
        // browser clears an unpreserved drawing buffer once it has presented it. An Image with w == 0 when nothing was
        // drawn, the context is lost or the read failed.
        Image readPixels();

    private:
        bool build();                                // program, vertex array, buffer, texture; false with error_ set
        void drop() noexcept;                        // deletes them (a lost context's objects: only the names)

        // em_webgl_context_callback, registered on the canvas with `this` as the user data.
        static bool onContextLost(int eventType, const void* reserved, void* self);
        static bool onContextRestored(int eventType, const void* reserved, void* self);

        std::string selector_;
        std::string error_;
        std::uintptr_t context_ = 0;                 // EMSCRIPTEN_WEBGL_CONTEXT_HANDLE; 0 = none
        bool     ready_ = false;                     // build() succeeded on the context as it is now
        bool     lostEvent_ = false;                 // between webglcontextlost and webglcontextrestored

        uint32_t program_ = 0;                       // GL names; 0 = none
        uint32_t vertexArray_ = 0;
        uint32_t buffer_ = 0;
        uint32_t texture_ = 0;
        int      uViewSize_ = -1;                    // uniform location

        std::vector<Vtx> scratch_;                   // 6 * prims, capacity kept across frames
        uint32_t frames_ = 0;
        uint32_t restores_ = 0;
        uint32_t lastVertices_ = 0;
        int      lastW_ = 0, lastH_ = 0;             // the last submit()'s physical size (readPixels)
    };
}

#include <funkgui/web/WebGlSink.h>

#include <funkgui/shaders/ui.es300.h>
#include <funkgui/text/FontService.h>

#include <emscripten/em_js.h>
#include <emscripten/em_macros.h>
#include <emscripten/html5.h>
#include <emscripten/html5_webgl.h>

#include <GLES3/gl3.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

// WebGlSink (v0.12.0; FCompressor ADR-93, docs/sprints/web/map-render.md): BgfxSink::submit and the part of
// BgfxContext it draws with (the vertex layout, the program, the uniform, the font texture), call for call on WebGL2
// through Emscripten's GLES3 bindings. The vertex stream is funkgui::expand's, the one fg.canvas.expansion pins, and
// the program is the text cmake/FunkGuiShaderText.cmake makes from shaders/*.sc, which fg.shader.web pins; what a
// browser draws from them is checked by test/web's page against SoftRaster (node has no WebGL).
//
// Every entry point makes the sink's context current first: Emscripten's GL calls go to one current context, and a page
// may hold others.

// Whether the selector names a canvas. querySelector throws on a malformed selector and getContext does not exist on
// another element: both would be an exception in Emscripten's own lookup, so they are settled here. (EM_JS defines a C
// symbol: file scope.)
EM_JS_DEPS(funkgui_web_sink_deps, "$UTF8ToString");
EM_JS(int, funkgui_web_is_canvas, (const char* selector), {
    try {
        return document.querySelector(UTF8ToString(selector)) instanceof HTMLCanvasElement ? 1 : 0;
    } catch (e) {
        return 0;
    }
});

namespace funkgui
{
    namespace
    {
        // funkgui::Vtx as the program's attributes: BgfxContext's vertex layout (A §2.2), by name, so the locations
        // are whatever the program was given.
        struct Attribute
        {
            const char* name;
            GLint       size;
            GLenum      type;
            GLboolean   normalised;
            std::size_t offset;
        };
        constexpr Attribute kAttributes[] = {
            { "a_position",  2, GL_FLOAT,         GL_FALSE, offsetof(Vtx, x)  },
            { "a_color0",    4, GL_UNSIGNED_BYTE, GL_TRUE,  offsetof(Vtx, c0) },
            { "a_color1",    4, GL_UNSIGNED_BYTE, GL_TRUE,  offsetof(Vtx, c1) },
            { "a_texcoord0", 4, GL_FLOAT,         GL_FALSE, offsetof(Vtx, d0) },
            { "a_texcoord1", 4, GL_FLOAT,         GL_FALSE, offsetof(Vtx, d1) },
            { "a_texcoord2", 4, GL_FLOAT,         GL_FALSE, offsetof(Vtx, d2) },
        };

        std::string trimmed(std::string s)
        {
            while (!s.empty() && (s.back() == '\0' || s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
                s.pop_back();
            return s;
        }

        // A compiled shader, or 0 with the browser's log in `log`.
        GLuint compile(GLenum type, const char* text, std::string& log)
        {
            const GLuint shader = glCreateShader(type);
            glShaderSource(shader, 1, &text, nullptr);
            glCompileShader(shader);
            GLint status = GL_FALSE;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
            if (status == GL_TRUE)
                return shader;
            GLint length = 0;
            glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
            log.assign(static_cast<std::size_t>(std::max(length, 1)), '\0');
            glGetShaderInfoLog(shader, static_cast<GLsizei>(log.size()), nullptr, log.data());
            log = trimmed(std::move(log));
            glDeleteShader(shader);
            return 0;
        }
    }

    WebGlSink::WebGlSink(const char* canvasSelector) : selector_(canvasSelector != nullptr ? canvasSelector : "")
    {
        if (selector_.empty() || funkgui_web_is_canvas(selector_.c_str()) == 0)
        {
            error_ = "no canvas matches '" + selector_ + "'";
            return;
        }

        // The native layer is opaque and single-sampled, and the frame needs no depth or stencil. With alpha the page
        // would show through every antialiased edge (the blend writes alpha below 1 there). No extension is used.
        EmscriptenWebGLContextAttributes attributes;
        emscripten_webgl_init_context_attributes(&attributes);
        attributes.alpha = false;
        attributes.depth = false;
        attributes.stencil = false;
        attributes.antialias = false;
        attributes.majorVersion = 2;
        attributes.minorVersion = 0;
        attributes.enableExtensionsByDefault = false;
        context_ = emscripten_webgl_create_context(selector_.c_str(), &attributes);
        if (context_ == 0)
        {
            error_ = "the browser gave no WebGL2 context for '" + selector_ + "'";
            return;
        }

        emscripten_set_webglcontextlost_callback(selector_.c_str(), this, false, &WebGlSink::onContextLost);
        emscripten_set_webglcontextrestored_callback(selector_.c_str(), this, false, &WebGlSink::onContextRestored);
        build();
    }

    WebGlSink::~WebGlSink()
    {
        if (context_ == 0)
            return;
        emscripten_set_webglcontextlost_callback(selector_.c_str(), nullptr, false, nullptr);
        emscripten_set_webglcontextrestored_callback(selector_.c_str(), nullptr, false, nullptr);
        drop();
        emscripten_webgl_destroy_context(context_);
    }

    bool WebGlSink::lost() const noexcept
    {
        // The flag of the browser's context as well as the event's: a context is lost before its event is delivered.
        return context_ != 0 && (lostEvent_ || emscripten_is_webgl_context_lost(context_));
    }

    bool WebGlSink::ok() const noexcept
    {
        return context_ != 0 && ready_ && !lost();
    }

    bool WebGlSink::onContextLost(int, const void*, void* self)
    {
        auto& sink = *static_cast<WebGlSink*>(self);
        sink.lostEvent_ = true;
        sink.drop();                                 // the context took its objects with it: forget their names
        return true;                                 // preventDefault(): the browser may restore this context
    }

    bool WebGlSink::onContextRestored(int, const void*, void* self)
    {
        auto& sink = *static_cast<WebGlSink*>(self);
        sink.lostEvent_ = false;
        if (sink.build())
            ++sink.restores_;
        return true;
    }

    void WebGlSink::drop() noexcept
    {
        ready_ = false;
        if (context_ == 0)
            return;
        // On a lost context these calls reach nothing in the browser, and still free Emscripten's own name tables.
        emscripten_webgl_make_context_current(context_);
        if (program_ != 0)
            glDeleteProgram(program_);
        if (vertexArray_ != 0)
            glDeleteVertexArrays(1, &vertexArray_);
        if (buffer_ != 0)
            glDeleteBuffers(1, &buffer_);
        if (texture_ != 0)
            glDeleteTextures(1, &texture_);
        program_ = vertexArray_ = buffer_ = texture_ = 0;
        uViewSize_ = -1;
        lastW_ = lastH_ = 0;                         // nothing this context drew is left to read back
    }

    bool WebGlSink::build()
    {
        drop();
        error_.clear();
        if (context_ == 0 || emscripten_is_webgl_context_lost(context_))
            return false;                            // nothing to build on; a restore builds
        emscripten_webgl_make_context_current(context_);
        while (glGetError() != GL_NO_ERROR) {}       // only this build's errors count below

        // ---- the program: shaders/vs_ui.sc + fs_ui.sc as GLSL ES 3.00
        std::string log;
        const GLuint vs = compile(GL_VERTEX_SHADER, shaders::vs_ui_es300, log);
        if (vs == 0)
        {
            if (!emscripten_is_webgl_context_lost(context_))
                error_ = "the vertex shader did not compile: " + log;
            return false;
        }
        const GLuint fs = compile(GL_FRAGMENT_SHADER, shaders::fs_ui_es300, log);
        if (fs == 0)
        {
            glDeleteShader(vs);
            if (!emscripten_is_webgl_context_lost(context_))
                error_ = "the fragment shader did not compile: " + log;
            return false;
        }
        program_ = glCreateProgram();
        glAttachShader(program_, vs);
        glAttachShader(program_, fs);
        glLinkProgram(program_);
        glDeleteShader(vs);                          // the program keeps them until it is deleted
        glDeleteShader(fs);
        GLint linked = GL_FALSE;
        glGetProgramiv(program_, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE)
        {
            GLint length = 0;
            glGetProgramiv(program_, GL_INFO_LOG_LENGTH, &length);
            log.assign(static_cast<std::size_t>(std::max(length, 1)), '\0');
            glGetProgramInfoLog(program_, static_cast<GLsizei>(log.size()), nullptr, log.data());
            const bool gone = emscripten_is_webgl_context_lost(context_);
            drop();
            if (!gone)
                error_ = "the program did not link: " + trimmed(std::move(log));
            return false;
        }
        glUseProgram(program_);
        uViewSize_ = glGetUniformLocation(program_, "u_viewSize");
        glUniform1i(glGetUniformLocation(program_, "s_texColor"), 0);

        // ---- the vertex stream: one buffer, refilled every frame, with Vtx's six attributes
        glGenVertexArrays(1, &vertexArray_);
        glBindVertexArray(vertexArray_);
        glGenBuffers(1, &buffer_);
        glBindBuffer(GL_ARRAY_BUFFER, buffer_);
        for (const Attribute& a : kAttributes)
        {
            const GLint location = glGetAttribLocation(program_, a.name);
            if (location < 0)
                continue;                            // the program does not read it
            glEnableVertexAttribArray(static_cast<GLuint>(location));
            glVertexAttribPointer(static_cast<GLuint>(location), a.size, a.type, a.normalised, sizeof(Vtx),
                                  reinterpret_cast<const void*>(a.offset));
        }

        // ---- the atlas: FontService's CPU bake (02 §3.4), the pixels the recorder laid this frame's text out
        // against; R8, linear, clamped, no mips, as BgfxContext's texture
        const FontAtlasSdf& atlas = FontService::get().atlas();
        glGenTextures(1, &texture_);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (atlas.baked())
        {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, FontAtlasSdf::kAtlasW, FontAtlasSdf::kAtlasH, 0, GL_RED,
                         GL_UNSIGNED_BYTE, atlas.pixels().data());
        }
        else
        {
            // A 1x1 texture so the sampler always has something valid bound. The recorder draws no text from an atlas
            // that is not baked; shapes still draw (BgfxContext::createResources).
            const uint8_t blank = 0;
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 1, 1, 0, GL_RED, GL_UNSIGNED_BYTE, &blank);
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        const GLenum glError = glGetError();
        if (glError != GL_NO_ERROR)
        {
            const bool gone = emscripten_is_webgl_context_lost(context_);
            drop();
            if (!gone)
                error_ = "WebGL error " + std::to_string(glError) + " while building the program, buffer and texture";
            return false;
        }
        ready_ = true;
        return true;
    }

    WebGlSink::Result WebGlSink::submit(const PrimList& list, int physW, int physH)
    {
        lastVertices_ = 0;
        if (context_ == 0)
            return Result::unavailable;
        if (lost())
            return Result::lost;
        if (!ready_)
            return Result::unavailable;
        if (physW < 1 || physH < 1)
            return Result::empty;
        emscripten_webgl_make_context_current(context_);
        const FrameInfo& info = list.info;

        // The drawing buffer is the frame's physical size (setting a canvas's size clears it and reallocates, so only
        // when it differs).
        int canvasW = 0, canvasH = 0;
        emscripten_get_canvas_element_size(selector_.c_str(), &canvasW, &canvasH);
        if (canvasW != physW || canvasH != physH)
            emscripten_set_canvas_element_size(selector_.c_str(), physW, physH);
        lastW_ = physW;
        lastH_ = physH;

        // BgfxSink's view: the whole target, cleared to the theme's ground even when nothing is drawn.
        glViewport(0, 0, physW, physH);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(static_cast<float>(info.clear.r) / 255.0f, static_cast<float>(info.clear.g) / 255.0f,
                     static_cast<float>(info.clear.b) / 255.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ++frames_;
        if (list.prims.empty())
            return Result::empty;

        expand(list, scratch_);
        const auto n = static_cast<GLsizei>(scratch_.size());

        glUseProgram(program_);
        glBindVertexArray(vertexArray_);
        glBindBuffer(GL_ARRAY_BUFFER, buffer_);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(scratch_.size() * sizeof(Vtx)), scratch_.data(),
                     GL_STREAM_DRAW);

        const float dpi = info.dpi > 0.05f ? info.dpi : 1.0f;     // HR's guard on the backing scale
        glUniform4f(uViewSize_, static_cast<float>(info.logicalW), static_cast<float>(info.logicalH), 1.0f / dpi,
                    info.seconds);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture_);

        // BGFX_STATE_WRITE_RGB | WRITE_A | BLEND_ALPHA: no depth, no culling, source-over in the stored values.
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_DITHER);
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDrawArrays(GL_TRIANGLES, 0, n);
        lastVertices_ = static_cast<uint32_t>(n);
        return Result::submitted;
    }

    Image WebGlSink::readPixels()
    {
        Image image;
        if (!ok() || lastW_ < 1 || lastH_ < 1)
            return image;
        emscripten_webgl_make_context_current(context_);
        while (glGetError() != GL_NO_ERROR) {}       // only the read's own error counts

        const auto w = static_cast<std::size_t>(lastW_), h = static_cast<std::size_t>(lastH_);
        std::vector<uint8_t> bottomUp(w * h * 4u);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, lastW_, lastH_, GL_RGBA, GL_UNSIGNED_BYTE, bottomUp.data());
        if (glGetError() != GL_NO_ERROR)
            return image;

        // GL's rows run from the bottom; an Image's from the top.
        image.w = lastW_;
        image.h = lastH_;
        image.rgba.resize(bottomUp.size());
        for (std::size_t y = 0; y < h; ++y)
            std::memcpy(&image.rgba[y * w * 4u], &bottomUp[(h - 1u - y) * w * 4u], w * 4u);
        return image;
    }
}

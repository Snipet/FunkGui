#include <funkgui/web/WebGlSink.h>

#include <funkgui/canvas/Images.h>
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
#include <memory>

// WebGlSink (v0.12.0; FCompressor ADR-93, docs/sprints/web/map-render.md): BgfxSink::submit and the part of
// BgfxContext it draws with (the vertex layout, the program, the uniform, the font texture), call for call on WebGL2
// through Emscripten's GLES3 bindings. The vertex stream is funkgui::expand's, the one fg.canvas.expansion pins, and
// the program is the text cmake/FunkGuiShaderText.cmake makes from shaders/*.sc, which fg.shader.web pins; what a
// browser draws from them is checked by test/web's page against SoftRaster (node has no WebGL).
//
// Every entry point makes the sink's context current first: Emscripten's GL calls go to one current context, and a page
// may hold others.

// What the selector names: 0 no canvas, 1 a canvas, 2 a canvas that carries a live sink's mark. querySelector throws on
// a malformed selector and getContext does not exist on another element: both would be an exception in Emscripten's own
// lookup, so they are settled here. (EM_JS defines a C symbol: file scope.)
//
// The mark is the sink's own: the property funkguiWebGlSink on the canvas element, set once the sink holds the context
// and deleted by its destructor (funkgui_web_mark_canvas). Emscripten's canvas.GLctxObject says nearly the same and is
// not used: it is an internal of libwebgl.js that no header names, and a second emscripten_webgl_create_context
// overwrites it rather than refusing. The name is written as a string so that a minifier leaves it alone, and a sink
// of another module instance on the page sees it too.
EM_JS_DEPS(funkgui_web_sink_deps, "$UTF8ToString");
EM_JS(int, funkgui_web_canvas_state, (const char* selector), {
    try {
        const canvas = document.querySelector(UTF8ToString(selector));
        if (!(canvas instanceof HTMLCanvasElement)) return 0;
        return canvas['funkguiWebGlSink'] ? 2 : 1;
    } catch (e) {
        return 0;
    }
});
EM_JS(void, funkgui_web_mark_canvas, (const char* selector, int marked), {
    try {
        const canvas = document.querySelector(UTF8ToString(selector));
        if (!canvas) return;
        if (marked) canvas['funkguiWebGlSink'] = true;
        else delete canvas['funkguiWebGlSink'];
    } catch (e) {
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

        // A linked program of the two texts, or 0 with what failed in `error` ("" when the context went away).
        GLuint link(const char* vsText, const char* fsText, const char* what, std::uintptr_t context,
                    std::string& error)
        {
            std::string log;
            const GLuint vs = compile(GL_VERTEX_SHADER, vsText, log);
            if (vs == 0)
            {
                if (!emscripten_is_webgl_context_lost(context))
                    error = std::string("the ") + what + " vertex shader did not compile: " + log;
                return 0;
            }
            const GLuint fs = compile(GL_FRAGMENT_SHADER, fsText, log);
            if (fs == 0)
            {
                glDeleteShader(vs);
                if (!emscripten_is_webgl_context_lost(context))
                    error = std::string("the ") + what + " fragment shader did not compile: " + log;
                return 0;
            }
            const GLuint program = glCreateProgram();
            glAttachShader(program, vs);
            glAttachShader(program, fs);
            glLinkProgram(program);
            glDeleteShader(vs);                      // the program keeps them until it is deleted
            glDeleteShader(fs);
            GLint linked = GL_FALSE;
            glGetProgramiv(program, GL_LINK_STATUS, &linked);
            if (linked == GL_TRUE)
                return program;
            GLint length = 0;
            glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
            log.assign(static_cast<std::size_t>(std::max(length, 1)), '\0');
            glGetProgramInfoLog(program, static_cast<GLsizei>(log.size()), nullptr, log.data());
            glDeleteProgram(program);
            if (!emscripten_is_webgl_context_lost(context))
                error = std::string("the ") + what + " program did not link: " + trimmed(std::move(log));
            return 0;
        }

        // Points the program's attributes of kAttributes at the bound buffer (one vertex array is bound).
        void pointAttributes(GLuint program)
        {
            for (const Attribute& a : kAttributes)
            {
                const GLint location = glGetAttribLocation(program, a.name);
                if (location < 0)
                    continue;                        // the program does not read it
                glEnableVertexAttribArray(static_cast<GLuint>(location));
                glVertexAttribPointer(static_cast<GLuint>(location), a.size, a.type, a.normalised, sizeof(Vtx),
                                      reinterpret_cast<const void*>(a.offset));
            }
        }
    }

    WebGlSink::WebGlSink(const char* canvasSelector) : selector_(canvasSelector != nullptr ? canvasSelector : "")
    {
        const int canvas = selector_.empty() ? 0 : funkgui_web_canvas_state(selector_.c_str());
        if (canvas == 0)
        {
            error_ = "no canvas matches '" + selector_ + "'";
            return;
        }
        if (canvas == 2)
        {
            // A second handle on the first sink's context: destroying either sink would strip the other's loss and
            // restore listeners (Emscripten removes a canvas's html5 callbacks with its context). Refused before
            // anything is created, so context_ stays 0 and the destructor leaves that sink's canvas alone.
            error_ = "the canvas '" + selector_ + "' already has a sink: destroy that one before constructing another";
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
        funkgui_web_mark_canvas(selector_.c_str(), 1);

        emscripten_set_webglcontextlost_callback(selector_.c_str(), this, false, &WebGlSink::onContextLost);
        emscripten_set_webglcontextrestored_callback(selector_.c_str(), this, false, &WebGlSink::onContextRestored);
        build();
    }

    WebGlSink::~WebGlSink()
    {
        if (context_ == 0)
            return;                                  // nothing was created (a refused sink): nothing here is this one's
        funkgui_web_mark_canvas(selector_.c_str(), 0);
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
        // v0.20.0: the image program and this context's picture textures.
        if (imageProgram_ != 0)
            glDeleteProgram(imageProgram_);
        if (imageVertexArray_ != 0)
            glDeleteVertexArrays(1, &imageVertexArray_);
        for (const ImageTexture& t : imageTextures_)
            glDeleteTextures(1, &t.name);
        imageTextures_.clear();
        imageProgram_ = imageVertexArray_ = 0;
        uImageViewSize_ = uImageRect_ = uImageUv_ = uImageClamp_ = uImageMask_ = -1;
        imagesSwept_ = 0;
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
        program_ = link(shaders::vs_ui_es300, shaders::fs_ui_es300, "UI", context_, error_);
        if (program_ == 0)
            return false;
        glUseProgram(program_);
        uViewSize_ = glGetUniformLocation(program_, "u_viewSize");
        glUniform1i(glGetUniformLocation(program_, "s_texColor"), 0);

        // ---- the vertex stream: one buffer, refilled every frame, with Vtx's six attributes
        glGenVertexArrays(1, &vertexArray_);
        glBindVertexArray(vertexArray_);
        glGenBuffers(1, &buffer_);
        glBindBuffer(GL_ARRAY_BUFFER, buffer_);
        pointAttributes(program_);

        // ---- v0.20.0: the image program (shaders/vs_custom.sc + fs_image.sc), its own vertex array over the same
        // buffer (it reads a_position and a_texcoord0 of the same Vtx), its sampler on texture unit 0
        imageProgram_ = link(shaders::vs_custom_es300, shaders::fs_image_es300, "image", context_, error_);
        if (imageProgram_ == 0)
        {
            drop();
            return false;
        }
        glUseProgram(imageProgram_);
        uImageViewSize_ = glGetUniformLocation(imageProgram_, "u_viewSize");
        uImageRect_ = glGetUniformLocation(imageProgram_, "u_imageRect");
        uImageUv_ = glGetUniformLocation(imageProgram_, "u_imageUv");
        uImageClamp_ = glGetUniformLocation(imageProgram_, "u_imageClamp");
        uImageMask_ = glGetUniformLocation(imageProgram_, "u_imageMask");
        glUniform1i(glGetUniformLocation(imageProgram_, "s_image"), 0);
        glGenVertexArrays(1, &imageVertexArray_);
        glBindVertexArray(imageVertexArray_);
        glBindBuffer(GL_ARRAY_BUFFER, buffer_);
        pointAttributes(imageProgram_);
        glBindVertexArray(vertexArray_);
        glUseProgram(program_);

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
        sweepImages();                               // v0.20.0: textures of removed or replaced pictures go
        if (list.prims.empty() && list.images.empty())
            return Result::empty;

        // The primitives' vertices, then (v0.20.0) each image draw's six, in list order, in one buffer.
        expand(list, scratch_);
        const auto imageVertices = static_cast<GLint>(scratch_.size());
        if (!list.images.empty())
        {
            scratch_.resize(scratch_.size() + list.images.size() * static_cast<std::size_t>(kVerticesPerPrim));
            Vtx* dst = scratch_.data() + imageVertices;
            for (const ImageDraw& m : list.images)
            {
                expandImage(m, dst);
                dst += kVerticesPerPrim;
            }
        }
        const auto n = static_cast<GLsizei>(scratch_.size());

        glBindBuffer(GL_ARRAY_BUFFER, buffer_);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(scratch_.size() * sizeof(Vtx)), scratch_.data(),
                     GL_STREAM_DRAW);

        const float dpi = info.dpi > 0.05f ? info.dpi : 1.0f;     // HR's guard on the backing scale
        const float aa = 1.0f / dpi;

        // BGFX_STATE_WRITE_RGB | WRITE_A | BLEND_ALPHA: no depth, no culling, source-over in the stored values.
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_DITHER);
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glActiveTexture(GL_TEXTURE0);

        // The draw calls in drawing order (drawSteps). Shader draws draw nothing here and split no run: the runs on
        // either side of one are one draw call, so a frame without image draws is the one call it always was.
        drawSteps(list, steps_);
        GLint runFirst = 0;
        GLsizei runCount = 0;
        const auto flushRun = [&]()
        {
            if (runCount == 0)
                return;
            glUseProgram(program_);
            glBindVertexArray(vertexArray_);
            glUniform4f(uViewSize_, static_cast<float>(info.logicalW), static_cast<float>(info.logicalH), aa,
                        info.seconds);
            glBindTexture(GL_TEXTURE_2D, texture_);
            glDisable(GL_SCISSOR_TEST);
            glDrawArrays(GL_TRIANGLES, runFirst, runCount);
            runCount = 0;
        };
        for (const DrawStep& step : steps_)
        {
            if (step.kind == DrawStep::Kind::shader)
                continue;
            if (step.kind == DrawStep::Kind::prims)
            {
                const auto first = static_cast<GLint>(step.first) * kVerticesPerPrim;
                if (runCount == 0)
                    runFirst = first;
                runCount += static_cast<GLsizei>(step.count) * kVerticesPerPrim;   // runs are contiguous
                continue;
            }
            flushRun();
            // v0.20.0: one image draw (BgfxSink's call for call: canvas/PrimList.h ImageDraw).
            const ImageDraw& m = list.images[step.first];
            int w = 0, h = 0;
            const GLuint name = imageTexture(m.id, w, h);
            const PixelRect px = imagePixels(m, info, physW, physH);
            if (name == 0 || px.w <= 0 || px.h <= 0)
                continue;                            // no picture, or nothing of it on screen
            const int level = imageLevel(m, dpi, w, h);
            const ImageSampling smp = imageSampling(m, level, w, h);
            glUseProgram(imageProgram_);
            glBindVertexArray(imageVertexArray_);
            glUniform4f(uImageViewSize_, static_cast<float>(info.logicalW), static_cast<float>(info.logicalH), aa,
                        info.seconds);
            glUniform4f(uImageRect_, m.dst.x, m.dst.y, m.dst.w, m.dst.h);
            glUniform4f(uImageUv_, smp.u0, smp.v0, smp.du, smp.dv);
            glUniform4f(uImageClamp_, smp.loU, smp.loV, smp.hiU, smp.hiV);
            glUniform4f(uImageMask_, m.radius, m.opacity, static_cast<float>(level), 0.0f);
            glBindTexture(GL_TEXTURE_2D, name);
            // The scissor: imagePixels' rows from the top, in GL's window coordinates from the bottom.
            glEnable(GL_SCISSOR_TEST);
            glScissor(px.x, physH - px.y - px.h, px.w, px.h);
            glDrawArrays(GL_TRIANGLES, imageVertices + static_cast<GLint>(step.first) * kVerticesPerPrim,
                         kVerticesPerPrim);
        }
        flushRun();
        glDisable(GL_SCISSOR_TEST);
        lastVertices_ = static_cast<uint32_t>(n);
        return Result::submitted;
    }

    // ---- v0.20.0: picture textures (web/WebGlSink.h) ----------------------------------------------------------------

    uint32_t WebGlSink::imageTexture(uint32_t id, int& w, int& h)
    {
        w = h = 0;
        const std::shared_ptr<const images::Picture> pic = images::picture(id);
        if (pic == nullptr)
            return 0;
        auto it = std::find_if(imageTextures_.begin(), imageTextures_.end(),
                               [id](const ImageTexture& t) { return t.id == id; });
        if (it != imageTextures_.end() && it->revision != pic->revision)
        {
            glDeleteTextures(1, &it->name);          // an older revision
            imageTextures_.erase(it);
            it = imageTextures_.end();
        }
        if (it == imageTextures_.end())
        {
            // Every level of the chain, so the texture is complete for LINEAR_MIPMAP_LINEAR and textureLod reads the
            // level asked for exactly.
            ImageTexture t;
            t.id = id;
            t.revision = pic->revision;
            glGenTextures(1, &t.name);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, t.name);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            const auto count = static_cast<GLint>(pic->levels.size());
            for (GLint l = 0; l < count; ++l)
            {
                const images::Level& lv = pic->levels[static_cast<std::size_t>(l)];
                glTexImage2D(GL_TEXTURE_2D, l, GL_RGBA8, lv.w, lv.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pic->level(l));
            }
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, count - 1);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            imageTextures_.push_back(t);
            it = imageTextures_.end() - 1;
        }
        w = pic->w;
        h = pic->h;
        return it->name;
    }

    void WebGlSink::sweepImages()
    {
        const uint64_t generation = images::generation();   // first: a change during the scan sweeps again next time
        if (generation == imagesSwept_)
            return;
        for (auto it = imageTextures_.begin(); it != imageTextures_.end();)
        {
            const std::shared_ptr<const images::Picture> now = images::picture(it->id);
            if (now != nullptr && now->revision == it->revision)
            {
                ++it;
                continue;
            }
            glDeleteTextures(1, &it->name);
            it = imageTextures_.erase(it);
        }
        imagesSwept_ = generation;
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

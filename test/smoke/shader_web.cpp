// FUNKGUI_TEST name=fg.shader.web timeout=300 gpu=0 links=harness
//
// fg.shader.web (v0.12.0; FCompressor ADR-93, docs/sprints/web-b.md G-C): the UI program as GLSL ES 3.00 text, which
// cmake/FunkGuiShaderText.cmake generates from shaders/{vs_ui,fs_ui,varying.def}.sc into
// <funkgui/shaders/ui.es300.h> and the WebGL2 sink (funkgui/web/WebGlSink.h) compiles in a browser. The script is
// plain CMake, so the header exists in every configuration and on every host, with or without bgfx, JUCE or
// Emscripten: this test runs in all of them against one set of golden rows.
//
// Golden rows: each text's FNV-1a hash and byte count (a changed shaders/*.sc, or a changed transformation, moves them:
// a DRIFT candidate, beside fg.shader.hash's rows for the same change).
//
// Spec rows, per stage:
// - version_first: the text starts with `#version 300 es` and a newline (GLSL ES accepts nothing before it).
// - highp: float, int and sampler2D are declared highp, and no lowp or mediump qualifier appears in the code.
// - no_bgfx_macro: nothing in the code needs bgfx's shaderc or bgfx_shader.sh: no `$` directive, no #include, no
//   gl_FragColor or gl_FragData, no bgfx identifier, and every bgfx_shader.sh macro, function or predefined uniform
//   the code names is one the text defines itself (today vec2_splat, texture2DLod and SAMPLER2D, in the preamble).
// - body_is_source: after the preamble the text is the .sc file (read from the source tree) byte for byte, less its
//   `$input`/`$output` and bgfx include lines and with gl_FragColor renamed, and the preamble holds nothing but
//   #version, precision, #define and in/out declarations: there is still one shader source.
// - vs_ui attributes: a_position, a_color0, a_color1, a_texcoord0, a_texcoord1, a_texcoord2 at locations 0..5, the
//   order of funkgui::Vtx's fields (offsets 0, 8, 12, 16, 32, 48), with canvas/Expand.h's types.
// - fs_ui output: `out vec4 fg_FragColor`, which the code assigns.

#include <funkgui/shaders/ui.es300.h>
#include <funkgui/test/Harness.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace T = funkgui::test;

namespace
{
    bool identChar(char c) noexcept
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    }

    // The text without its comments (// to the end of the line, /* to */), so prose cannot name a macro.
    std::string code(std::string_view text)
    {
        std::string out;
        out.reserve(text.size());
        for (std::size_t i = 0; i < text.size();)
        {
            if (text.compare(i, 2, "//") == 0)
            {
                while (i < text.size() && text[i] != '\n')
                    ++i;
            }
            else if (text.compare(i, 2, "/*") == 0)
            {
                const std::size_t end = text.find("*/", i + 2);
                i = end == std::string_view::npos ? text.size() : end + 2;
                out += ' ';
            }
            else
                out += text[i++];
        }
        return out;
    }

    std::vector<std::string_view> identifiers(std::string_view s)
    {
        std::vector<std::string_view> out;
        for (std::size_t i = 0; i < s.size();)
        {
            if (!identChar(s[i]))
            {
                ++i;
                continue;
            }
            std::size_t j = i;
            while (j < s.size() && identChar(s[j]))
                ++j;
            if (s[i] < '0' || s[i] > '9')
                out.push_back(s.substr(i, j - i));
            i = j;
        }
        return out;
    }

    bool names(std::string_view s, std::string_view identifier)
    {
        for (const std::string_view id : identifiers(s))
            if (id == identifier)
                return true;
        return false;
    }

    // What bgfx_shader.sh (and shaderc's own preprocessing) gives a .sc file and GLSL ES 3.00 does not have.
    constexpr std::string_view kBgfxNames[] = {
        "vec2_splat", "vec3_splat", "vec4_splat", "uvec2_splat", "uvec3_splat", "uvec4_splat", "ivec2_splat",
        "ivec3_splat", "ivec4_splat", "bvec2_splat", "bvec3_splat", "bvec4_splat", "mul", "saturate", "instMul",
        "mtxFromRows", "mtxFromCols", "texture2D", "texture2DLod", "texture2DLodOffset", "texture2DProj",
        "texture2DBias", "texture2DGrad", "texture2DArray", "texture2DArrayLod", "texture3D", "texture3DLod",
        "textureCube", "textureCubeLod", "textureCubeBias", "shadow2D", "shadow2DProj", "SAMPLER2D", "SAMPLER2DMS",
        "SAMPLER2DARRAY", "SAMPLER2DSHADOW", "SAMPLER3D", "SAMPLERCUBE", "ISAMPLER2D", "USAMPLER2D", "ISAMPLER3D",
        "USAMPLER3D", "ARRAY_BEGIN", "ARRAY_END", "CONST", "EARLY_DEPTH_STENCIL", "u_viewRect", "u_viewTexel",
        "u_view", "u_invView", "u_proj", "u_invProj", "u_viewProj", "u_invViewProj", "u_model", "u_modelView",
        "u_modelViewProj", "u_alphaRef", "u_alphaRef4",
    };

    bool noBgfxMacro(std::string_view text)
    {
        const std::string c = code(text);
        bool ok = c.find('$') == std::string::npos && c.find("#include") == std::string::npos;
        for (const std::string_view id : identifiers(c))
        {
            if (id == "gl_FragColor" || id == "gl_FragData" || id.starts_with("bgfx") || id.starts_with("BGFX"))
            {
                std::printf("INFO     the text names %.*s\n", static_cast<int>(id.size()), id.data());
                ok = false;
            }
            for (const std::string_view b : kBgfxNames)
                if (id == b && c.find("#define " + std::string(b)) == std::string::npos)
                {
                    std::printf("INFO     the text names %.*s and does not define it\n", static_cast<int>(b.size()),
                                b.data());
                    ok = false;
                }
        }
        return ok;
    }

    bool highp(std::string_view text)
    {
        const std::string c = code(text);
        return c.find("precision highp float;") != std::string::npos
            && c.find("precision highp int;") != std::string::npos
            && c.find("precision highp sampler2D;") != std::string::npos && !names(c, "lowp") && !names(c, "mediump");
    }

    // shaders/<name>.sc as the script keeps it: CRLF read as LF, the directive and bgfx include lines dropped.
    std::string sourceBody(const char* name)
    {
        const std::filesystem::path path =
            std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() / "shaders" / name;
        std::ifstream f(path, std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        if (!f)
        {
            std::printf("INFO     cannot read %s\n", path.string().c_str());
            return {};
        }
        std::istringstream lines(ss.str());
        std::string body, line;
        while (std::getline(lines, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.starts_with("$input") || line.starts_with("$output")
                || (line.starts_with("#include") && line.find("<bgfx_shader.sh>") != std::string::npos))
                continue;
            body += line;
            body += '\n';
        }
        return body;
    }

    std::string replaceAll(std::string s, std::string_view from, std::string_view to)
    {
        for (std::size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
            s.replace(at, from.size(), to);
        return s;
    }

    // The text ends with the source's body, and what precedes it is a preamble of known line kinds.
    bool bodyIsSource(std::string_view text, const std::string& body)
    {
        if (body.empty() || !text.ends_with(body))
            return false;
        std::istringstream preamble{ std::string(text.substr(0, text.size() - body.size())) };
        std::string line;
        int n = 0;
        while (std::getline(preamble, line))
        {
            const bool known = n == 0 ? line == "#version 300 es"
                                      : line.starts_with("precision highp ") || line.starts_with("#define ")
                                            || line.starts_with("in ") || line.starts_with("out ")
                                            || line.starts_with("layout(location = ");
            if (!known)
            {
                std::printf("INFO     preamble line %d is '%s'\n", n + 1, line.c_str());
                return false;
            }
            ++n;
        }
        return n > 0;
    }

    void stage(T::Probe& P, const std::string& key, const char* text, std::size_t bytes, const char* source,
               bool fragment)
    {
        const std::string_view sv(text, bytes);
        P.eq(key + ".nul_free", std::strlen(text) == bytes, 1);
        P.eq(key + ".version_first", sv.starts_with("#version 300 es\n"), 1);
        P.eq(key + ".highp", highp(sv), 1);
        P.eq(key + ".no_bgfx_macro", noBgfxMacro(sv), 1);
        std::string body = sourceBody(source);
        if (fragment)
            body = replaceAll(std::move(body), "gl_FragColor", "fg_FragColor");
        P.eq(key + ".body_is_source", bodyIsSource(sv, body), 1);
        P.hash(key + ".hash", T::fnv1a(text, bytes));
        P.num(key + ".bytes", static_cast<double>(bytes), T::Tol::exact());
    }
}

int main(int argc, char** argv)
{
    using namespace funkgui::shaders;
    T::Probe P("fg.shader.web", "", argc, argv);

    stage(P, "shader.vs_ui.es300", vs_ui_es300, sizeof vs_ui_es300 - 1, "vs_ui.sc", false);
    stage(P, "shader.fs_ui.es300", fs_ui_es300, sizeof fs_ui_es300 - 1, "fs_ui.sc", true);

    // The attributes WebGlSink points at funkgui::Vtx: the order of its fields, one location each.
    {
        const std::string vs = code(vs_ui_es300);
        const char* const want[] = { "in vec2 a_position;",  "in vec4 a_color0;",    "in vec4 a_color1;",
                                     "in vec4 a_texcoord0;", "in vec4 a_texcoord1;", "in vec4 a_texcoord2;" };
        bool ok = true;
        for (int i = 0; i < 6; ++i)
            ok = ok && vs.find("layout(location = " + std::to_string(i) + ") " + want[i] + "\n") != std::string::npos;
        P.eq("shader.vs_ui.es300.attributes", ok && vs.find("layout(location = 6)") == std::string::npos, 1);
    }
    {
        const std::string fs = code(fs_ui_es300);
        P.eq("shader.fs_ui.es300.output",
             fs.find("\nout vec4 fg_FragColor;\n") != std::string::npos
                 && fs.find("fg_FragColor = ") != std::string::npos,
             1);
    }
    return P.finish();
}

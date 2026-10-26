#include <funkgui/gpu/ShaderLibrary.h>

#include "ShaderLibraryDetail.h"

#include <funkgui/gpu/BgfxContext.h>
#include <funkgui/shaders/custom.text.h>

#include <bgfx/bgfx.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The custom-shader registry (v0.19.0; include/funkgui/gpu/ShaderLibrary.h has the contract). What set() checks before
// bgfx sees a binary, and why each check is here rather than left to bgfx:
// - the header: bgfx's createShader() refuses a bad signature, but quietly (a BX_TRACE), and its stage and version are
//   ours to explain;
// - the varyings: createProgram() refuses a fragment shader whose input hash is not the vertex shader's output hash
//   (shaderc's MurmurHash2A of the sorted $input / $output names), again quietly: the same comparison here gives the
//   reason;
// - the uniforms: a sampler would read an unbound texture and a uniform FunkGui does not set would read 0 for ever;
//   one of FunkGui's three with another type or size would be resized under the others by bgfx (its createUniform
//   keeps the larger), so all of these are refused by name;
// - the code: Metal compiles the text at the next frame and aborts the process when it cannot (BGFX_FATAL), so macOS
//   compiles it here first (src/gpu/ShaderCheckMetal.mm); Linux checks the SPIR-V words' magic and size only.
// The registry outlives bgfx: BgfxContext::destroyResources() calls releaseGpu() before every shutdown, and program()
// builds a kept binary again on its first draw after bgfx starts. Never destroyed (HR idiom, as the tag registry): a
// plug-in is unloaded while statics are torn down in an order nothing here controls, after bgfx is gone.

#if defined(__APPLE__)
  #include <funkgui/shaders/vs_custom.mtl.h>
#else
  #include <funkgui/shaders/vs_custom.spv.h>
#endif

namespace funkgui::shaders
{
    namespace
    {
#if defined(__APPLE__)
        constexpr const uint8_t* kVs = vs_custom_mtl;
        constexpr std::size_t    kVsBytes = sizeof vs_custom_mtl;
        constexpr bgfx::RendererType::Enum kRenderer = bgfx::RendererType::Metal;
#else
        constexpr const uint8_t* kVs = vs_custom_spv;
        constexpr std::size_t    kVsBytes = sizeof vs_custom_spv;
        constexpr bgfx::RendererType::Enum kRenderer = bgfx::RendererType::Vulkan;
#endif

        // bgfx's uniform types (bgfx::UniformType) and the flag bits shaderc ORs into a binary's type byte
        // (bgfx_p.h kUniformMask: fragment, sampler, read-only, compare).
        constexpr uint8_t kUniformFlags = 0x10 | 0x20 | 0x40 | 0x80;

        // bgfx_shader.sh's predefined uniforms (bgfx.cpp s_predefinedName): bgfx sets them itself.
        constexpr const char* kPredefined[] = {
            "u_viewRect", "u_viewTexel", "u_view", "u_invView", "u_proj", "u_invProj", "u_viewProj",
            "u_invViewProj", "u_model", "u_modelView", "u_invModelView", "u_modelViewProj", "u_alphaRef4",
            "bgfx_indirectArgBase",
        };

        // The front of a bgfx shader binary, as bgfx_p.h's createShader and the renderers read it: magic, the input
        // and output hashes (one hash before version 6), the uniforms, (compute: three thread counts), then the code's
        // size and the code itself, followed by one NUL.
        struct Binary
        {
            char     stage = 0;                      // 'V', 'F' or 'C'
            uint8_t  version = 0;
            uint32_t hashIn = 0, hashOut = 0;
            struct Uniform
            {
                std::string name;
                uint8_t     type = 0;                // bgfx::UniformType, flags removed
                uint8_t     num = 0;
            };
            std::vector<Uniform> uniforms;
            const uint8_t* code = nullptr;
            uint32_t       codeSize = 0;
        };

        class Reader
        {
        public:
            Reader(const uint8_t* p, std::size_t n) : p_(p), n_(n) {}
            bool bytes(void* out, std::size_t k)
            {
                if (k > n_ - at_)
                    return false;
                std::memcpy(out, p_ + at_, k);
                at_ += k;
                return true;
            }
            template <typename T> bool le(T& v)      // little-endian, as bx writes on every host FunkGui builds for
            {
                uint8_t b[sizeof(T)] = {};
                if (!bytes(b, sizeof b))
                    return false;
                uint64_t x = 0;
                for (std::size_t i = sizeof(T); i-- > 0;)
                    x = (x << 8) | static_cast<uint64_t>(b[i]);
                v = static_cast<T>(x);
                return true;
            }
            const uint8_t* here() const { return p_ + at_; }
            std::size_t left() const { return n_ - at_; }

        private:
            const uint8_t* p_;
            std::size_t n_, at_ = 0;
        };

        bool parse(const uint8_t* p, std::size_t n, Binary& b, std::string& why)
        {
            Reader r(p, n);
            uint8_t magic[4] = {};
            if (!r.bytes(magic, 4) || magic[1] != 'S' || magic[2] != 'H'
                || (magic[0] != 'V' && magic[0] != 'F' && magic[0] != 'C'))
            {
                why = "not a bgfx shader binary (shaderc's output starts with FSH and a version byte)";
                return false;
            }
            b.stage = static_cast<char>(magic[0]);
            b.version = magic[3];
            uint16_t count = 0;
            bool ok = r.le(b.hashIn);
            b.hashOut = b.hashIn;
            if (ok && b.version >= 6)
                ok = r.le(b.hashOut);
            ok = ok && r.le(count);
            for (uint16_t i = 0; ok && i < count; ++i)
            {
                uint8_t nameSize = 0, type = 0, num = 0;
                uint16_t regIndex = 0, regCount = 0, texInfo = 0, texFormat = 0;
                char name[256] = {};
                ok = r.le(nameSize) && r.bytes(name, nameSize) && r.le(type) && r.le(num) && r.le(regIndex)
                     && r.le(regCount) && (b.version < 8 || r.le(texInfo)) && (b.version < 10 || r.le(texFormat));
                if (ok)
                    b.uniforms.push_back({ std::string(name, nameSize), static_cast<uint8_t>(type & ~kUniformFlags),
                                           num });
            }
            uint16_t threads[3] = {};
            if (ok && b.stage == 'C')
                ok = r.le(threads[0]) && r.le(threads[1]) && r.le(threads[2]);
            ok = ok && r.le(b.codeSize) && b.codeSize > 0 && r.left() > b.codeSize;   // the code and its NUL
            if (!ok)
            {
                why = "a bgfx shader binary cut short or corrupted";
                return false;
            }
            b.code = r.here();
            return true;
        }

        // The vertex shader's front, parsed once (the embedded binary is FunkGui's own: it parses).
        const Binary& vsBinary()
        {
            static const Binary b = [] {
                Binary v;
                std::string ignored;
                parse(kVs, kVsBytes, v, ignored);
                return v;
            }();
            return b;
        }

        struct Entry
        {
            uint32_t             id = 0;
            std::vector<uint8_t> binary;
            bgfx::ShaderHandle   fs = BGFX_INVALID_HANDLE;
            bgfx::ProgramHandle  program = BGFX_INVALID_HANDLE;
            bool                 refused = false;    // bgfx refused it in this run: not retried before the next
        };

        struct Registry
        {
            std::vector<Entry>  entries;
            bgfx::ShaderHandle  vs = BGFX_INVALID_HANDLE;
            bgfx::UniformHandle uRect = BGFX_INVALID_HANDLE;
            bgfx::UniformHandle uParams = BGFX_INVALID_HANDLE;
        };

        Registry& registry()
        {
            static Registry* const r = new Registry();   // never destroyed (the file comment)
            return *r;
        }

        Entry* find(uint32_t id)
        {
            for (Entry& e : registry().entries)
                if (e.id == id)
                    return &e;
            return nullptr;
        }

        bool running()
        {
            return BgfxContext::get().valid() && bgfx::getRendererType() == kRenderer;
        }

        template <typename H> void drop(H& h)
        {
            if (bgfx::isValid(h))
                bgfx::destroy(h);
            h = BGFX_INVALID_HANDLE;
        }

        // The run's shared objects: the vertex shader and the two uniforms. False when bgfx refused the shader.
        bool shared()
        {
            Registry& reg = registry();
            if (!bgfx::isValid(reg.vs))
                reg.vs = bgfx::createShader(bgfx::copy(kVs, static_cast<uint32_t>(kVsBytes)));
            if (!bgfx::isValid(reg.uRect))
                reg.uRect = bgfx::createUniform("u_rect", bgfx::UniformType::Vec4);
            if (!bgfx::isValid(reg.uParams))
                reg.uParams = bgfx::createUniform("u_params", bgfx::UniformType::Vec4, 4);
            return bgfx::isValid(reg.vs);
        }

        // bgfx objects for a binary that passed check(): the fragment shader and the program. False with why.
        bool build(const std::vector<uint8_t>& bin, bgfx::ShaderHandle& fs, bgfx::ProgramHandle& program,
                   std::string& why)
        {
            if (!shared())
            {
                why = "bgfx refused FunkGui's custom vertex shader";
                return false;
            }
            fs = bgfx::createShader(bgfx::copy(bin.data(), static_cast<uint32_t>(bin.size())));
            if (!bgfx::isValid(fs))
            {
                why = "bgfx refused the fragment shader";
                return false;
            }
            program = bgfx::createProgram(registry().vs, fs, false);
            if (!bgfx::isValid(program))
            {
                drop(fs);
                why = "bgfx refused to link the program (the shader's $input is not the varyings of varyingDef())";
                return false;
            }
            return true;
        }

        bool isPredefined(std::string_view name)
        {
            return std::any_of(std::begin(kPredefined), std::end(kPredefined),
                               [name](const char* p) { return name == p; });
        }

        // Everything set() can say about a binary without bgfx (the file comment).
        bool check(const uint8_t* p, std::size_t n, std::string& why)
        {
            Binary b;
            if (!parse(p, n, b, why))
                return false;
            if (b.stage != 'F')
            {
                why = b.stage == 'V' ? "a vertex shader binary: compile with --type fragment"
                                     : "a compute shader binary: compile with --type fragment";
                return false;
            }
            const Binary& vs = vsBinary();
            if (b.version < 5 || b.version > vs.version)
            {
                why = "bgfx shader binary version " + std::to_string(b.version) + ", and FunkGui's bgfx reads 5 to "
                      + std::to_string(vs.version) + ": compile with the shaderc of FunkGui's bgfx";
                return false;
            }
            if (b.hashIn != vs.hashOut)
            {
                why = "the shader's varyings are not v_uv and v_px: its first line must be `$input v_uv, v_px`, "
                      "compiled with --varyingdef of shaders::varyingDef()";
                return false;
            }
            for (const Binary::Uniform& u : b.uniforms)
            {
                if (u.type == 0)                     // bgfx::UniformType::Sampler
                {
                    why = "the shader samples a texture (" + u.name + "), and custom shaders have none";
                    return false;
                }
                const bool ours = u.name == "u_viewSize" || u.name == "u_rect" || u.name == "u_params";
                if (!ours && !isPredefined(u.name))
                {
                    why = "the shader reads the uniform " + u.name
                          + ", which FunkGui never sets: use u_params (16 floats), u_rect and u_viewSize";
                    return false;
                }
                const int most = u.name == "u_params" ? 4 : 1;
                if (ours && (u.type != 2 || u.num < 1 || u.num > most))   // 2: bgfx::UniformType::Vec4
                {
                    why = "the shader declares " + u.name + " otherwise than FunkGui: "
                          + (u.name == "u_params" ? "uniform vec4 u_params[4];" : "uniform vec4 " + u.name + ";");
                    return false;
                }
            }
#if defined(__APPLE__)
            const std::string msl(reinterpret_cast<const char*>(b.code), b.codeSize);
            if (b.codeSize >= 4 && b.code[0] == 0x03 && b.code[1] == 0x02 && b.code[2] == 0x23 && b.code[3] == 0x07)
            {
                why = "the shader holds SPIR-V, not Metal: compile with --platform osx -p metal";
                return false;
            }
            return detail::metalCompiles(msl, why);
#else
            const bool spirv = b.codeSize >= 20 && b.codeSize % 4 == 0 && b.code[0] == 0x03 && b.code[1] == 0x02
                               && b.code[2] == 0x23 && b.code[3] == 0x07;     // 0x07230203, little-endian
            if (!spirv)
                why = "the shader holds no SPIR-V: compile with --platform linux -p spirv";
            return spirv;
#endif
        }
    }

    Target target()
    {
        if (!running())
            return {};
#if defined(__APPLE__)
        return { "osx", "metal" };
#else
        return { "linux", "spirv" };
#endif
    }

    bool set(uint32_t id, const void* fragmentBinary, uint32_t size, std::string& why)
    {
        if (fragmentBinary == nullptr || size == 0)
        {
            why = "no shader binary";
            return false;
        }
        const auto* p = static_cast<const uint8_t*>(fragmentBinary);
        if (!check(p, size, why))
            return false;

        std::vector<uint8_t> bin(p, p + size);
        bgfx::ShaderHandle fs = BGFX_INVALID_HANDLE;
        bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
        if (running() && !build(bin, fs, program, why))
            return false;                            // the id keeps what it had

        Entry* e = find(id);
        if (e == nullptr)
        {
            registry().entries.push_back({});
            e = &registry().entries.back();
            e->id = id;
        }
        if (running())
        {
            drop(e->program);                        // bgfx frees them after the frame that may still use them
            drop(e->fs);
        }
        e->binary = std::move(bin);
        e->fs = fs;
        e->program = program;
        e->refused = false;
        why.clear();
        return true;
    }

    void remove(uint32_t id)
    {
        auto& entries = registry().entries;
        for (auto it = entries.begin(); it != entries.end(); ++it)
            if (it->id == id)
            {
                if (running())
                {
                    drop(it->program);
                    drop(it->fs);
                }
                entries.erase(it);
                return;
            }
    }

    bool has(uint32_t id) { return find(id) != nullptr; }

    const char* varyingDef() { return text::varying_custom_def; }

    const char* fragmentPrelude() { return text::fs_custom_prelude; }

    const char* bgfxShaderSh() { return text::bgfx_shader_sh; }

    namespace detail
    {
        bgfx::ProgramHandle program(uint32_t id)
        {
            Entry* e = find(id);
            if (e == nullptr || !running())
                return BGFX_INVALID_HANDLE;
            if (!bgfx::isValid(e->program) && !e->refused)
            {
                std::string why;
                e->refused = !build(e->binary, e->fs, e->program, why);
            }
            return e->program;
        }

        bgfx::UniformHandle uRect() { return registry().uRect; }

        bgfx::UniformHandle uParams() { return registry().uParams; }

        void releaseGpu()
        {
            Registry& reg = registry();
            for (Entry& e : reg.entries)
            {
                drop(e.program);
                drop(e.fs);
                e.refused = false;
            }
            drop(reg.vs);
            drop(reg.uRect);
            drop(reg.uParams);
        }
    }
}

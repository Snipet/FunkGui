#include <funkgui/gpu/ShaderLibrary.h>

#include "ShaderLibraryDetail.h"

#include <funkgui/gpu/BgfxContext.h>
#include <funkgui/shaders/custom.text.h>

#include <bgfx/bgfx.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iterator>
#include <mutex>
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
// - the uniforms: a uniform FunkGui does not set would read 0 for ever, and one of FunkGui's three with another type or
//   size would be resized under the others by bgfx (its createUniform keeps the larger), so these are refused by name;
// - the samplers (0.25.0): only 2D samplers of floats at stages 0 to 3 are bound (with the texture bound to the name,
//   or a 1 x 1 texture of zeros), so any other kind of texture, a stage outside them or shared, or a buffer or image
//   would read what nothing binds; each is refused by name, with the stage found as bgfx's renderer finds it (the
//   Metal text's [[texture(n)]], the SPIR-V binding less kSpirvBindShift, the Direct3D 11 register) and the kind
//   from the code where it says (the Metal text's texture2d<float>, the SPIR-V OpTypeImage). shaderc lists a plain
//   int uniform as a sampler too, and Metal's float, vec2 and vec3 uniforms with no type: those are uniforms FunkGui
//   never sets, refused as such. The samplers are read after the code's format (a binary for another renderer is
//   called that) and before Metal compiles it;
// - the code: Metal compiles the text at the next frame and aborts the process when it cannot (BGFX_FATAL), so macOS
//   compiles it here first (src/gpu/ShaderCheckMetal.mm); Linux checks the SPIR-V words' magic and size only, and
//   Windows (v0.24.0) the Direct3D 11 bytecode's DXBC container magic only (bgfx's Direct3D 11 creates the shader from
//   it at the next frame, and a refused one is a failed draw, not a fatal error).
// The registry outlives bgfx: BgfxContext::destroyResources() calls releaseGpu() before every shutdown, and program()
// builds a kept binary again on its first draw after bgfx starts. Never destroyed (HR idiom, as the tag registry): a
// plug-in is unloaded while statics are torn down in an order nothing here controls, after bgfx is gone.
//
// Textures (0.25.0) are a second table beside the programs: the texels by texture id, kept across bgfx's restarts as
// the binaries are, each uploaded at its first draw in a bgfx run, and the bindings (shader id, sampler name) ->
// texture id, which no binary owns. Message thread only, as set().
//
// check() (0.25.0) is all of the above that needs no bgfx, on any thread: it reads only its arguments, the embedded
// vertex shader's front (parsed once, under the language's guard for a function-local static) and constants, and
// Metal's compile goes through the record of accepted codes below, under its own mutex. The record holds the MSL
// text itself, not a hash, so a code is taken unseen only when it is byte for byte one Metal compiled in this process;
// Metal's answer for a text does not change while the process runs (the same compiler and the same default device).
// The compile runs outside the lock, so two threads checking one new code at once may both compile it; it is recorded
// once.

#if defined(__APPLE__)
  #include <funkgui/shaders/vs_custom.mtl.h>
#elif defined(_WIN32)
  #include <funkgui/shaders/vs_custom.dx11.h>
#else
  #include <funkgui/shaders/vs_custom.spv.h>
#endif

namespace funkgui::shaders
{
    namespace
    {
        using detail::Code;
        using detail::Sampler;

#if defined(__APPLE__)
        constexpr const uint8_t* kVs = vs_custom_mtl;
        constexpr std::size_t    kVsBytes = sizeof vs_custom_mtl;
        constexpr bgfx::RendererType::Enum kRenderer = bgfx::RendererType::Metal;
        constexpr Code kCode = Code::metal;
#elif defined(_WIN32)
        constexpr const uint8_t* kVs = vs_custom_dx11;
        constexpr std::size_t    kVsBytes = sizeof vs_custom_dx11;
        constexpr bgfx::RendererType::Enum kRenderer = bgfx::RendererType::Direct3D11;
        constexpr Code kCode = Code::dxbc;
#else
        constexpr const uint8_t* kVs = vs_custom_spv;
        constexpr std::size_t    kVsBytes = sizeof vs_custom_spv;
        constexpr bgfx::RendererType::Enum kRenderer = bgfx::RendererType::Vulkan;
        constexpr Code kCode = Code::spirv;
#endif

        // bgfx's uniform types (bgfx::UniformType: Sampler 0, End 1, Vec4 2) and the flag bits shaderc ORs into a
        // binary's type byte (bgfx_p.h kUniformMask: fragment, sampler, read-only, compare).
        constexpr uint8_t kSamplerType = 0, kEndType = 1, kVec4Type = 2;
        constexpr uint8_t kSamplerFlag = 0x20, kCompareFlag = 0x80;
        constexpr uint8_t kUniformFlags = 0x10 | kSamplerFlag | 0x40 | kCompareFlag;

        // The register index shaderc's Metal path writes for a live uniform outside the uniform buffer (glslang's
        // offset -1: a separate texture, sampler state or image); a plain uniform has its byte offset there.
        constexpr uint16_t kNoOffset = 0xFFFF;

        // A SPIR-V binary's texture bindings are the declared stage plus bgfx's shift (shader.h kSpirvBindShift) in
        // its binding model of version 11 on, the one renderer_vk.cpp reads as `stage = regIndex - kSpirvBindShift`;
        // its tex info is the texture's component type (bits 0-7: 0 float) and dimension (bits 8-15: 2 for 2D),
        // shader.cpp's ids.
        constexpr uint16_t kSpirvBindShift = 2;
        constexpr uint16_t kTexFloat2D = 0x0200;

        // The uniforms FunkGui gives a vec4 (its custom programs' three, the image program's, and v0.26.0's table of
        // the instanced UI program, BgfxContext's u_unorm8), which no sampler may be named after (bgfx's createUniform
        // would change the type of the uniform the other programs read). Every vec4 uniform FunkGui creates is here.
        constexpr const char* kFunkGuiUniforms[] = {
            "u_viewSize", "u_rect", "u_params", "u_imageRect", "u_imageUv", "u_imageClamp", "u_imageMask", "u_unorm8",
        };

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
                uint8_t     flags = 0;               // the flags (kUniformFlags) of the type byte
                uint8_t     num = 0;
                uint16_t    reg = 0;                 // the register index: a sampler's binding, as shaderc wrote it
                uint16_t    texInfo = 0;             // (version 8 on) a texture's component type and dimension
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
                                           static_cast<uint8_t>(type & kUniformFlags), num, regIndex, texInfo });
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

        // A program's bgfx objects in one bgfx run.
        struct Gpu
        {
            bgfx::ShaderHandle  fs = BGFX_INVALID_HANDLE;
            bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
            std::vector<bgfx::UniformHandle> samplers;   // one per Entry::samplers, in order (0.25.0)
        };

        struct Entry
        {
            uint32_t             id = 0;
            std::vector<uint8_t> binary;
            std::vector<Sampler> samplers;           // the binary's, with their stages (0.25.0)
            Gpu                  gpu;
            bool                 refused = false;    // bgfx refused it in this run: not retried before the next
        };

        // A texture of the registry (0.25.0): its texels kept for every bgfx run, its GPU texture made at its first
        // draw in one.
        struct Texture
        {
            uint32_t             id = 0;
            int                  w = 0, h = 0;
            TextureFormat        format = TextureFormat::rgba8;
            TextureSampling      sampling;
            std::vector<uint8_t> texels;             // tightly packed rows, the top first
            bgfx::TextureHandle  handle = BGFX_INVALID_HANDLE;
            bool                 refused = false;    // bgfx refused it in this run: not retried before the next
        };

        struct Binding
        {
            uint32_t    shader = 0;
            std::string sampler;
            uint32_t    texture = 0;
        };

        struct Registry
        {
            std::vector<Entry>  entries;
            std::vector<Texture> textures;           // 0.25.0
            std::vector<Binding> bindings;           // 0.25.0
            uint64_t            generation = 1;      // detail::generation() (0.25.0)
            bgfx::ShaderHandle  vs = BGFX_INVALID_HANDLE;
            bgfx::UniformHandle uRect = BGFX_INVALID_HANDLE;
            bgfx::UniformHandle uParams = BGFX_INVALID_HANDLE;
            bgfx::TextureHandle zeros = BGFX_INVALID_HANDLE;   // 1 x 1, (0, 0, 0, 0): an unbound sampler's (0.25.0)
            bool                zerosRefused = false;
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

        Texture* findTexture(uint32_t id)
        {
            for (Texture& t : registry().textures)
                if (t.id == id)
                    return &t;
            return nullptr;
        }

        Binding* findBinding(uint32_t shader, std::string_view sampler)
        {
            for (Binding& b : registry().bindings)
                if (b.shader == shader && b.sampler == sampler)
                    return &b;
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

        void drop(Gpu& g)
        {
            drop(g.program);
            drop(g.fs);
            for (bgfx::UniformHandle& u : g.samplers)
                drop(u);
            g.samplers.clear();
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

        // bgfx objects for a binary that passed check(): the fragment shader, the program and (0.25.0) a sampler
        // uniform per sampler, the handle bgfx::setTexture takes. False with why, and nothing left created.
        bool build(const std::vector<uint8_t>& bin, const std::vector<Sampler>& samplers, Gpu& out, std::string& why)
        {
            if (!shared())
            {
                why = "bgfx refused FunkGui's custom vertex shader";
                return false;
            }
            Gpu g;
            g.fs = bgfx::createShader(bgfx::copy(bin.data(), static_cast<uint32_t>(bin.size())));
            if (!bgfx::isValid(g.fs))
            {
                why = "bgfx refused the fragment shader";
                return false;
            }
            g.program = bgfx::createProgram(registry().vs, g.fs, false);
            if (!bgfx::isValid(g.program))
            {
                drop(g);
                why = "bgfx refused to link the program (the shader's $input is not the varyings of varyingDef())";
                return false;
            }
            for (const Sampler& s : samplers)
            {
                g.samplers.push_back(bgfx::createUniform(s.name.c_str(), bgfx::UniformType::Sampler));
                if (!bgfx::isValid(g.samplers.back()))
                {
                    drop(g);
                    why = "bgfx refused the shader's sampler " + s.name;
                    return false;
                }
            }
            out = std::move(g);
            return true;
        }

        bool isPredefined(std::string_view name)
        {
            return std::any_of(std::begin(kPredefined), std::end(kPredefined),
                               [name](const char* p) { return name == p; });
        }

        bool isFunkGuis(std::string_view name)
        {
            return std::any_of(std::begin(kFunkGuiUniforms), std::end(kFunkGuiUniforms),
                               [name](const char* p) { return name == p; });
        }

        std::string neverSet(const std::string& name)
        {
            return "the shader reads the uniform " + name
                   + ", which FunkGui never sets: use u_params (16 floats), u_rect and u_viewSize";
        }

        std::string neverBound(const std::string& name)
        {
            return "the shader uses " + name + ", a buffer, image or texture FunkGui never binds: custom shaders read "
                   "2D textures through SAMPLER2D only";
        }

        // Whether an entry of type Sampler is a texture (0.25.0). shaderc writes a live int uniform as a Sampler too
        // (its reflection's GL_INT case, with the int's byte offset as its register); SPIR-V and Direct3D 11 mark a
        // texture with the sampler bit, as bgfx's renderers read it, and Metal sets no bit but lists a texture with
        // num 0 (an int has its array size, 1 or more).
        bool isTexture(const Binary::Uniform& u, Code code)
        {
            return code == Code::metal ? u.num == 0 : (u.flags & kSamplerFlag) != 0;
        }

        // The type of the SPIR-V image at a binding, as the code declares it (OpTypeImage's operands): what the tex
        // info does not say, since shaderc's dimension drops the multisample flag and says nothing of the format.
        struct SpirvImage
        {
            uint32_t dim = 0, arrayed = 0, ms = 0, sampled = 0, format = 0;
        };

        // The image type of the variable decorated with Binding `binding`; false when the code is no SPIR-V module,
        // is cut short or has no image there. Types precede the variables that use them in a module, so one pass.
        bool spirvImage(const Binary& b, uint32_t binding, SpirvImage& out)
        {
            constexpr uint32_t kMagic = 0x07230203, kOpTypeImage = 25, kOpTypePointer = 32, kOpVariable = 59;
            constexpr uint32_t kOpDecorate = 71, kDecorationBinding = 33;
            const std::size_t words = b.codeSize / 4;
            const auto word = [&b](std::size_t i) {
                const uint8_t* p = b.code + i * 4;   // little-endian, as Reader
                return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8
                       | static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
            };
            if (words < 5 || word(0) != kMagic)
                return false;
            std::vector<uint32_t> bound;                                  // ids decorated with the binding
            std::vector<std::pair<uint32_t, uint32_t>> pointers;          // pointer type -> its pointee
            std::vector<std::pair<uint32_t, SpirvImage>> images;          // image type -> its operands
            for (std::size_t i = 5; i < words;)
            {
                const uint32_t op = word(i) & 0xFFFF, count = word(i) >> 16;
                if (count == 0 || count > words - i)
                    return false;
                if (op == kOpDecorate && count >= 4 && word(i + 2) == kDecorationBinding && word(i + 3) == binding)
                    bound.push_back(word(i + 1));
                else if (op == kOpTypePointer && count >= 4)
                    pointers.emplace_back(word(i + 1), word(i + 3));
                else if (op == kOpTypeImage && count >= 9)
                    images.emplace_back(word(i + 1),
                                        SpirvImage{ word(i + 3), word(i + 5), word(i + 6), word(i + 7), word(i + 8) });
                else if (op == kOpVariable && count >= 4
                         && std::find(bound.begin(), bound.end(), word(i + 2)) != bound.end())
                {
                    const auto p = std::find_if(pointers.begin(), pointers.end(),
                                                [t = word(i + 1)](const auto& x) { return x.first == t; });
                    if (p != pointers.end())
                    {
                        const auto img = std::find_if(images.begin(), images.end(),
                                                      [t = p->second](const auto& x) { return x.first == t; });
                        if (img != images.end())
                        {
                            out = img->second;
                            return true;
                        }
                    }
                }
                i += count;
            }
            return false;
        }

        // SAMPLER2D's texture in SPIR-V: Dim 2D (1), not arrayed, single-sampled, sampled through a sampler (not a
        // storage image: Sampled 2) and of no fixed format (an IMAGE2D_RO's is its declared one).
        bool isSampler2D(const SpirvImage& t)
        {
            return t.dim == 1 && t.arrayed == 0 && t.ms == 0 && t.sampled != 2 && t.format == 0;
        }

        // The name of a Metal argument buffer other than the uniforms' (bgfx binds a fragment shader's uniforms at
        // [[buffer(0)]]): SPIRV-Cross writes a BUFFER_RO or BUFFER_RW as `... name [[buffer(n)]]`, and shaderc's table
        // lists none of it. Empty when the code has none.
        std::string metalBuffer(std::string_view msl)
        {
            constexpr std::string_view key = " [[buffer(";
            for (std::size_t at = msl.find(key); at != std::string_view::npos; at = msl.find(key, at + key.size()))
            {
                if (msl.substr(at + key.size(), 4) == "0)]]")
                    continue;
                const auto word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; };
                std::size_t from = at;
                while (from > 0 && word(msl[from - 1]))
                    --from;
                return from < at ? std::string(msl.substr(from, at - from)) : std::string("a buffer");
            }
            return {};
        }

        // The stage of Metal's `texture2d<float> <name> [[texture(<n>)]]`, the argument SPIRV-Cross writes for a
        // SAMPLER2D(name, n); -1 when the code has none (another kind of texture: texture3d, texturecube,
        // texture2d_array, depth2d, texture2d<int>, ...).
        int metalStage(std::string_view msl, const std::string& name)
        {
            const std::string key = "texture2d<float> " + name + " [[texture(";
            const std::size_t at = msl.find(key);
            if (at == std::string_view::npos)
                return -1;
            int stage = 0;
            std::size_t i = at + key.size();
            const std::size_t first = i;
            for (; i < msl.size() && i < first + 4 && msl[i] >= '0' && msl[i] <= '9'; ++i)
                stage = stage * 10 + (msl[i] - '0');
            return i > first && msl.substr(i, 3) == ")]]" ? stage : -1;
        }

        // The binary's samplers (0.25.0; the file comment): each texture of type Sampler with its stage, by `code`'s
        // rules (an int uniform, which shaderc also types Sampler, is a uniform FunkGui never sets), and the entries of
        // type End (no value) allowed only as a Metal sampler's own <name>Texture and <name>Sampler. Any other End is,
        // on Metal, a plain uniform of a type shaderc does not map (float, vec2, vec3, bool, an int vector) when it has
        // a uniform-buffer offset, and else an image; on SPIR-V a storage image or buffer. A Metal argument buffer is
        // not in the table at all: the code says.
        bool samplersOf(const Binary& b, Code code, std::vector<Sampler>& out, std::string& why)
        {
            out.clear();
            const std::string_view text(reinterpret_cast<const char*>(b.code), b.codeSize);
            for (const Binary::Uniform& u : b.uniforms)
            {
                if (u.type != kSamplerType)
                    continue;
                if (!isTexture(u, code))
                {
                    if (isPredefined(u.name))
                        continue;
                    why = neverSet(u.name);
                    return false;
                }
                if (isFunkGuis(u.name) || isPredefined(u.name))
                {
                    why = "the shader's sampler " + u.name + " has the name of a uniform FunkGui or bgfx sets: give it "
                          "another name";
                    return false;
                }
                int stage = -1;
                bool kind = true;
                if (code == Code::metal)
                {
                    stage = metalStage(text, u.name);
                    kind = stage >= 0;
                }
                else if (code == Code::spirv)
                {
                    if (b.version < 11)
                    {
                        why = "the shader samples a texture in a SPIR-V binary of version " + std::to_string(b.version)
                              + ", bound otherwise than version 11's: compile with the shaderc of FunkGui's bgfx";
                        return false;
                    }
                    kind = (u.flags & kCompareFlag) == 0 && u.texInfo == kTexFloat2D;
                    stage = u.reg >= kSpirvBindShift ? static_cast<int>(u.reg - kSpirvBindShift) : -1;
                }
                else
                {
                    stage = static_cast<int>(u.reg);  // Direct3D's reflection says no kind (gpu/ShaderLibrary.h)
                }
                if (!kind)
                {
                    why = "the shader's sampler " + u.name + " is not a 2D sampler of floats: declare it with "
                          "SAMPLER2D(" + u.name + ", stage)";
                    return false;
                }
                if (stage < 0 || stage >= kMaxSamplerStages)
                {
                    why = "the shader's sampler " + u.name + " is at stage " + std::to_string(stage)
                          + ": a custom shader's samplers take stages 0 to " + std::to_string(kMaxSamplerStages - 1);
                    return false;
                }
                for (const Sampler& o : out)
                    if (o.stage == stage)
                    {
                        why = "the shader's samplers " + o.name + " and " + u.name + " are both at stage "
                              + std::to_string(stage) + ": give each its own";
                        return false;
                    }
                if (code == Code::spirv)
                {
                    // The tex info says 2D for a multisampled texture and an image too: the code's own type says.
                    SpirvImage t;
                    if (!spirvImage(b, u.reg, t))
                    {
                        why = "a bgfx shader binary cut short or corrupted (its code has no texture for the sampler "
                              + u.name + ")";
                        return false;
                    }
                    if (!isSampler2D(t))
                    {
                        why = "the shader's sampler " + u.name + " is not a 2D sampler of floats: declare it with "
                              "SAMPLER2D(" + u.name + ", stage)";
                        return false;
                    }
                }
                out.push_back({ u.name, static_cast<uint8_t>(stage) });
            }
            for (const Binary::Uniform& u : b.uniforms)
            {
                if (u.type != kEndType)
                    continue;
                const auto part = [&u](const Sampler& s, std::string_view suffix) {
                    return u.name.size() == s.name.size() + suffix.size() && u.name.starts_with(s.name)
                           && u.name.ends_with(suffix);
                };
                const bool own = std::any_of(out.begin(), out.end(), [&part](const Sampler& s) {
                    return part(s, "Texture") || part(s, "Sampler");
                });
                if (own)
                    continue;
                if (code == Code::metal && u.reg != kNoOffset)
                {
                    if (isPredefined(u.name))
                        continue;
                    why = neverSet(u.name);
                    return false;
                }
                why = neverBound(u.name);
                return false;
            }
            if (code == Code::metal)
            {
                const std::string buffer = metalBuffer(text);
                if (!buffer.empty())
                {
                    why = neverBound(buffer);
                    return false;
                }
            }
            return true;
        }

#if defined(__APPLE__)
        // The codes Metal's compiler accepted lately, oldest first (the file comment; gpu/ShaderLibrary.h "Checking it
        // off the message thread"): at most kAcceptedCodes of them and kAcceptedBytes of text, the newest always kept;
        // past either bound the code used least lately goes. The count is generous because a host may check a code long
        // before it sets it (at an editor's first open, after many instances checked theirs); the bytes bound the
        // memory (a small shader's code is a KB or so).
        constexpr std::size_t kAcceptedCodes = 256;
        constexpr std::size_t kAcceptedBytes = std::size_t{ 2 } << 20;

        struct Accepted
        {
            std::mutex              lock;
            std::deque<std::string> codes;
            std::size_t             bytes = 0;
        };

        Accepted& accepted()
        {
            static Accepted* const a = new Accepted();   // never destroyed (the file comment)
            return *a;
        }

        std::atomic<uint64_t> compiles{ 0 };             // detail::metalCompileCount()

        // Whether Metal takes this MSL text: from the record when it is there (and then the newest again), else by
        // compiling it, outside the lock, and recording it when Metal took it.
        bool metalTakes(std::string msl, std::string& why)
        {
            Accepted& a = accepted();
            {
                const std::lock_guard<std::mutex> hold(a.lock);
                const auto it = std::find(a.codes.begin(), a.codes.end(), msl);
                if (it != a.codes.end())
                {
                    std::rotate(it, it + 1, a.codes.end());
                    return true;
                }
            }
            compiles.fetch_add(1, std::memory_order_relaxed);
            if (!detail::metalCompiles(msl, why))
                return false;
            const std::lock_guard<std::mutex> hold(a.lock);
            if (std::find(a.codes.begin(), a.codes.end(), msl) == a.codes.end())
            {
                a.bytes += msl.size();
                a.codes.push_back(std::move(msl));
                while (a.codes.size() > 1 && (a.codes.size() > kAcceptedCodes || a.bytes > kAcceptedBytes))
                {
                    a.bytes -= a.codes.front().size();
                    a.codes.pop_front();
                }
            }
            return true;
        }
#endif

        // Everything set() can say about a binary without bgfx (the file comment): shaders::check(). With samplers,
        // also the binary's samplers and their stages (0.25.0).
        bool checkBinary(const uint8_t* p, std::size_t n, std::vector<Sampler>* samplers, std::string& why)
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
                const bool ours = u.name == "u_viewSize" || u.name == "u_rect" || u.name == "u_params";
                if (ours)
                {
                    // Elements: Metal's and SPIR-V's binaries count a uniform that is no array 1; Direct3D's count it
                    // 0, as its reflection does (shaderc's HLSL path writes D3D11_SHADER_TYPE_DESC::Elements). One vec4.
                    const int elements = u.num == 0 ? 1 : u.num;
                    const int most = u.name == "u_params" ? 4 : 1;
                    if (u.type != kVec4Type || elements > most)
                    {
                        why = "the shader declares " + u.name + " otherwise than FunkGui: "
                              + (u.name == "u_params" ? "uniform vec4 u_params[4];" : "uniform vec4 " + u.name + ";");
                        return false;
                    }
                    continue;
                }
                if (u.type == kSamplerType || u.type == kEndType)
                    continue;                        // samplersOf's (0.25.0)
                if (!isPredefined(u.name))
                {
                    why = neverSet(u.name);
                    return false;
                }
            }
            // The code's format first: a binary for another renderer is said to be one, whatever its samplers are, and
            // the samplers (read by this renderer's rules) before Metal compiles anything.
            std::vector<Sampler> found;
#if defined(__APPLE__)
            if (b.codeSize >= 4 && b.code[0] == 0x03 && b.code[1] == 0x02 && b.code[2] == 0x23 && b.code[3] == 0x07)
            {
                why = "the shader holds SPIR-V, not Metal: compile with --platform osx -p metal";
                return false;
            }
            if (!samplersOf(b, kCode, found, why))
                return false;
            if (!metalTakes(std::string(reinterpret_cast<const char*>(b.code), b.codeSize), why))
                return false;
#elif defined(_WIN32)
            // D3DCompile's output is a DXBC container: its four-byte magic first (the bytecode follows its checksum).
            const bool dxbc = b.codeSize >= 32 && b.code[0] == 'D' && b.code[1] == 'X' && b.code[2] == 'B'
                              && b.code[3] == 'C';
            if (!dxbc)
            {
                why = "the shader holds no Direct3D 11 bytecode: compile with --platform windows -p s_5_0";
                return false;
            }
            if (!samplersOf(b, kCode, found, why))
                return false;
#else
            const bool spirv = b.codeSize >= 20 && b.codeSize % 4 == 0 && b.code[0] == 0x03 && b.code[1] == 0x02
                               && b.code[2] == 0x23 && b.code[3] == 0x07;     // 0x07230203, little-endian
            if (!spirv)
            {
                why = "the shader holds no SPIR-V: compile with --platform linux -p spirv";
                return false;
            }
            if (!samplersOf(b, kCode, found, why))
                return false;
#endif
            if (samplers != nullptr)
                *samplers = std::move(found);
            return true;
        }

        // bgfx's sampler flags for a texture's sampling: point or linear, clamped or wrapped (bgfx's default).
        uint64_t samplerFlags(TextureSampling s)
        {
            uint64_t f = s.repeat ? 0 : (BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
            if (s.nearest)
                f |= BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT | BGFX_SAMPLER_MIP_POINT;
            return f;
        }

        // A texture's GPU texture in this run, made on first use; invalid when bgfx refused it (not retried before
        // the next run or a replacement).
        bgfx::TextureHandle upload(Texture& t)
        {
            if (bgfx::isValid(t.handle) || t.refused)
                return t.handle;
            const bgfx::TextureFormat::Enum format = t.format == TextureFormat::r8 ? bgfx::TextureFormat::R8
                                                                                   : bgfx::TextureFormat::RGBA8;
            const bgfx::Caps* caps = bgfx::getCaps();
            const bool fits = caps == nullptr
                              || (static_cast<uint32_t>(t.w) <= caps->limits.maxTextureSize
                                  && static_cast<uint32_t>(t.h) <= caps->limits.maxTextureSize
                                  && (caps->formats[format] & BGFX_CAPS_FORMAT_TEXTURE_2D) != 0);
            if (fits)
                t.handle = bgfx::createTexture2D(static_cast<uint16_t>(t.w), static_cast<uint16_t>(t.h), false, 1,
                                                 format, samplerFlags(t.sampling),
                                                 bgfx::copy(t.texels.data(), static_cast<uint32_t>(t.texels.size())));
            t.refused = !bgfx::isValid(t.handle);
            return t.handle;
        }

        // The 1 x 1 texture of zeros an unbound sampler reads, made on first use in a run.
        bgfx::TextureHandle zeros()
        {
            Registry& reg = registry();
            if (!bgfx::isValid(reg.zeros) && !reg.zerosRefused)
            {
                const uint8_t texel[4] = { 0, 0, 0, 0 };
                reg.zeros = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA8,
                                                  samplerFlags({ true, false }), bgfx::copy(texel, sizeof texel));
                reg.zerosRefused = !bgfx::isValid(reg.zeros);
            }
            return reg.zeros;
        }

        // A GPU texture let go: destroyed while bgfx runs (bgfx frees it after the frames that may sample it), else
        // already gone with bgfx (releaseGpu()).
        void letGo(bgfx::TextureHandle& h)
        {
            if (running())
                drop(h);
            h = BGFX_INVALID_HANDLE;
        }
    }

    Target target()
    {
        if (!running())
            return {};
#if defined(__APPLE__)
        return { "osx", "metal" };
#elif defined(_WIN32)
        return { "windows", "s_5_0" };
#else
        return { "linux", "spirv" };
#endif
    }

    bool check(const void* fragmentBinary, uint32_t size, std::string& why)
    {
        if (fragmentBinary == nullptr || size == 0)
        {
            why = "no shader binary";
            return false;
        }
        if (!checkBinary(static_cast<const uint8_t*>(fragmentBinary), size, nullptr, why))
            return false;
        why.clear();
        return true;
    }

    bool set(uint32_t id, const void* fragmentBinary, uint32_t size, std::string& why)
    {
        if (fragmentBinary == nullptr || size == 0)
        {
            why = "no shader binary";
            return false;
        }
        const auto* p = static_cast<const uint8_t*>(fragmentBinary);
        std::vector<Sampler> samplers;
        if (!checkBinary(p, size, &samplers, why))
            return false;

        std::vector<uint8_t> bin(p, p + size);
        Gpu gpu;
        if (running() && !build(bin, samplers, gpu, why))
            return false;                            // the id keeps what it had

        Entry* e = find(id);
        if (e == nullptr)
        {
            registry().entries.push_back({});
            e = &registry().entries.back();
            e->id = id;
        }
        if (running())
            drop(e->gpu);                            // bgfx frees them after the frame that may still use them
        e->binary = std::move(bin);
        e->samplers = std::move(samplers);
        e->gpu = std::move(gpu);
        e->refused = false;
        ++registry().generation;
        why.clear();
        return true;
    }

    void remove(uint32_t id)
    {
        Registry& reg = registry();
        std::erase_if(reg.bindings, [id](const Binding& b) { return b.shader == id; });
        for (auto it = reg.entries.begin(); it != reg.entries.end(); ++it)
            if (it->id == id)
            {
                if (running())
                    drop(it->gpu);
                reg.entries.erase(it);
                ++reg.generation;
                return;
            }
    }

    bool has(uint32_t id) { return find(id) != nullptr; }

    const char* varyingDef() { return text::varying_custom_def; }

    const char* fragmentPrelude() { return text::fs_custom_prelude; }

    const char* bgfxShaderSh() { return text::bgfx_shader_sh; }

    bool setTexture(uint32_t textureId, int w, int h, TextureFormat format, const void* texels, int strideBytes,
                    TextureSampling sampling)
    {
        if (format != TextureFormat::rgba8 && format != TextureFormat::r8)
            return false;
        const int bytesPerTexel = format == TextureFormat::r8 ? 1 : 4;
        if (w < 1 || h < 1 || w > kMaxTextureSide || h > kMaxTextureSide || texels == nullptr
            || (strideBytes != 0 && strideBytes < w * bytesPerTexel))
            return false;
        const std::size_t row = static_cast<std::size_t>(w) * static_cast<std::size_t>(bytesPerTexel);
        const std::size_t stride = strideBytes == 0 ? row : static_cast<std::size_t>(strideBytes);
        std::vector<uint8_t> packed(row * static_cast<std::size_t>(h));
        const auto* src = static_cast<const uint8_t*>(texels);
        for (std::size_t y = 0; y < static_cast<std::size_t>(h); ++y)
            std::memcpy(packed.data() + y * row, src + y * stride, row);

        Texture* t = findTexture(textureId);
        if (t == nullptr)
        {
            registry().textures.push_back({});
            t = &registry().textures.back();
            t->id = textureId;
        }
        letGo(t->handle);                            // a replacement is uploaded again at its next draw
        t->w = w;
        t->h = h;
        t->format = format;
        t->sampling = sampling;
        t->texels = std::move(packed);
        t->refused = false;
        ++registry().generation;
        return true;
    }

    void removeTexture(uint32_t textureId)
    {
        auto& textures = registry().textures;
        for (auto it = textures.begin(); it != textures.end(); ++it)
            if (it->id == textureId)
            {
                letGo(it->handle);
                textures.erase(it);
                ++registry().generation;
                return;
            }
    }

    bool hasTexture(uint32_t textureId) { return findTexture(textureId) != nullptr; }

    bool bindTexture(uint32_t shaderId, std::string_view sampler, uint32_t textureId)
    {
        Registry& reg = registry();
        if (Binding* b = findBinding(shaderId, sampler))
        {
            if (b->texture != textureId)
            {
                b->texture = textureId;
                ++reg.generation;
            }
        }
        else
        {
            reg.bindings.push_back({ shaderId, std::string(sampler), textureId });
            ++reg.generation;
        }
        const Entry* e = find(shaderId);
        return e != nullptr && std::any_of(e->samplers.begin(), e->samplers.end(),
                                           [sampler](const Sampler& s) { return s.name == sampler; });
    }

    void unbindTexture(uint32_t shaderId, std::string_view sampler)
    {
        Registry& reg = registry();
        if (std::erase_if(reg.bindings, [&](const Binding& b) { return b.shader == shaderId && b.sampler == sampler; })
            > 0)
            ++reg.generation;
    }

    namespace detail
    {
        bgfx::ProgramHandle program(uint32_t id)
        {
            Entry* e = find(id);
            if (e == nullptr || !running())
                return BGFX_INVALID_HANDLE;
            if (!bgfx::isValid(e->gpu.program) && !e->refused)
            {
                std::string why;
                e->refused = !build(e->binary, e->samplers, e->gpu, why);
            }
            return e->gpu.program;
        }

        void bindTextures(uint32_t id)
        {
            const Entry* e = find(id);
            if (e == nullptr || !running() || e->gpu.samplers.size() != e->samplers.size())
                return;
            for (std::size_t i = 0; i < e->samplers.size(); ++i)
            {
                const Sampler& s = e->samplers[i];
                bgfx::TextureHandle t = BGFX_INVALID_HANDLE;
                if (const Binding* b = findBinding(id, s.name))
                    if (Texture* tex = findTexture(b->texture))
                        t = upload(*tex);
                if (!bgfx::isValid(t))
                    t = zeros();
                bgfx::setTexture(s.stage, e->gpu.samplers[i], t);
            }
        }

        uint32_t textureCount()
        {
            const Registry& reg = registry();
            const auto n = std::count_if(reg.textures.begin(), reg.textures.end(),
                                         [](const Texture& t) { return bgfx::isValid(t.handle); });
            return static_cast<uint32_t>(n) + (bgfx::isValid(reg.zeros) ? 1u : 0u);
        }

        uint64_t generation() { return registry().generation; }

        uint32_t programCount() { return static_cast<uint32_t>(registry().entries.size()); }   // F1: gpuInfo()

        uint64_t metalCompileCount()
        {
#if defined(__APPLE__)
            return compiles.load(std::memory_order_relaxed);
#else
            return 0;
#endif
        }

        bgfx::UniformHandle uRect() { return registry().uRect; }

        bgfx::UniformHandle uParams() { return registry().uParams; }

        void releaseGpu()
        {
            Registry& reg = registry();
            for (Entry& e : reg.entries)
            {
                drop(e.gpu);
                e.refused = false;
            }
            for (Texture& t : reg.textures)
            {
                drop(t.handle);
                t.refused = false;
            }
            drop(reg.zeros);
            reg.zerosRefused = false;
            drop(reg.vs);
            drop(reg.uRect);
            drop(reg.uParams);
        }

        bool samplers(const void* binary, uint32_t size, Code code, std::vector<Sampler>& out, std::string& why)
        {
            out.clear();
            Binary b;
            if (binary == nullptr || size == 0)
            {
                why = "no shader binary";
                return false;
            }
            return parse(static_cast<const uint8_t*>(binary), size, b, why) && samplersOf(b, code, out, why);
        }
    }
}

#include <funkgui/canvas/FrameCompare.h>

#include <cstddef>
#include <cstring>

// sameDrawing (canvas/FrameCompare.h). Floats are compared by their bits through memcmp: exact, NaN-safe, and free of
// -Wfloat-equal, which a consumer's target may apply to FunkGui's sources. Prim is 84 bytes with no padding (Prim.h's
// static_assert), so a whole primitive vector is one memcmp; shader and image draws are compared field by field, which
// leaves out their tags and live flags (no sink draws them) and holds whatever their layout.

namespace funkgui
{
    namespace
    {
        template <typename T> bool sameBits(const T& a, const T& b) noexcept
        {
            return std::memcmp(&a, &b, sizeof(T)) == 0;
        }

        bool sameRect(const Rect& a, const Rect& b) noexcept
        {
            return sameBits(a.x, b.x) && sameBits(a.y, b.y) && sameBits(a.w, b.w) && sameBits(a.h, b.h);
        }

        bool sameShader(const ShaderDraw& a, const ShaderDraw& b) noexcept
        {
            return a.id == b.id && a.at == b.at && sameRect(a.rect, b.rect) && sameRect(a.clip, b.clip)
                && a.clipped == b.clipped && std::memcmp(a.params, b.params, sizeof a.params) == 0;
        }

        bool sameImage(const ImageDraw& a, const ImageDraw& b) noexcept
        {
            return a.id == b.id && a.at == b.at && a.shaderAt == b.shaderAt && sameRect(a.dst, b.dst)
                && sameRect(a.uv, b.uv) && sameBits(a.radius, b.radius) && sameBits(a.opacity, b.opacity)
                && sameRect(a.clip, b.clip) && a.clipped == b.clipped;
        }
    }

    bool sameDrawing(const PrimList& a, const PrimList& b) noexcept
    {
        const FrameInfo& x = a.info;
        const FrameInfo& y = b.info;
        if (x.logicalW != y.logicalW || x.logicalH != y.logicalH || !sameBits(x.dpi, y.dpi)
            || x.clear.r != y.clear.r || x.clear.g != y.clear.g || x.clear.b != y.clear.b || x.clear.a != y.clear.a
            || !sameBits(x.textGamma, y.textGamma))
            return false;
        if ((!a.shaders.empty() || !b.shaders.empty()) && !sameBits(x.seconds, y.seconds))
            return false;

        if (a.prims.size() != b.prims.size() || a.shaders.size() != b.shaders.size()
            || a.images.size() != b.images.size())
            return false;
        if (!a.prims.empty() && std::memcmp(a.prims.data(), b.prims.data(), a.prims.size() * sizeof(Prim)) != 0)
            return false;
        for (std::size_t i = 0; i < a.shaders.size(); ++i)
            if (!sameShader(a.shaders[i], b.shaders[i]))
                return false;
        for (std::size_t i = 0; i < a.images.size(); ++i)
            if (!sameImage(a.images[i], b.images[i]))
                return false;
        return true;
    }
}

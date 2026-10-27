#include <funkgui/canvas/Images.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

// The picture registry (v0.20.0; canvas/Images.h has the contract). set() builds the whole Picture, the premultiplied
// level chain included, before it takes the lock, so a reader on another thread waits only for a pointer swap. The
// table is a short vector searched linearly: a host registers a handful of pictures (a chat's attachments), not
// thousands. Never destroyed (the header says why); the function-local pointer is constant-initialised on first use,
// thread-safely, as the tag and shader registries are.

namespace funkgui::images
{
    namespace
    {
        struct Registry
        {
            std::mutex                                   lock;
            std::vector<std::shared_ptr<const Picture>> pictures;   // at most one per id, in no particular order
            uint64_t                                     nextRevision = 1;
            std::atomic<uint64_t>                        generation{ 1 };
        };

        Registry& registry()
        {
            static Registry* const r = new Registry();   // never destroyed (the file comment)
            return *r;
        }

        // Under the lock: where id's picture is in the table, or end.
        std::vector<std::shared_ptr<const Picture>>::iterator find(Registry& reg, uint32_t id)
        {
            return std::find_if(reg.pictures.begin(), reg.pictures.end(),
                                [id](const std::shared_ptr<const Picture>& p) { return p->id == id; });
        }

        // The mean of n texel values (n = 1, 2 or 4), to the nearest.
        uint8_t mean(unsigned sum, unsigned n) noexcept { return static_cast<uint8_t>((sum + n / 2u) / n); }
    }

    int levelCount(int w, int h) noexcept
    {
        if (w < 1 || h < 1)
            return 0;
        int n = 1;
        for (int side = std::max(w, h); side > 1; side >>= 1)
            ++n;
        return n;
    }

    bool set(uint32_t id, int w, int h, const uint8_t* rgba, int strideBytes)
    {
        if (rgba == nullptr || w < 1 || h < 1 || w > kMaxSide || h > kMaxSide)
            return false;
        const std::size_t row = static_cast<std::size_t>(w) * 4u;
        if (strideBytes != 0 && (strideBytes < 0 || static_cast<std::size_t>(strideBytes) < row))
            return false;
        const std::size_t stride = strideBytes == 0 ? row : static_cast<std::size_t>(strideBytes);

        auto pic = std::make_shared<Picture>();
        pic->id = id;
        pic->w = w;
        pic->h = h;
        const int n = levelCount(w, h);
        pic->levels.resize(static_cast<std::size_t>(n));
        std::size_t total = 0;
        for (int l = 0; l < n; ++l)
        {
            Level& lv = pic->levels[static_cast<std::size_t>(l)];
            lv.w = std::max(1, w >> l);
            lv.h = std::max(1, h >> l);
            lv.offset = total;
            total += static_cast<std::size_t>(lv.w) * static_cast<std::size_t>(lv.h) * 4u;
        }
        pic->pixels.resize(total);

        // Level 0: the caller's straight texels, premultiplied.
        uint8_t* base = pic->pixels.data();
        for (int y = 0; y < h; ++y)
        {
            const uint8_t* s = rgba + static_cast<std::size_t>(y) * stride;
            uint8_t* d = base + static_cast<std::size_t>(y) * row;
            for (int x = 0; x < w; ++x, s += 4, d += 4)
            {
                const unsigned a = s[3];
                for (int c = 0; c < 3; ++c)
                    d[c] = static_cast<uint8_t>((static_cast<unsigned>(s[c]) * a + 127u) / 255u);
                d[3] = s[3];
            }
        }

        // Each further level: the box filter of the one above (Images.h).
        for (int l = 1; l < n; ++l)
        {
            const Level& up = pic->levels[static_cast<std::size_t>(l - 1)];
            const Level& lv = pic->levels[static_cast<std::size_t>(l)];
            const uint8_t* src = base + up.offset;
            uint8_t* dst = base + lv.offset;
            const std::size_t upRow = static_cast<std::size_t>(up.w) * 4u;
            const auto texel = [src, upRow](int x, int y) noexcept
            { return src + static_cast<std::size_t>(y) * upRow + static_cast<std::size_t>(x) * 4u; };
            for (int y = 0; y < lv.h; ++y)
            {
                const int y0 = std::min(2 * y, up.h - 1), y1 = std::min(2 * y + 1, up.h - 1);
                for (int x = 0; x < lv.w; ++x)
                {
                    // Distinct texels only: a level 1 texel wide or high averages two of them, not one counted twice.
                    const int x0 = std::min(2 * x, up.w - 1), x1 = std::min(2 * x + 1, up.w - 1);
                    const unsigned nx = x1 != x0 ? 2u : 1u, ny = y1 != y0 ? 2u : 1u;
                    uint8_t* d = dst + (static_cast<std::size_t>(y) * static_cast<std::size_t>(lv.w)
                                        + static_cast<std::size_t>(x)) * 4u;
                    for (int c = 0; c < 4; ++c)
                    {
                        unsigned sum = static_cast<unsigned>(texel(x0, y0)[c]);
                        if (nx == 2u)
                            sum += static_cast<unsigned>(texel(x1, y0)[c]);
                        if (ny == 2u)
                            sum += static_cast<unsigned>(texel(x0, y1)[c]);
                        if (nx == 2u && ny == 2u)
                            sum += static_cast<unsigned>(texel(x1, y1)[c]);
                        d[c] = mean(sum, nx * ny);
                    }
                }
            }
        }

        Registry& reg = registry();
        std::shared_ptr<const Picture> old;          // released after the lock: the last owner frees it there
        {
            const std::lock_guard<std::mutex> guard(reg.lock);
            pic->revision = reg.nextRevision++;
            std::shared_ptr<const Picture> stored = std::move(pic);
            const auto it = find(reg, id);
            if (it == reg.pictures.end())
                reg.pictures.push_back(std::move(stored));
            else
            {
                old = std::move(*it);
                *it = std::move(stored);
            }
            reg.generation.fetch_add(1, std::memory_order_release);
        }
        return true;
    }

    void remove(uint32_t id)
    {
        Registry& reg = registry();
        std::shared_ptr<const Picture> old;
        {
            const std::lock_guard<std::mutex> guard(reg.lock);
            const auto it = find(reg, id);
            if (it == reg.pictures.end())
                return;
            old = std::move(*it);
            reg.pictures.erase(it);
            reg.generation.fetch_add(1, std::memory_order_release);
        }
    }

    std::shared_ptr<const Picture> picture(uint32_t id)
    {
        Registry& reg = registry();
        const std::lock_guard<std::mutex> guard(reg.lock);
        const auto it = find(reg, id);
        return it == reg.pictures.end() ? nullptr : *it;
    }

    bool has(uint32_t id) { return picture(id) != nullptr; }

    int width(uint32_t id)
    {
        const std::shared_ptr<const Picture> p = picture(id);
        return p ? p->w : 0;
    }

    int height(uint32_t id)
    {
        const std::shared_ptr<const Picture> p = picture(id);
        return p ? p->h : 0;
    }

    uint64_t generation() { return registry().generation.load(std::memory_order_acquire); }
}

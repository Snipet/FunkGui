// FUNKGUI_TEST name=fg.image.registry timeout=300 gpu=0
//
// fg.image.registry (v0.20.0; canvas/Images.h): the process-wide picture registry Canvas::image draws from. Plain C++
// with no GPU: every configuration, natively and under node (the thread rows natively only: wasm32 here has no
// threads). Spec rows only.
//
// - set.*: what is refused (a null buffer, a size outside 1..4096, a stride below w * 4 or negative) changes nothing
//   (not has(), the generation unchanged); a stored picture's size, its texels premultiplied ((c * a + 127) / 255,
//   alpha kept, an opaque texel unchanged), a stride's padding ignored, the caller's buffer copied, the largest size
//   taken.
// - replace.*: set() on a held id makes a new revision (larger, never reused), moves the generation, takes the new
//   size, and leaves a picture already handed out exactly as it was.
// - remove.*: has() false, width/height 0, picture() null, the generation moved; an unknown id changes nothing.
// - levels.*: levelCount for 1 x 1, 4096 x 4096, 5 x 1 and the sizes that are not pictures; the chain's sizes and
//   offsets (back to back, bgfx's mip layout) and each level the rounded box filter of the one above, odd edges
//   dropped, one-texel-wide levels averaging two texels, and premultiplied (no colour bleeds from a transparent texel).
// - threads.*: a reader on another thread taking picture() while the main thread replaces and removes the id only
//   ever sees whole pictures (its levels match its size, its texels its revision's pattern).

#include <funkgui/canvas/Images.h>
#include <funkgui/test/Harness.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#if !defined(__EMSCRIPTEN__)
  #include <thread>
#endif

namespace T = funkgui::test;
namespace images = funkgui::images;

namespace
{
    // w x h straight RGBA texels from f(x, y, c), tightly packed.
    template <typename F> std::vector<uint8_t> texels(int w, int h, F f)
    {
        std::vector<uint8_t> v(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                for (int c = 0; c < 4; ++c)
                    v[(static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 4u
                      + static_cast<std::size_t>(c)] = static_cast<uint8_t>(f(x, y, c));
        return v;
    }

    const uint8_t* texel(const images::Picture& p, int level, int x, int y)
    {
        const images::Level& lv = p.levels[static_cast<std::size_t>(level)];
        return p.level(level) + (static_cast<std::size_t>(y) * static_cast<std::size_t>(lv.w)
                                 + static_cast<std::size_t>(x)) * 4u;
    }

    bool texelIs(const images::Picture& p, int level, int x, int y, int r, int g, int b, int a)
    {
        const uint8_t* t = texel(p, level, x, y);
        const bool ok = t[0] == r && t[1] == g && t[2] == b && t[3] == a;
        if (!ok)
            std::printf("INFO     level %d texel (%d, %d) = %u %u %u %u, want %d %d %d %d\n", level, x, y, t[0], t[1],
                        t[2], t[3], r, g, b, a);
        return ok;
    }

    // The chain's shape: levelCount levels, each max(1, side >> l), back to back.
    bool shaped(const images::Picture& p)
    {
        const int n = images::levelCount(p.w, p.h);
        if (static_cast<int>(p.levels.size()) != n || n < 1)
            return false;
        std::size_t at = 0;
        for (int l = 0; l < n; ++l)
        {
            const images::Level& lv = p.levels[static_cast<std::size_t>(l)];
            const int w = p.w >> l > 1 ? p.w >> l : 1, h = p.h >> l > 1 ? p.h >> l : 1;
            if (lv.w != w || lv.h != h || lv.offset != at)
                return false;
            at += static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u;
        }
        return at == p.pixels.size() && p.levels.back().w == 1 && p.levels.back().h == 1;
    }
}

int main(int argc, char** argv)
{
    T::Probe P("fg.image.registry", "", argc, argv);

    // ---- set: refusals ----------------------------------------------------------------------------------------------
    {
        const std::vector<uint8_t> px = texels(8, 8, [](int, int, int) { return 200; });
        const uint64_t g0 = images::generation();
        P.eq("set.generation_not_0", g0 != 0, 1);
        P.eq("set.refuses_null", images::set(1, 8, 8, nullptr), 0);
        P.eq("set.refuses_w0", images::set(1, 0, 8, px.data()), 0);
        P.eq("set.refuses_h_negative", images::set(1, 8, -1, px.data()), 0);
        P.eq("set.refuses_w_4097", images::set(1, 4097, 1, px.data()), 0);
        P.eq("set.refuses_h_4097", images::set(1, 1, 4097, px.data()), 0);
        P.eq("set.refuses_small_stride", images::set(1, 8, 8, px.data(), 31), 0);
        P.eq("set.refuses_negative_stride", images::set(1, 8, 8, px.data(), -32), 0);
        P.eq("set.refusals_store_nothing", !images::has(1) && images::width(1) == 0 && images::height(1) == 0
                                               && images::picture(1) == nullptr, 1);
        P.eq("set.refusals_keep_generation", images::generation() == g0, 1);
    }

    // ---- set: what is stored ----------------------------------------------------------------------------------------
    {
        // r = 10 x, g = 10 y, b = 255, a = 255 except texel (1, 0): a = 128 and (2, 0): a = 0.
        const std::vector<uint8_t> px = texels(3, 2, [](int x, int y, int c) {
            if (c == 3)
                return y == 0 && x == 1 ? 128 : (y == 0 && x == 2 ? 0 : 255);
            return c == 0 ? 10 * x + 100 : (c == 1 ? 10 * y + 50 : 255);
        });
        const uint64_t g0 = images::generation();
        P.eq("set.stores", images::set(2, 3, 2, px.data()), 1);
        P.eq("set.generation_moved", images::generation() != g0, 1);
        P.eq("set.size", images::has(2) && images::width(2) == 3 && images::height(2) == 2, 1);
        const std::shared_ptr<const images::Picture> p = images::picture(2);
        if (P.eq("set.picture", p != nullptr && p->id == 2 && p->w == 3 && p->h == 2, 1))
        {
            P.eq("set.levels_shaped", shaped(*p), 1);
            P.eq("set.opaque_unchanged", texelIs(*p, 0, 0, 0, 100, 50, 255, 255)
                                             && texelIs(*p, 0, 2, 1, 120, 60, 255, 255), 1);
            // (110 * 128 + 127) / 255 = 55, (50 * 128 + 127) / 255 = 25, (255 * 128 + 127) / 255 = 128
            P.eq("set.premultiplied", texelIs(*p, 0, 1, 0, 55, 25, 128, 128), 1);
            P.eq("set.transparent_is_0", texelIs(*p, 0, 2, 0, 0, 0, 0, 0), 1);
        }

        // A stride with padding full of junk: the same texels as the packed buffer.
        std::vector<uint8_t> padded(static_cast<std::size_t>(2) * 20u, 0xEE);
        for (int y = 0; y < 2; ++y)
            for (int i = 0; i < 12; ++i)
                padded[static_cast<std::size_t>(y) * 20u + static_cast<std::size_t>(i)] =
                    px[static_cast<std::size_t>(y) * 12u + static_cast<std::size_t>(i)];
        P.eq("set.stride", images::set(3, 3, 2, padded.data(), 20), 1);
        const std::shared_ptr<const images::Picture> q = images::picture(3);
        P.eq("set.stride_ignores_padding", q != nullptr && p != nullptr && q->pixels == p->pixels, 1);
        P.eq("set.stride_exact_row", images::set(4, 3, 2, px.data(), 12), 1);

        // The caller's buffer is copied: changing it afterwards changes nothing.
        std::vector<uint8_t> mine = px;
        images::set(5, 3, 2, mine.data());
        mine.assign(mine.size(), 0);
        const std::shared_ptr<const images::Picture> c = images::picture(5);
        P.eq("set.copies", c != nullptr && texelIs(*c, 0, 0, 0, 100, 50, 255, 255), 1);

        const std::vector<uint8_t> wide(static_cast<std::size_t>(images::kMaxSide) * 4u, 255);
        P.eq("set.largest_side", images::set(6, images::kMaxSide, 1, wide.data())
                                     && images::width(6) == images::kMaxSide && images::height(6) == 1, 1);
        for (uint32_t id = 2; id <= 6; ++id)
            images::remove(id);
    }

    // ---- replace and remove -----------------------------------------------------------------------------------------
    {
        const std::vector<uint8_t> red = texels(4, 4, [](int, int, int c) { return c == 0 || c == 3 ? 255 : 0; });
        const std::vector<uint8_t> blue = texels(2, 6, [](int, int, int c) { return c == 2 || c == 3 ? 255 : 0; });
        images::set(10, 4, 4, red.data());
        const std::shared_ptr<const images::Picture> first = images::picture(10);
        const uint64_t g0 = images::generation();
        P.eq("replace.stores", images::set(10, 2, 6, blue.data()), 1);
        const std::shared_ptr<const images::Picture> second = images::picture(10);
        const bool both = first != nullptr && second != nullptr;
        P.eq("replace.new_revision", both && second->revision > first->revision, 1);
        P.eq("replace.generation_moved", images::generation() != g0, 1);
        P.eq("replace.new_size", images::width(10) == 2 && images::height(10) == 6, 1);
        P.eq("replace.held_unchanged", both && first->w == 4 && first->h == 4 && shaped(*first)
                                           && texelIs(*first, 0, 3, 3, 255, 0, 0, 255), 1);
        P.eq("replace.new_texels", both && texelIs(*second, 0, 1, 5, 0, 0, 255, 255), 1);
        images::set(11, 4, 4, red.data());
        const std::shared_ptr<const images::Picture> other = images::picture(11);
        P.eq("replace.revisions_unique", other != nullptr && both && other->revision > second->revision, 1);

        const uint64_t g1 = images::generation();
        images::remove(10);
        P.eq("remove.gone", !images::has(10) && images::width(10) == 0 && images::height(10) == 0
                                && images::picture(10) == nullptr, 1);
        P.eq("remove.generation_moved", images::generation() != g1, 1);
        P.eq("remove.held_still_valid", both && second->w == 2 && texelIs(*second, 0, 0, 0, 0, 0, 255, 255), 1);
        P.eq("remove.others_kept", images::has(11), 1);
        const uint64_t g2 = images::generation();
        images::remove(10);
        images::remove(12345);
        P.eq("remove.unknown_ignored", images::generation() == g2, 1);
        images::remove(11);
    }

    // ---- levels -----------------------------------------------------------------------------------------------------
    {
        P.eq("levels.count_1x1", images::levelCount(1, 1), 1);
        P.eq("levels.count_4096", images::levelCount(4096, 4096), 13);
        P.eq("levels.count_5x1", images::levelCount(5, 1), 3);
        P.eq("levels.count_1x8", images::levelCount(1, 8), 4);
        P.eq("levels.count_not_a_picture", images::levelCount(0, 3) == 0 && images::levelCount(3, -1) == 0, 1);

        // 2 x 2 -> 1 x 1: the rounded mean of the four (premultiplied: all opaque here).
        const int v2[4][3] = { { 10, 20, 30 }, { 11, 21, 31 }, { 12, 22, 33 }, { 14, 24, 35 } };
        const std::vector<uint8_t> quad = texels(2, 2, [&v2](int x, int y, int c) {
            return c == 3 ? 255 : v2[y * 2 + x][c];
        });
        images::set(20, 2, 2, quad.data());
        const std::shared_ptr<const images::Picture> a = images::picture(20);
        // (10 + 11 + 12 + 14 + 2) / 4 = 12, (20 + 21 + 22 + 24 + 2) / 4 = 22, (30 + 31 + 33 + 35 + 2) / 4 = 32
        P.eq("levels.box_mean", a != nullptr && shaped(*a) && texelIs(*a, 1, 0, 0, 12, 22, 32, 255), 1);

        // 3 x 1 -> 1 x 1: texels 0 and 1 only (halving drops the odd last column).
        const std::vector<uint8_t> row = texels(3, 1, [](int x, int, int c) {
            return c == 3 ? 255 : (x == 0 ? 100 : (x == 1 ? 51 : 255));
        });
        images::set(21, 3, 1, row.data());
        const std::shared_ptr<const images::Picture> b = images::picture(21);
        P.eq("levels.odd_edge_dropped", b != nullptr && shaped(*b) && b->levels.size() == 2
                                            && texelIs(*b, 1, 0, 0, 76, 76, 76, 255), 1);   // (100 + 51 + 1) / 2

        // 1 x 4 -> 1 x 2 -> 1 x 1: a level one texel wide averages two texels, not one counted twice.
        const std::vector<uint8_t> column = texels(1, 4, [](int, int y, int c) { return c == 3 ? 255 : 40 * y; });
        images::set(22, 1, 4, column.data());
        const std::shared_ptr<const images::Picture> d = images::picture(22);
        P.eq("levels.one_wide", d != nullptr && shaped(*d) && texelIs(*d, 1, 0, 0, 20, 20, 20, 255)
                                    && texelIs(*d, 1, 0, 1, 100, 100, 100, 255)
                                    && texelIs(*d, 2, 0, 0, 60, 60, 60, 255), 1);

        // A transparent red texel beside an opaque blue one: premultiplied, the mean has no red in it.
        const std::vector<uint8_t> pair = texels(2, 1, [](int x, int, int c) {
            if (x == 0)
                return c == 0 ? 255 : 0;                                   // red, alpha 0
            return c == 2 || c == 3 ? 255 : 0;                            // blue, opaque
        });
        images::set(23, 2, 1, pair.data());
        const std::shared_ptr<const images::Picture> e = images::picture(23);
        P.eq("levels.premultiplied_no_bleed", e != nullptr && texelIs(*e, 0, 0, 0, 0, 0, 0, 0)
                                                  && texelIs(*e, 1, 0, 0, 0, 0, 128, 128), 1);

        // A large picture's chain: 13 levels down to 1 x 1, back to back.
        const std::vector<uint8_t> big(static_cast<std::size_t>(300) * 200u * 4u, 128);
        images::set(24, 300, 200, big.data());
        const std::shared_ptr<const images::Picture> f = images::picture(24);
        P.eq("levels.chain_300x200", f != nullptr && shaped(*f) && f->levels.size() == 9
                                         && f->levels[1].w == 150 && f->levels[1].h == 100
                                         && f->levels[8].w == 1 && f->levels[8].h == 1, 1);
        for (uint32_t id = 20; id <= 24; ++id)
            images::remove(id);
    }

#if !defined(__EMSCRIPTEN__)
    // ---- threads: whole pictures only -------------------------------------------------------------------------------
    {
        // Picture k is (k % 7 + 1) x (k % 5 + 1) texels, every texel (k & 255, 0, 0, 255).
        std::atomic<bool> stop{ false };
        std::atomic<int> seen{ 0 }, broken{ 0 };
        std::thread reader([&]() {
            while (!stop.load())
            {
                const std::shared_ptr<const images::Picture> p = images::picture(30);
                if (p == nullptr)
                    continue;
                ++seen;
                const uint8_t r = p->pixels.empty() ? 0 : p->pixels[0];
                bool ok = shaped(*p);
                for (std::size_t i = 0; ok && i < p->levels[0].offset + static_cast<std::size_t>(p->w * p->h) * 4u;
                     i += 4)
                    ok = p->pixels[i] == r && p->pixels[i + 3] == 255;
                if (!ok)
                    ++broken;
            }
        });
        for (int k = 0; k < 400; ++k)
        {
            const int w = k % 7 + 1, h = k % 5 + 1;
            const std::vector<uint8_t> px = texels(w, h, [k](int, int, int c) {
                return c == 0 ? (k & 255) : (c == 3 ? 255 : 0);
            });
            images::set(30, w, h, px.data());
            if (k % 50 == 49)
                images::remove(30);
        }
        stop.store(true);
        reader.join();
        std::printf("INFO     the reader saw %d pictures\n", seen.load());
        P.eq("threads.whole_pictures", broken.load(), 0);
        images::remove(30);
    }
#endif

    return P.finish();
}

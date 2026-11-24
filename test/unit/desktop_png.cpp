// FUNKGUI_TEST name=fg.desktop.png timeout=120 gpu=0
//
// fg.desktop.png (v0.28.0): funkgui::writePng, the PNG a desktop host's screenshot and HeadlessHost::writePng write.
// Without JUCE the encoder is FunkGui's own (src/nojuce/WritePng.cpp: the pixels in stored deflate blocks, CRC-32 and
// Adler-32), and this test holds it to a known file and to the formats' rules, each checked here with code of the
// test's own (a bitwise CRC-32, a plain Adler-32, a reader of stored blocks):
//   known.*    a 3 x 2 image's file, byte for byte, against the bytes Python's zlib and struct made for the same
//              pixels (the array below)
//   shape.*    the signature, IHDR (the size, 8 bits, colour type 6, no interlace), one IDAT, IEND, each chunk's CRC
//   stored.*   a 211 x 97 image, whose scanlines fill more than one stored block: every block's length and its
//              complement, the last one marked final, the blocks' bytes the scanlines (filter 0 before each row, the
//              pixels as given, alpha straight), the Adler-32 of them at the end
//   refused.*  an empty image, a pixel buffer of the wrong size, a path in a folder that does not exist
//   partial.*  nothing is left beside the file
// With JUCE the encoder is JUCE's (src/juce/WritePng.cpp), whose bytes are its own: only shape.signature, shape.ihdr
// and the refusals are checked there, and a note says so. Spec rows only.

#include <funkgui/canvas/SoftRaster.h>
#include <funkgui/core/HasJuce.h>
#include <funkgui/test/Harness.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace T = funkgui::test;
using funkgui::Image;

namespace
{
#if !FUNKGUI_HAS_JUCE
    // The known file: 3 x 2 px, rows (255,0,0,255) (0,255,0,128) (0,0,255,0) and (1,2,3,4) (250,251,252,253)
    // (128,64,32,16).
    constexpr uint8_t kKnown[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
        0x00, 0x03, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x9d, 0x74, 0x66, 0x1a, 0x00, 0x00, 0x00,
        0x25, 0x49, 0x44, 0x41, 0x54, 0x78, 0x01, 0x01, 0x1a, 0x00, 0xe5, 0xff, 0x00, 0xff, 0x00, 0x00, 0xff, 0x00,
        0xff, 0x00, 0x80, 0x00, 0x00, 0xff, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0xfa, 0xfb, 0xfc, 0xfd, 0x80, 0x40,
        0x20, 0x10, 0x77, 0xc2, 0x09, 0x65, 0x7c, 0xb3, 0xe5, 0x5b, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44,
        0xae, 0x42, 0x60, 0x82,
    };
#endif

    std::vector<uint8_t> readAll(const char* path)
    {
        std::vector<uint8_t> bytes;
        if (std::FILE* f = std::fopen(path, "rb"))
        {
            uint8_t buf[4096];
            for (std::size_t n = std::fread(buf, 1, sizeof buf, f); n > 0; n = std::fread(buf, 1, sizeof buf, f))
                bytes.insert(bytes.end(), buf, buf + n);
            std::fclose(f);
        }
        return bytes;
    }

    bool exists(const char* path)
    {
        if (std::FILE* f = std::fopen(path, "rb"))
        {
            std::fclose(f);
            return true;
        }
        return false;
    }

    uint32_t be32(const std::vector<uint8_t>& b, std::size_t at)
    {
        return (static_cast<uint32_t>(b[at]) << 24) | (static_cast<uint32_t>(b[at + 1]) << 16)
             | (static_cast<uint32_t>(b[at + 2]) << 8) | static_cast<uint32_t>(b[at + 3]);
    }

    // CRC-32 bit by bit (no table), over [at, at + n).
    uint32_t crcOf(const std::vector<uint8_t>& b, std::size_t at, std::size_t n)
    {
        uint32_t crc = 0xffffffffu;
        for (std::size_t i = 0; i < n; ++i)
        {
            crc ^= b[at + i];
            for (int k = 0; k < 8; ++k)
                crc = (crc & 1u) != 0u ? (crc >> 1) ^ 0xedb88320u : crc >> 1;
        }
        return ~crc;
    }

#if !FUNKGUI_HAS_JUCE
    // Adler-32, reduced at every byte.
    uint32_t adlerOf(const std::vector<uint8_t>& b)
    {
        uint32_t a = 1u, s = 0u;
        for (const uint8_t v : b)
        {
            a = (a + v) % 65521u;
            s = (s + a) % 65521u;
        }
        return (s << 16) | a;
    }

    struct Chunk
    {
        std::string type;
        std::size_t data = 0, size = 0;
        bool crcOk = false;
    };

    // The chunks after the signature; empty when the file is cut short.
    std::vector<Chunk> chunksOf(const std::vector<uint8_t>& png)
    {
        std::vector<Chunk> out;
        std::size_t at = 8;
        while (at + 12 <= png.size())
        {
            Chunk c;
            c.size = be32(png, at);
            if (at + 12 + c.size > png.size())
                return {};
            c.type.assign(png.begin() + static_cast<std::ptrdiff_t>(at + 4),
                          png.begin() + static_cast<std::ptrdiff_t>(at + 8));
            c.data = at + 8;
            c.crcOk = crcOf(png, at + 4, 4 + c.size) == be32(png, at + 8 + c.size);
            out.push_back(c);
            at += 12 + c.size;
        }
        return at == png.size() ? out : std::vector<Chunk>{};
    }

    Image gradient(int w, int h)
    {
        Image img;
        img.w = w;
        img.h = h;
        img.rgba.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u);
        for (std::size_t i = 0; i < img.rgba.size(); ++i)
            img.rgba[i] = static_cast<uint8_t>((i * 37u + (i >> 9) * 11u + 5u) & 0xffu);
        return img;
    }
#endif
}

int main(int argc, char** argv)
{
    T::Probe P("fg.desktop.png", "", argc, argv);
    constexpr uint8_t kSignature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };

    Image small;
    small.w = 3;
    small.h = 2;
    small.rgba = { 255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 0, 1, 2, 3, 4, 250, 251, 252, 253, 128, 64, 32, 16 };

    // ---- refusals (either encoder) ----------------------------------------------------------------------------------
    P.eq("refused.empty_image", funkgui::writePng(Image{}, "empty.png"), 0);
    {
        Image bad = small;
        bad.rgba.pop_back();
        P.eq("refused.wrong_size", funkgui::writePng(bad, "bad.png"), 0);
    }
    P.eq("refused.no_such_folder", funkgui::writePng(small, "no-such-folder/x.png"), 0);
    P.eq("refused.null_path", funkgui::writePng(small, nullptr), 0);

    // ---- the small image --------------------------------------------------------------------------------------------
    P.eq("known.written", funkgui::writePng(small, "known.png"), 1);
    P.eq("known.written_again_over_it", funkgui::writePng(small, "known.png"), 1);
    const std::vector<uint8_t> file = readAll("known.png");
    bool signature = file.size() >= 8;
    for (std::size_t i = 0; signature && i < 8; ++i)
        signature = file[i] == kSignature[i];
    P.eq("shape.signature", signature, 1);
    const bool ihdr = file.size() >= 33 && be32(file, 8) == 13u && file[12] == 'I' && file[13] == 'H'
                   && file[14] == 'D' && file[15] == 'R' && be32(file, 16) == 3u && be32(file, 20) == 2u
                   && file[24] == 8 && file[25] == 6 && file[26] == 0 && file[27] == 0 && file[28] == 0
                   && crcOf(file, 12, 17) == be32(file, 29);
    P.eq("shape.ihdr", ihdr, 1);
    P.eq("partial.none_left", exists("known.png.partial"), 0);

#if FUNKGUI_HAS_JUCE
    std::printf("NOTE     a JUCE build: the encoder is JUCE's; the known file and the stored blocks are not checked\n");
#else
    bool same = file.size() == sizeof kKnown;
    for (std::size_t i = 0; same && i < file.size(); ++i)
        same = file[i] == kKnown[i];
    P.eq("known.bytes", same, 1);
    P.eq("known.size", static_cast<int64_t>(file.size()), static_cast<int64_t>(sizeof kKnown));

    // ---- an image of more than one stored block ---------------------------------------------------------------------
    const Image big = gradient(211, 97);                         // 97 rows of 845 bytes: 81,965 bytes, two blocks
    P.eq("stored.written", funkgui::writePng(big, "big.png"), 1);
    const std::vector<uint8_t> png = readAll("big.png");
    const std::vector<Chunk> chunks = chunksOf(png);
    P.eq("shape.chunks", chunks.size() == 3 && chunks[0].type == "IHDR" && chunks[1].type == "IDAT"
                             && chunks[2].type == "IEND" && chunks[2].size == 0, 1);
    bool crcs = !chunks.empty();
    for (const Chunk& c : chunks)
        crcs = crcs && c.crcOk;
    P.eq("shape.crcs", crcs, 1);
    P.eq("partial.none_left_big", exists("big.png.partial"), 0);

    std::vector<uint8_t> raw;
    bool blocksOk = chunks.size() == 3 && chunks[1].size >= 6;
    int blocks = 0;
    bool finalLast = false, finalEarly = false;
    std::size_t at = 0, end = 0;
    if (blocksOk)
    {
        at = chunks[1].data;
        end = at + chunks[1].size;
        blocksOk = png[at] == 0x78 && ((static_cast<unsigned>(png[at]) << 8) | png[at + 1]) % 31u == 0u
                && (png[at + 1] & 0x20u) == 0u;          // deflate, a valid check, no preset dictionary
        at += 2;
        while (blocksOk && at + 5 <= end - 4 && !finalLast)
        {
            const uint8_t head = png[at];
            const std::size_t len = png[at + 1] | (static_cast<std::size_t>(png[at + 2]) << 8);
            const std::size_t nlen = png[at + 3] | (static_cast<std::size_t>(png[at + 4]) << 8);
            blocksOk = (head & 0xfeu) == 0u && (len ^ nlen) == 0xffffu && at + 5 + len <= end - 4;
            if (!blocksOk)
                break;
            raw.insert(raw.end(), png.begin() + static_cast<std::ptrdiff_t>(at + 5),
                       png.begin() + static_cast<std::ptrdiff_t>(at + 5 + len));
            at += 5 + len;
            ++blocks;
            finalLast = (head & 1u) != 0u;
            finalEarly = finalEarly || (finalLast && at != end - 4);
        }
    }
    P.eq("stored.blocks_well_formed", blocksOk, 1);
    P.eq("stored.block_count", blocks, 2);
    P.eq("stored.final_is_last", finalLast && !finalEarly, 1);
    const std::size_t rowBytes = 211u * 4u + 1u;
    bool rows = raw.size() == rowBytes * 97u;
    for (std::size_t y = 0; rows && y < 97u; ++y)
    {
        rows = raw[y * rowBytes] == 0u;
        for (std::size_t i = 0; rows && i < 211u * 4u; ++i)
            rows = raw[y * rowBytes + 1u + i] == big.rgba[y * 211u * 4u + i];
    }
    P.eq("stored.scanlines_are_the_pixels", rows, 1);
    P.eq("stored.adler32", blocksOk && at + 4 == end && adlerOf(raw) == be32(png, at), 1);
#endif
    return P.finish();
}

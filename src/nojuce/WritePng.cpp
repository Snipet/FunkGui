#include <funkgui/canvas/SoftRaster.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// funkgui::writePng without JUCE (v0.28.0; before, this build could write no PNG and answered false). The encoder is
// FunkGui's own and needs no library: an 8-bit RGBA PNG (colour type 6, no interlace) whose image data is one zlib
// stream of stored deflate blocks, that is, the pixels uncompressed, each scanline behind filter type 0. Stored blocks
// need only the two checksums of the formats: CRC-32 over each chunk (PNG, ISO 3309) and Adler-32 over the scanlines
// (zlib, RFC 1950). The file is as large as the pixels (4 bytes a pixel and a little more): right for a test's
// picture or a screenshot, and any PNG reader takes it. The alpha is straight, as Image holds it and PNG stores it.
//
// As src/juce/WritePng.cpp: false for an empty or ill-sized image or a path that cannot be written, and the file is
// written beside its place and renamed onto it once complete, so a reader never sees half a file.
// Compiled only with FUNKGUI_WITH_JUCE=OFF (src/nojuce/**).

namespace funkgui
{
    namespace
    {
        // CRC-32 (the reflected polynomial 0xedb88320), by a table made once at compile time.
        constexpr std::array<uint32_t, 256> makeCrcTable() noexcept
        {
            std::array<uint32_t, 256> table{};
            for (uint32_t n = 0; n < 256u; ++n)
            {
                uint32_t c = n;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1u) != 0u ? 0xedb88320u ^ (c >> 1) : c >> 1;
                table[n] = c;
            }
            return table;
        }

        inline constexpr std::array<uint32_t, 256> kCrcTable = makeCrcTable();

        uint32_t crc32(uint32_t crc, const uint8_t* data, size_t size) noexcept
        {
            crc = ~crc;
            for (size_t i = 0; i < size; ++i)
                crc = kCrcTable[(crc ^ data[i]) & 0xffu] ^ (crc >> 8);
            return ~crc;
        }

        // Adler-32, the sums reduced every 5552 bytes (the most that cannot overflow 32 bits).
        uint32_t adler32(uint32_t adler, const uint8_t* data, size_t size) noexcept
        {
            constexpr uint32_t kMod = 65521u;
            constexpr size_t kRun = 5552u;
            uint32_t a = adler & 0xffffu, b = adler >> 16;
            while (size > 0u)
            {
                const size_t n = size < kRun ? size : kRun;
                for (size_t i = 0; i < n; ++i)
                {
                    a += data[i];
                    b += a;
                }
                a %= kMod;
                b %= kMod;
                data += n;
                size -= n;
            }
            return (b << 16) | a;
        }

        void put32(std::vector<uint8_t>& out, uint32_t v)
        {
            out.push_back(static_cast<uint8_t>(v >> 24));
            out.push_back(static_cast<uint8_t>(v >> 16));
            out.push_back(static_cast<uint8_t>(v >> 8));
            out.push_back(static_cast<uint8_t>(v));
        }

        // One chunk: its length, its type and data, and the CRC of type and data.
        void putChunk(std::vector<uint8_t>& out, const char type[4], const std::vector<uint8_t>& data)
        {
            put32(out, static_cast<uint32_t>(data.size()));
            const size_t start = out.size();
            out.insert(out.end(), type, type + 4);
            out.insert(out.end(), data.begin(), data.end());
            put32(out, crc32(0u, out.data() + start, out.size() - start));
        }

        // The whole file in memory. An image is at most a few thousand px a side here; one whose data would not fit a
        // chunk's 31-bit length is refused.
        bool encode(const Image& img, std::vector<uint8_t>& png)
        {
            const size_t w = static_cast<size_t>(img.w), h = static_cast<size_t>(img.h);
            const size_t rowBytes = w * 4u + 1u;         // the filter byte, then the pixels
            const size_t rawBytes = rowBytes * h;
            constexpr size_t kBlock = 65535u;            // a stored block's most
            const size_t blocks = (rawBytes + kBlock - 1u) / kBlock;
            const size_t zlibBytes = 2u + rawBytes + blocks * 5u + 4u;
            if (zlibBytes > 0x7fffffffu)
                return false;

            // The scanlines: filter type 0 (none) before each row.
            std::vector<uint8_t> raw(rawBytes);
            for (size_t y = 0; y < h; ++y)
            {
                uint8_t* row = raw.data() + y * rowBytes;
                row[0] = 0u;
                const uint8_t* src = img.rgba.data() + y * w * 4u;
                for (size_t i = 0; i < w * 4u; ++i)
                    row[1u + i] = src[i];
            }

            // zlib: the header (deflate, a 32 KiB window, no preset dictionary, the fastest level: 0x78 0x01, a
            // multiple of 31 as the format asks), the stored blocks, the Adler-32 of the scanlines.
            std::vector<uint8_t> idat;
            idat.reserve(zlibBytes);
            idat.push_back(0x78u);
            idat.push_back(0x01u);
            for (size_t at = 0; at < rawBytes; at += kBlock)
            {
                const size_t n = rawBytes - at < kBlock ? rawBytes - at : kBlock;
                idat.push_back(at + n == rawBytes ? 1u : 0u);    // BFINAL, and BTYPE 00: stored
                idat.push_back(static_cast<uint8_t>(n & 0xffu));
                idat.push_back(static_cast<uint8_t>(n >> 8));
                idat.push_back(static_cast<uint8_t>(~n & 0xffu));
                idat.push_back(static_cast<uint8_t>((~n >> 8) & 0xffu));
                idat.insert(idat.end(), raw.begin() + static_cast<std::ptrdiff_t>(at),
                            raw.begin() + static_cast<std::ptrdiff_t>(at + n));
            }
            put32(idat, adler32(1u, raw.data(), raw.size()));

            std::vector<uint8_t> ihdr;
            put32(ihdr, static_cast<uint32_t>(img.w));
            put32(ihdr, static_cast<uint32_t>(img.h));
            ihdr.push_back(8u);                          // bits a channel
            ihdr.push_back(6u);                          // colour type: RGB with alpha
            ihdr.push_back(0u);                          // compression: deflate
            ihdr.push_back(0u);                          // filter method 0
            ihdr.push_back(0u);                          // no interlace

            static constexpr uint8_t kSignature[8] = { 0x89u, 'P', 'N', 'G', '\r', '\n', 0x1au, '\n' };
            png.clear();
            png.reserve(sizeof kSignature + 12u + ihdr.size() + 12u + idat.size() + 12u);
            png.insert(png.end(), kSignature, kSignature + sizeof kSignature);
            putChunk(png, "IHDR", ihdr);
            putChunk(png, "IDAT", idat);
            putChunk(png, "IEND", {});
            return true;
        }
    }

    bool writePng(const Image& img, const char* path)
    {
        if (path == nullptr || *path == 0 || img.w <= 0 || img.h <= 0
            || img.rgba.size() != static_cast<size_t>(img.w) * static_cast<size_t>(img.h) * 4u)
            return false;
        std::vector<uint8_t> png;
        if (!encode(img, png))
            return false;

        const std::string tmp = std::string(path) + ".partial";
        std::remove(tmp.c_str());
        std::FILE* f = std::fopen(tmp.c_str(), "wb");
        if (f == nullptr)
            return false;
        const bool written = std::fwrite(png.data(), 1u, png.size(), f) == png.size();
        const bool closed = std::fclose(f) == 0;
        if (written && closed)
        {
            if (std::rename(tmp.c_str(), path) == 0)
                return true;
            // Windows' rename refuses a name that exists: the old file goes first there.
            std::remove(path);
            if (std::rename(tmp.c_str(), path) == 0)
                return true;
        }
        std::remove(tmp.c_str());
        return false;
    }
}

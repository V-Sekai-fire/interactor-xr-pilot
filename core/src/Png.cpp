// SPDX-License-Identifier: Apache-2.0 OR MIT

#include "xrpilot/Png.h"

#include <algorithm>
#include <array>
#include <fstream>

namespace xrpilot
{

namespace
{

void putU32(std::vector<uint8_t>& out, uint32_t v)
{
    out.push_back(uint8_t(v >> 24));
    out.push_back(uint8_t(v >> 16));
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v));
}

void chunk(std::vector<uint8_t>& out, const char type[4], const std::vector<uint8_t>& data)
{
    putU32(out, uint32_t(data.size()));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    putU32(out, crc32(out.data() + start, out.size() - start));
}

} // namespace

uint32_t crc32(const uint8_t* data, size_t size, uint32_t crc)
{
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t n = 0; n < 256; ++n)
        {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (size_t i = 0; i < size; ++i)
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

std::vector<uint8_t> encodePng(const uint8_t* rgba, int width, int height)
{
    // Each scanline is a filter byte (0, none) and its pixels.
    const size_t stride = size_t(width) * 4;
    std::vector<uint8_t> raw;
    raw.reserve((stride + 1) * size_t(height));
    for (int y = 0; y < height; ++y)
    {
        raw.push_back(0);
        raw.insert(raw.end(), rgba + y * stride, rgba + (y + 1) * stride);
    }

    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1;
    uint32_t b = 0;
    for (uint8_t byte : raw)
    {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    for (size_t offset = 0; offset < raw.size() || offset == 0;)
    {
        const size_t len = std::min<size_t>(65535, raw.size() - offset);
        const bool last = offset + len >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(uint8_t(len));
        z.push_back(uint8_t(len >> 8));
        z.push_back(uint8_t(~len));
        z.push_back(uint8_t(~len >> 8));
        z.insert(z.end(), raw.begin() + offset, raw.begin() + offset + len);
        offset += len;
        if (last)
            break;
    }
    putU32(z, (b << 16) | a);

    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> header;
    putU32(header, uint32_t(width));
    putU32(header, uint32_t(height));
    header.insert(header.end(), {8, 6, 0, 0, 0}); // 8-bit RGBA, deflate, adaptive filter, no interlace
    chunk(out, "IHDR", header);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});
    return out;
}

bool writePng(const std::string& path, const uint8_t* rgba, int width, int height)
{
    const std::vector<uint8_t> png = encodePng(rgba, width, height);
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
    return bool(file);
}

} // namespace xrpilot

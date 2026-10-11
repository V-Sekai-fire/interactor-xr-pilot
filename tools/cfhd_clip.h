// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// What frames2cfhd and pyro2cfhd share: fitting an RGBA frame into the 4K canvas, and the CineForm
// .cfhd RIFF that entities-godot-cineform's MovieWriterCineForm writes and interactor-av1mkv reads.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#define _ftelli64 ftello
#define _fseeki64 fseeko
#endif

namespace cfhdclip
{

inline double catmullRom(double x)
{
    x = std::abs(x);
    if (x < 1.0)
        return 1.5 * x * x * x - 2.5 * x * x + 1.0;
    if (x < 2.0)
        return -0.5 * x * x * x + 2.5 * x * x - 4.0 * x + 2.0;
    return 0.0;
}

// Catmull-Rom taps from src samples to dst: output i reads src[first[i] + k] with weight w[i * n + k].
struct Taps
{
    std::vector<int> first;
    std::vector<float> w;
    int n = 0;
};

inline Taps resampleTaps(int src, int dst)
{
    Taps t;
    const double scale = double(src) / dst, widen = std::max(1.0, scale), support = 2.0 * widen;
    t.n = 2 * int(std::ceil(support)) + 1;
    t.first.resize(size_t(dst));
    t.w.assign(size_t(dst) * t.n, 0.0f);
    for (int i = 0; i < dst; i++)
    {
        const double c = (i + 0.5) * scale - 0.5;
        const int f0 = int(std::floor(c - support)) + 1;
        double sum = 0.0;
        for (int k = 0; k < t.n; k++)
            sum += catmullRom((f0 + k - c) / widen);
        for (int k = 0; k < t.n; k++)
            t.w[size_t(i) * t.n + k] = float(catmullRom((f0 + k - c) / widen) / sum);
        t.first[size_t(i)] = f0;
    }
    return t;
}

template <typename Body> void parallelRows(int count, const Body& body)
{
    const int threads = std::max(1, std::min(int(std::thread::hardware_concurrency()), count));
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; t++)
        pool.emplace_back([&, t] { body(count * t / threads, count * (t + 1) / threads); });
    for (std::thread& th : pool)
        th.join();
}

struct Fit
{
    int dw = 0, dh = 0;
    Taps x, y;
};

// The largest even-sized rectangle with sw:sh's aspect inside W x H, centred, and its taps.
inline Fit fitInto(int sw, int sh, int W, int H)
{
    const double s = std::min(double(W) / sw, double(H) / sh);
    Fit f;
    f.dw = std::max(2, int(sw * s) & ~1);
    f.dh = std::max(2, int(sh * s) & ~1);
    f.x = resampleTaps(sw, f.dw);
    f.y = resampleTaps(sh, f.dh);
    return f;
}

// Fits src into a W x H canvas, letterboxed, BGRA bottom row first (AVI); Catmull-Rom was the most faithful on Alyx.
inline void fitBgraBottomUp(const std::vector<uint8_t>& src, int sw, int sh, int W, int H, std::vector<uint8_t>& out)
{
    out.assign(size_t(W) * size_t(H) * 4, 0);
    const Fit fit = fitInto(sw, sh, W, H);
    const int dw = fit.dw, dh = fit.dh, ox = (W - dw) / 2, oy = (H - dh) / 2;
    const Taps &tx = fit.x, &ty = fit.y;
    std::vector<float> rows(size_t(sh) * dw * 3);
    parallelRows(sh, [&](int begin, int end) {
        for (int y = begin; y < end; y++)
            for (int x = 0; x < dw; x++)
                for (int c = 0; c < 3; c++)
                {
                    float v = 0.0f;
                    for (int k = 0; k < tx.n; k++)
                    {
                        const int sx = std::clamp(tx.first[size_t(x)] + k, 0, sw - 1);
                        v += tx.w[size_t(x) * tx.n + k] * src[(size_t(y) * sw + sx) * 4 + c];
                    }
                    rows[(size_t(y) * dw + x) * 3 + c] = v;
                }
    });
    parallelRows(dh, [&](int begin, int end) {
        for (int y = begin; y < end; y++)
        {
            uint8_t* row = &out[size_t(H - 1 - (oy + y)) * size_t(W) * 4];
            for (int x = 0; x < dw; x++)
            {
                uint8_t* d = row + size_t(ox + x) * 4;
                for (int c = 0; c < 3; c++)
                {
                    float v = 0.0f;
                    for (int k = 0; k < ty.n; k++)
                    {
                        const int sy = std::clamp(ty.first[size_t(y)] + k, 0, sh - 1);
                        v += ty.w[size_t(y) * ty.n + k] * rows[(size_t(sy) * dw + x) * 3 + c];
                    }
                    d[2 - c] = uint8_t(std::clamp(v + 0.5f, 0.0f, 255.0f));
                }
                d[3] = 255;
            }
        }
    });
}

struct Avi
{
    std::FILE* f = nullptr;
    int64_t riffAt = 0, framesAt = 0, streamLenAt = 0, audioLenAt = 0, moviSizeAt = 0, moviStart = 0;
    struct Entry { uint32_t offset, size, stream; };
    std::vector<Entry> index;
    uint32_t frames = 0, audioBytes = 0;

    void u32(uint32_t v) { const uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}; std::fwrite(b, 1, 4, f); }
    void u16(uint16_t v) { const uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)}; std::fwrite(b, 1, 2, f); }
    void tag(const char* t) { std::fwrite(t, 1, 4, f); }
    int64_t tell() { return _ftelli64(f); }

    bool begin(const std::string& path, int W, int H, uint32_t rate, uint32_t mixRate)
    {
        f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        tag("RIFF"); riffAt = tell(); u32(0); tag("AVI ");
        tag("LIST"); u32(4 + 8 + 56 + 8 + 4 + 8 + 56 + 8 + 40 + 8 + 4 + 8 + 56 + 8 + 18); tag("hdrl");
        tag("avih"); u32(56); u32(1000000u / rate); u32(0); u32(0); u32(0x110);
        framesAt = tell(); u32(0); u32(0); u32(2); u32(0); u32(uint32_t(W)); u32(uint32_t(H));
        for (int i = 0; i < 4; i++) u32(0);
        tag("LIST"); u32(4 + 8 + 56 + 8 + 40); tag("strl");
        tag("strh"); u32(56); tag("vids"); tag("CFHD"); u32(0); u32(0); u32(0); u32(1); u32(rate); u32(0);
        streamLenAt = tell(); u32(0); u32(0); u32(0xFFFFFFFFu); u32(0); u32(0); u32(uint32_t(W) | uint32_t(H) << 16);
        tag("strf"); u32(40); u32(40); u32(uint32_t(W)); u32(uint32_t(H)); u32(1 | 24u << 16); tag("CFHD");
        u32(uint32_t(W) * uint32_t(H) * 3); for (int i = 0; i < 4; i++) u32(0);
        const uint32_t align = 4;
        tag("LIST"); u32(4 + 8 + 56 + 8 + 18); tag("strl");
        tag("strh"); u32(56); tag("auds"); u32(1); u32(0); u32(0); u32(0); u32(align); u32(mixRate); u32(0);
        audioLenAt = tell(); u32(0); u32(mixRate * align); u32(0xFFFFFFFFu); u32(align); u32(0); u32(0);
        tag("strf"); u32(18); u16(1); u16(2); u32(mixRate); u32(mixRate * align); u16(uint16_t(align)); u16(16); u16(0);
        tag("LIST"); moviSizeAt = tell(); u32(0); tag("movi"); moviStart = tell();
        return true;
    }
    void chunk(const char* t, const void* data, uint32_t n, uint32_t stream)
    {
        const int64_t here = tell();
        tag(t); u32(n); std::fwrite(data, 1, n, f);
        if (n & 1) { const uint8_t pad = 0; std::fwrite(&pad, 1, 1, f); }
        index.push_back({uint32_t(here - moviStart + 4), n, stream});
        if (stream == 0) frames++; else audioBytes += n;
    }
    bool finish()
    {
        const int64_t moviEnd = tell();
        tag("idx1"); u32(uint32_t(index.size() * 16));
        for (const Entry& e : index) { tag(e.stream == 0 ? "00dc" : "01wb"); u32(0x10); u32(e.offset); u32(e.size); }
        const int64_t end = tell();
        const bool fits = end - riffAt - 4 <= int64_t(0xFFFFFFFFu);
        _fseeki64(f, riffAt, SEEK_SET); u32(uint32_t(end - riffAt - 4));
        _fseeki64(f, moviSizeAt, SEEK_SET); u32(uint32_t(moviEnd - moviSizeAt - 4));
        _fseeki64(f, framesAt, SEEK_SET); u32(frames);
        _fseeki64(f, streamLenAt, SEEK_SET); u32(frames);
        _fseeki64(f, audioLenAt, SEEK_SET); u32(audioBytes / 4);
        return std::fclose(f) == 0 && fits;
    }
};


} // namespace cfhdclip

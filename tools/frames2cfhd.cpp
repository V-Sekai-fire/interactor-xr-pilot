// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// xr-pilot's kept left-eye frames (<seq>_<monotonic ms>.png) to a CineForm .cfhd, the RIFF that
// entities-godot-cineform's MovieWriterCineForm writes, so interactor-av1mkv's `mkv` and `check` and
// deliver.exs take it unchanged. Each output frame at time t shows the newest frame captured at or
// before t, so the clip plays at the capture's real pace and lasts first-to-last capture. Frames are
// fitted into the output size, letterboxed on black. Audio is silent 16-bit stereo at 48 kHz.
//
//   frames2cfhd <frames dir> <out.cfhd> [--fps 30] [--size 3840x2160] [--quality 0..5]

#include <cstddef>
#include <cstdint>

#include "CFHDEncoder.h"
#include "CFHDError.h"
#include "CFHDTypes.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

#include "cfhd_clip.h"

using cfhdclip::Avi;
using cfhdclip::fitBgraBottomUp;

namespace
{

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

uint8_t paeth(int a, int b, int c)
{
    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return uint8_t(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

// RGBA8 PNGs whose zlib stream is stored blocks, as xr-pilot's encodePng writes them. Anything else
// is refused by name rather than decoded wrong.
bool readPng(const std::string& path, std::vector<uint8_t>& rgba, int& w, int& h, std::string& err)
{
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return err = "cannot open " + path, false;
    std::vector<uint8_t> b;
    std::fseek(f, 0, SEEK_END);
    b.resize(size_t(std::ftell(f)));
    std::fseek(f, 0, SEEK_SET);
    const bool read = std::fread(b.data(), 1, b.size(), f) == b.size();
    std::fclose(f);
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (!read || b.size() < 8 || std::memcmp(b.data(), sig, 8)) return err = path + ": not a PNG", false;
    std::vector<uint8_t> z;
    size_t p = 8;
    while (p + 12 <= b.size())
    {
        const uint32_t len = be32(&b[p]);
        const char* type = reinterpret_cast<const char*>(&b[p + 4]);
        if (p + 12 + len > b.size()) return err = path + ": a chunk is cut short", false;
        if (!std::memcmp(type, "IHDR", 4))
        {
            w = int(be32(&b[p + 8]));
            h = int(be32(&b[p + 12]));
            if (b[p + 16] != 8 || b[p + 17] != 6 || b[p + 20] != 0) return err = path + ": not 8-bit RGBA non-interlaced", false;
        }
        else if (!std::memcmp(type, "IDAT", 4))
            z.insert(z.end(), &b[p + 8], &b[p + 8 + len]);
        else if (!std::memcmp(type, "IEND", 4))
            break;
        p += 12 + len;
    }
    const size_t stride = size_t(w) * 4, want = (stride + 1) * size_t(h);
    std::vector<uint8_t> raw;
    raw.reserve(want);
    size_t q = 2; // the zlib header
    for (bool last = false; !last;)
    {
        if (q >= z.size()) return err = path + ": the zlib stream ends early", false;
        const uint8_t hdr = z[q++];
        last = hdr & 1;
        if ((hdr >> 1) != 0) return err = path + ": a compressed deflate block; only stored blocks are read", false;
        if (q + 4 > z.size()) return err = path + ": a stored block header is cut short", false;
        const uint16_t n = uint16_t(z[q] | z[q + 1] << 8);
        if (uint16_t(~n) != uint16_t(z[q + 2] | z[q + 3] << 8)) return err = path + ": a stored block length fails its check", false;
        q += 4;
        if (q + n > z.size()) return err = path + ": a stored block is cut short", false;
        raw.insert(raw.end(), &z[q], &z[q + n]);
        q += n;
    }
    if (raw.size() != want) return err = path + ": " + std::to_string(raw.size()) + " bytes of scanlines, expected " + std::to_string(want), false;
    rgba.assign(stride * size_t(h), 0);
    for (int y = 0; y < h; y++)
    {
        const uint8_t ft = raw[size_t(y) * (stride + 1)];
        const uint8_t* s = &raw[size_t(y) * (stride + 1) + 1];
        uint8_t* d = &rgba[size_t(y) * stride];
        const uint8_t* up = y ? d - stride : nullptr;
        for (size_t x = 0; x < stride; x++)
        {
            const int a = x >= 4 ? d[x - 4] : 0, u = up ? up[x] : 0, c = (x >= 4 && up) ? up[x - 4] : 0;
            switch (ft)
            {
            case 0: d[x] = s[x]; break;
            case 1: d[x] = uint8_t(s[x] + a); break;
            case 2: d[x] = uint8_t(s[x] + u); break;
            case 3: d[x] = uint8_t(s[x] + ((a + u) >> 1)); break;
            case 4: d[x] = uint8_t(s[x] + paeth(a, u, c)); break;
            default: return err = path + ": unknown filter " + std::to_string(ft), false;
            }
        }
    }
    return true;
}

int fail(const std::string& m) { std::fprintf(stderr, "frames2cfhd: %s\n", m.c_str()); return 1; }

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) return fail("usage: frames2cfhd <frames dir> <out.cfhd> [--fps 30] [--size 3840x2160] [--quality 0..5]");
    uint32_t fps = 30, mixRate = 48000;
    int W = 3840, H = 2160, quality = 2;
    for (int i = 3; i + 1 < argc; i += 2)
    {
        if (!std::strcmp(argv[i], "--fps")) fps = uint32_t(std::atoi(argv[i + 1]));
        else if (!std::strcmp(argv[i], "--size")) std::sscanf(argv[i + 1], "%dx%d", &W, &H);
        else if (!std::strcmp(argv[i], "--quality")) quality = std::clamp(std::atoi(argv[i + 1]), 0, 5);
        else return fail(std::string("unknown option ") + argv[i]);
    }
    if (fps == 0 || mixRate % fps) return fail("48000 must divide by --fps");
    if ((W & 1) || (H & 1)) return fail("CineForm needs an even width and height");

    struct In { std::string path; int64_t ms; };
    std::vector<In> in;
    for (const fs::directory_entry& e : fs::directory_iterator(argv[1]))
    {
        const std::string n = e.path().filename().string();
        const size_t u = n.find('_');
        if (e.path().extension() != ".png" || u == std::string::npos) continue;
        in.push_back({e.path().string(), std::atoll(n.c_str() + u + 1)});
    }
    std::sort(in.begin(), in.end(), [](const In& a, const In& b) { return a.path < b.path; });
    if (in.size() < 2) return fail("fewer than two <seq>_<ms>.png frames in " + std::string(argv[1]));
    for (size_t i = 1; i < in.size(); i++)
        if (in[i].ms < in[i - 1].ms) return fail("frame times go backwards at " + in[i].path);
    const int64_t spanMs = in.back().ms - in.front().ms;
    const uint64_t total = uint64_t(spanMs) * fps / 1000 + 1;

    static const CFHD_EncodingQuality ladder[] = {CFHD_ENCODING_QUALITY_LOW, CFHD_ENCODING_QUALITY_MEDIUM, CFHD_ENCODING_QUALITY_HIGH,
                                                  CFHD_ENCODING_QUALITY_FILMSCAN1, CFHD_ENCODING_QUALITY_FILMSCAN2, CFHD_ENCODING_QUALITY_FILMSCAN3};
    CFHD_EncoderPoolRef pool = nullptr;
    const int jobs = 8;
    if (CFHD_CreateEncoderPool(&pool, 16, jobs, nullptr) != CFHD_ERROR_OKAY) return fail("CFHD_CreateEncoderPool failed");
    if (CFHD_PrepareEncoderPool(pool, uint_least16_t(W), uint_least16_t(H), CFHD_PIXEL_FORMAT_BGRA, CFHD_ENCODED_FORMAT_RGB_444,
                                CFHD_ENCODING_FLAGS_NONE, ladder[quality]) != CFHD_ERROR_OKAY)
        return fail("CFHD_PrepareEncoderPool failed");
    if (CFHD_StartEncoderPool(pool) != CFHD_ERROR_OKAY) return fail("CFHD_StartEncoderPool failed");

    Avi avi;
    if (!avi.begin(argv[2], W, H, fps, mixRate)) return fail(std::string("cannot write ") + argv[2]);
    const std::vector<int16_t> silence(size_t(mixRate / fps) * 2, 0);

    // The pool reads each frame's buffer until its sample comes back, so a ring of staging buffers
    // one longer than the job queue is never reused early.
    std::vector<std::vector<uint8_t>> ring(jobs + 1);
    std::vector<uint8_t> canvas, rgba;
    size_t shown = size_t(-1), next = 0;
    int queued = 0, sw = 0, sh = 0;
    uint64_t changes = 0;
    std::string err;
    auto collect = [&]() -> bool {
        uint32_t number = 0;
        CFHD_SampleBufferRef buf = nullptr;
        if (CFHD_WaitForSample(pool, &number, &buf) != CFHD_ERROR_OKAY || !buf) return false;
        void* data = nullptr;
        size_t len = 0;
        const bool ok = CFHD_GetEncodedSample(buf, &data, &len) == CFHD_ERROR_OKAY && data && len;
        if (ok) avi.chunk("00dc", data, uint32_t(len), 0);
        CFHD_ReleaseSampleBuffer(pool, buf);
        queued--;
        return ok;
    };
    for (uint64_t k = 0; k < total; k++)
    {
        const int64_t t = in.front().ms + int64_t(k * 1000 / fps);
        while (next + 1 < in.size() && in[next + 1].ms <= t) next++;
        if (next != shown)
        {
            if (!readPng(in[next].path, rgba, sw, sh, err)) return fail(err);
            fitBgraBottomUp(rgba, sw, sh, W, H, canvas);
            shown = next;
            changes++;
        }
        if (queued >= jobs && !collect()) return fail("the encoder returned no sample at frame " + std::to_string(k));
        std::vector<uint8_t>& slot = ring[k % ring.size()];
        slot = canvas;
        if (CFHD_EncodeAsyncSample(pool, uint32_t(k), slot.data(), intptr_t(W) * 4, nullptr) != CFHD_ERROR_OKAY)
            return fail("CFHD_EncodeAsyncSample failed at frame " + std::to_string(k));
        queued++;
        avi.chunk("01wb", silence.data(), uint32_t(silence.size() * 2), 1);
        if (k % (fps * 10) == 0) std::fprintf(stderr, "frames2cfhd: %llu/%llu\n", (unsigned long long)k, (unsigned long long)total);
    }
    while (queued > 0)
        if (!collect()) return fail("the encoder returned no sample while draining");
    CFHD_StopEncoderPool(pool);
    CFHD_ReleaseEncoderPool(pool);
    if (!avi.finish()) return fail("the AVI is over 4 GB or did not close");
    if (avi.frames != total) return fail(std::to_string(total) + " frames submitted, " + std::to_string(avi.frames) + " written");
    std::printf("frames2cfhd: %s  %dx%d  %u fps  %u frames (%.3f s) from %zu captures over %lld ms, %llu distinct, quality %d\n", argv[2], W, H,
                fps, avi.frames, double(avi.frames) / fps, in.size(), (long long)spanMs, (unsigned long long)changes, quality);
    return 0;
}

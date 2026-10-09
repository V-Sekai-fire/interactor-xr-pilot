// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// A .pwrec stream recording (xr-pilot --record-stream) to a CineForm .cfhd, which interactor-av1mkv's
// deliver.exs turns into the MKV. PyroWave decodes on a chosen Vulkan GPU, so the one running VR is
// left alone; CineForm encodes on the CPU. Each output frame at time t shows the newest streamed
// frame presented at or before t; the left eye is fitted into the canvas, letterboxed on black.
// Audio is silent 16-bit stereo at 48 kHz.
//
//   pyro2cfhd <in.pwrec> <out.cfhd> [--fps 30] [--size 3840x2160] [--quality 0..5] [--gpu <vid>:<pid>]
//   pyro2cfhd --self-test <dir> [--gpu <vid>:<pid>]
//
// --gpu takes the PCI vendor and device ids in hex (10de:2684 is an RTX 4090); without it PyroWave
// takes its default device.

#include "cfhd_clip.h"
#include "xrpilot/GpuDecoder.h"
#include "xrpilot/StreamRecord.h"

#include <cstddef>
#include <cstdint>

#include "CFHDEncoder.h"
#include "CFHDError.h"
#include "CFHDTypes.h"

#include <pyrowave.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using cfhdclip::Avi;
using cfhdclip::fitBgraBottomUp;
using xrpilot::StreamFrame;

namespace
{

int fail(const std::string& m)
{
    std::fprintf(stderr, "pyro2cfhd: %s\n", m.c_str());
    return 1;
}

struct Decoder
{
    pyrowave_device device = nullptr;
    pyrowave_decoder decoder = nullptr;
    int width = 0, height = 0;
    std::vector<uint8_t> y, cb, cr;

    ~Decoder()
    {
        if (decoder)
            pyrowave_decoder_destroy(decoder);
    }

    // The frame's left eye as RGBA8: the stream is both eyes side by side, BT.709 full range.
    bool leftEye(const std::vector<uint8_t>& stream, std::vector<uint8_t>& rgba, int& ew, int& eh, std::string& err)
    {
        int w = 0, h = 0;
        if (!xrpilot::GpuDecoder::frameSize(stream.data(), stream.size(), w, h) || w < 4 || h < 2)
            return err = "not a PyroWave frame", false;
        if (!decoder || w != width || h != height)
        {
            if (decoder)
                pyrowave_decoder_destroy(decoder);
            decoder = nullptr;
            const pyrowave_decoder_create_info info = {device, w, h, PYROWAVE_CHROMA_SUBSAMPLING_420, false};
            if (pyrowave_decoder_create(&info, &decoder) != PYROWAVE_SUCCESS)
                return err = "cannot make a " + std::to_string(w) + "x" + std::to_string(h) + " decoder", false;
            width = w;
            height = h;
            y.assign(size_t(w) * h, 0);
            cb.assign(size_t(w / 2) * (h / 2), 0);
            cr.assign(size_t(w / 2) * (h / 2), 0);
        }
        pyrowave_cpu_buffer b = {};
        b.data[0] = y.data();
        b.data[1] = cb.data();
        b.data[2] = cr.data();
        b.row_stride_in_bytes[0] = size_t(w);
        b.row_stride_in_bytes[1] = b.row_stride_in_bytes[2] = size_t(w / 2);
        b.plane_size_in_bytes[0] = y.size();
        b.plane_size_in_bytes[1] = cb.size();
        b.plane_size_in_bytes[2] = cr.size();
        b.width = w;
        b.height = h;
        b.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
        pyrowave_decoder_clear(decoder);
        if (pyrowave_decoder_push_packet(decoder, stream.data(), stream.size()) != PYROWAVE_SUCCESS ||
            !pyrowave_decoder_decode_is_ready(decoder, false) ||
            pyrowave_decoder_decode_cpu_buffer_synchronous(decoder, &b) != PYROWAVE_SUCCESS)
            return err = "the frame does not decode", false;
        ew = (w / 2) & ~1;
        eh = h;
        rgba.resize(size_t(ew) * eh * 4);
        for (int j = 0; j < eh; ++j)
            for (int i = 0; i < ew; ++i)
            {
                const size_t c = size_t(j / 2) * (w / 2) + size_t(i / 2);
                const float Y = y[size_t(j) * w + i], Cb = cb[c] - 128.0f, Cr = cr[c] - 128.0f;
                uint8_t* o = &rgba[(size_t(j) * ew + i) * 4];
                o[0] = uint8_t(std::clamp(Y + 1.5748f * Cr + 0.5f, 0.0f, 255.0f));
                o[1] = uint8_t(std::clamp(Y - 0.1873f * Cb - 0.4681f * Cr + 0.5f, 0.0f, 255.0f));
                o[2] = uint8_t(std::clamp(Y + 1.8556f * Cb + 0.5f, 0.0f, 255.0f));
                o[3] = 255;
            }
        return true;
    }
};

struct Options
{
    uint32_t fps = 30;
    int W = 3840, H = 2160, quality = 2;
    uint32_t vid = 0, pid = 0;
};

bool makeDevice(const Options& o, pyrowave_device& device)
{
    return (o.vid || o.pid ? pyrowave_create_device_by_compat(o.vid, o.pid, nullptr, nullptr, nullptr, &device)
                           : pyrowave_create_default_device(&device)) == PYROWAVE_SUCCESS;
}

struct Summary
{
    uint32_t frames = 0;
    uint64_t streamed = 0, distinct = 0;
    int64_t spanNs = 0;
    double decodeS = 0, totalS = 0;
};

int convert(pyrowave_device device, const std::string& in, const std::string& out, const Options& o, Summary& sum)
{
    using Clock = std::chrono::steady_clock;
    const Clock::time_point t0 = Clock::now();
    if (o.fps == 0 || 48000 % o.fps)
        return fail("48000 must divide by --fps");
    if ((o.W & 1) || (o.H & 1))
        return fail("CineForm needs an even width and height");

    // A first pass for the times only, so the clip's length is known and nothing is held in memory.
    std::vector<int64_t> times;
    {
        xrpilot::StreamRecordReader r;
        std::string err;
        if (!r.open(in, &err))
            return fail(err);
        StreamFrame f;
        while (r.next(f, &err))
        {
            if (!times.empty() && f.presentationNs < times.back())
                return fail("presentation times go backwards at frame " + std::to_string(f.frameIndex));
            times.push_back(f.presentationNs);
        }
        if (!err.empty())
            return fail(err);
    }
    if (times.size() < 2)
        return fail("fewer than two streamed frames in " + in);
    sum.streamed = times.size();
    sum.spanNs = times.back() - times.front();
    const uint64_t total = uint64_t(sum.spanNs) * o.fps / 1'000'000'000ull + 1;

    static const CFHD_EncodingQuality ladder[] = {CFHD_ENCODING_QUALITY_LOW,       CFHD_ENCODING_QUALITY_MEDIUM,
                                                  CFHD_ENCODING_QUALITY_HIGH,      CFHD_ENCODING_QUALITY_FILMSCAN1,
                                                  CFHD_ENCODING_QUALITY_FILMSCAN2, CFHD_ENCODING_QUALITY_FILMSCAN3};
    const int jobs = 8;
    CFHD_EncoderPoolRef pool = nullptr;
    if (CFHD_CreateEncoderPool(&pool, 16, jobs, nullptr) != CFHD_ERROR_OKAY ||
        CFHD_PrepareEncoderPool(pool, uint_least16_t(o.W), uint_least16_t(o.H), CFHD_PIXEL_FORMAT_BGRA,
                                CFHD_ENCODED_FORMAT_RGB_444, CFHD_ENCODING_FLAGS_NONE, ladder[o.quality]) != CFHD_ERROR_OKAY ||
        CFHD_StartEncoderPool(pool) != CFHD_ERROR_OKAY)
        return fail("the CineForm encoder pool did not start");

    Avi avi;
    if (!avi.begin(out, o.W, o.H, o.fps, 48000))
        return fail("cannot write " + out);
    const std::vector<int16_t> silence(size_t(48000 / o.fps) * 2, 0);

    xrpilot::StreamRecordReader r;
    std::string err;
    r.open(in, &err);
    Decoder dec;
    dec.device = device;
    StreamFrame current, ahead;
    bool haveAhead = r.next(ahead, &err);
    bool haveCurrent = false;
    std::vector<std::vector<uint8_t>> ring(jobs + 1);
    std::vector<uint8_t> canvas, rgba;
    int queued = 0;
    auto collect = [&]() -> bool {
        uint32_t number = 0;
        CFHD_SampleBufferRef buf = nullptr;
        if (CFHD_WaitForSample(pool, &number, &buf) != CFHD_ERROR_OKAY || !buf)
            return false;
        void* data = nullptr;
        size_t len = 0;
        const bool ok = CFHD_GetEncodedSample(buf, &data, &len) == CFHD_ERROR_OKAY && data && len;
        if (ok)
            avi.chunk("00dc", data, uint32_t(len), 0);
        CFHD_ReleaseSampleBuffer(pool, buf);
        queued--;
        return ok;
    };
    for (uint64_t k = 0; k < total; k++)
    {
        const int64_t t = times.front() + int64_t(k * 1'000'000'000ull / o.fps);
        bool changed = false;
        while (haveAhead && ahead.presentationNs <= t)
        {
            current = std::move(ahead);
            haveCurrent = changed = true;
            haveAhead = r.next(ahead, &err);
        }
        if (!err.empty())
            return fail(err);
        if (changed)
        {
            int ew = 0, eh = 0;
            const Clock::time_point d0 = Clock::now();
            if (!dec.leftEye(current.payload, rgba, ew, eh, err))
                return fail("frame " + std::to_string(current.frameIndex) + ": " + err);
            sum.decodeS += std::chrono::duration<double>(Clock::now() - d0).count();
            fitBgraBottomUp(rgba, ew, eh, o.W, o.H, canvas);
            sum.distinct++;
        }
        if (!haveCurrent)
            return fail("no frame at the clip's start");
        if (queued >= jobs && !collect())
            return fail("the encoder returned no sample at frame " + std::to_string(k));
        std::vector<uint8_t>& slot = ring[k % ring.size()];
        slot = canvas;
        if (CFHD_EncodeAsyncSample(pool, uint32_t(k), slot.data(), intptr_t(o.W) * 4, nullptr) != CFHD_ERROR_OKAY)
            return fail("CFHD_EncodeAsyncSample failed at frame " + std::to_string(k));
        queued++;
        avi.chunk("01wb", silence.data(), uint32_t(silence.size() * 2), 1);
        if (k % (o.fps * 10) == 0)
            std::fprintf(stderr, "pyro2cfhd: %llu/%llu\n", (unsigned long long)k, (unsigned long long)total);
    }
    while (queued > 0)
        if (!collect())
            return fail("the encoder returned no sample while draining");
    CFHD_StopEncoderPool(pool);
    CFHD_ReleaseEncoderPool(pool);
    if (!avi.finish())
        return fail("the AVI is over 4 GB or did not close");
    sum.frames = avi.frames;
    sum.totalS = std::chrono::duration<double>(Clock::now() - t0).count();
    if (sum.frames != total)
        return fail(std::to_string(total) + " frames submitted, " + std::to_string(sum.frames) + " written");
    return 0;
}

// Synthetic eyes, PyroWave-encoded on the CPU into a .pwrec at a 90 Hz cadence; then the conversion
// must give span * fps + 1 frames, every streamed frame shown, and refuse damaged input by frame.
int selfTest(const std::string& dir, const Options& base)
{
    pyrowave_device device = nullptr;
    if (!makeDevice(base, device))
        return fail("no PyroWave device");
    const int eyeW = 256, eyeH = 128, w = eyeW * 2, h = eyeH;
    pyrowave_encoder_create_info ei = {device, w, h, PYROWAVE_CHROMA_SUBSAMPLING_420};
    pyrowave_encoder enc = nullptr;
    if (pyrowave_encoder_create(&ei, &enc) != PYROWAVE_SUCCESS)
        return fail("no PyroWave encoder");
    const int count = 90; // one second at 90 Hz
    const std::string rec = dir + "/selftest.pwrec", bad = dir + "/selftest_bad.pwrec";
    xrpilot::StreamRecordWriter wr, wrBad;
    wr.open(rec);
    wrBad.open(bad);
    for (int n = 0; n < count; ++n)
    {
        std::vector<uint8_t> Y(size_t(w) * h), C(size_t(w / 2) * (h / 2), 128);
        for (int j = 0; j < h; ++j)
            for (int i = 0; i < w; ++i)
                Y[size_t(j) * w + i] = uint8_t(((i + n * 2) / 16 + j / 16) % 2 ? 200 : 40);
        std::vector<uint8_t> Cr = C;
        pyrowave_cpu_buffer b = {};
        b.data[0] = Y.data();
        b.data[1] = C.data();
        b.data[2] = Cr.data();
        b.row_stride_in_bytes[0] = size_t(w);
        b.row_stride_in_bytes[1] = b.row_stride_in_bytes[2] = size_t(w / 2);
        b.plane_size_in_bytes[0] = Y.size();
        b.plane_size_in_bytes[1] = C.size();
        b.plane_size_in_bytes[2] = Cr.size();
        b.width = w;
        b.height = h;
        b.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
        pyrowave_rate_control rate = {size_t(w) * h / 2};
        size_t packets = 0, written = 0;
        if (pyrowave_encoder_encode_cpu_synchronous(enc, &b, &rate) != PYROWAVE_SUCCESS ||
            pyrowave_encoder_compute_num_packets(enc, 64 * 1024, &packets) != PYROWAVE_SUCCESS)
            return fail("the self-test encode failed");
        std::vector<pyrowave_packet> pk(packets);
        std::vector<uint8_t> bits(rate.maximum_bitstream_size + 64 * 1024);
        if (pyrowave_encoder_packetize(enc, pk.data(), 64 * 1024, &written, bits.data(), bits.size()) != PYROWAVE_SUCCESS)
            return fail("the self-test packetize failed");
        // The packets lie end to end in the bitstream, as the runtime sends them.
        const size_t bytes = pk[written - 1].offset + pk[written - 1].size;
        std::vector<uint8_t> frame(bits.begin(), bits.begin() + std::ptrdiff_t(bytes));
        StreamFrame f{int64_t(5'000'000'000LL + int64_t(n) * 11'111'111), 0, uint32_t(n), frame};
        wr.write(f);
        if (n == 45)
            f.payload.resize(8); // damaged: too short to be a frame
        wrBad.write(f);
    }
    wr.close();
    wrBad.close();
    pyrowave_encoder_destroy(enc);

    Options o = base;
    o.W = 640;
    o.H = 360;
    o.fps = 30;
    int bad_ = 0;
    auto check = [&](bool ok, const std::string& what) {
        std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
        bad_ += ok ? 0 : 1;
    };
    Summary s;
    const int rc = convert(device, rec, dir + "/selftest.cfhd", o, s);
    check(rc == 0, "a 90 Hz recording converts");
    check(s.frames == 30, "a span of 0.99 s at 30 fps is 30 frames (got " + std::to_string(s.frames) + ")");
    check(s.distinct == 30, "each output frame shows a new streamed frame, none held (got " + std::to_string(s.distinct) + ")");
    Summary s60;
    o.fps = 120;
    convert(device, rec, dir + "/selftest120.cfhd", o, s60);
    check(s60.distinct == 90 && s60.frames == 119,
          "at 120 fps all 90 streamed frames appear once, so a faster clip holds frames rather than inventing them");
    o.fps = 30;
    Summary sb;
    check(convert(device, bad, dir + "/selftest_bad.cfhd", o, sb) != 0, "control: a damaged frame stops the conversion");
    std::printf("%d wrong\n", bad_);
    return bad_ == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    Options o;
    std::vector<std::string> pos;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--fps" && i + 1 < argc)
            o.fps = uint32_t(std::atoi(argv[++i]));
        else if (a == "--size" && i + 1 < argc)
            std::sscanf(argv[++i], "%dx%d", &o.W, &o.H);
        else if (a == "--quality" && i + 1 < argc)
            o.quality = std::clamp(std::atoi(argv[++i]), 0, 5);
        else if (a == "--gpu" && i + 1 < argc)
            std::sscanf(argv[++i], "%x:%x", &o.vid, &o.pid);
        else
            pos.push_back(a);
    }
    if (pos.size() == 2 && pos[0] == "--self-test")
        return selfTest(pos[1], o);
    if (pos.size() != 2)
        return fail("usage: pyro2cfhd <in.pwrec> <out.cfhd> [--fps 30] [--size 3840x2160] [--quality 0..5] [--gpu vid:pid]");
    pyrowave_device device = nullptr;
    if (!makeDevice(o, device))
        return fail("no PyroWave device for that --gpu");
    Summary s;
    const int rc = convert(device, pos[0], pos[1], o, s);
    if (rc == 0)
        std::printf("pyro2cfhd: %s  %dx%d  %u fps  %u frames (%.3f s) from %llu streamed frames over %.3f s, %llu shown; "
                    "decode %.1f s, total %.1f s, quality %d\n",
                    pos[1].c_str(), o.W, o.H, o.fps, s.frames, double(s.frames) / o.fps, (unsigned long long)s.streamed,
                    double(s.spanNs) / 1e9, (unsigned long long)s.distinct, s.decodeS, s.totalS, o.quality);
    return rc;
}

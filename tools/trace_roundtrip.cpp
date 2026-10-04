// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// A kept trace frame through an intermediate image and back:
//   trace_roundtrip decode <traces.sqlite> <asset_id|latest> <out>   frame -> <out>.rgbf32 (scene-linear RGB)
//   trace_roundtrip encode <in.rgbf32> <out>                          RGB -> PyroWave at the frame's own byte
//                                                                      budget -> decoded again, compared
// decode also keeps the decoded planes (<out>.yuv) and the stream (<out>.pyrowave) that encode compares with.

#include "xrpilot/GpuDecoder.h"

#include <pyrowave.h>
#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{

struct Planes
{
    int width = 0;
    int height = 0;
    std::vector<uint8_t> y;
    std::vector<uint8_t> cb;
    std::vector<uint8_t> cr;
};

std::vector<uint8_t> readFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool writeFile(const std::string& path, const void* data, size_t size)
{
    std::ofstream out(path, std::ios::binary);
    out.write(static_cast<const char*>(data), std::streamsize(size));
    return bool(out);
}

pyrowave_cpu_buffer cpuBuffer(Planes& p)
{
    pyrowave_cpu_buffer b = {};
    b.data[0] = p.y.data();
    b.data[1] = p.cb.data();
    b.data[2] = p.cr.data();
    b.row_stride_in_bytes[0] = size_t(p.width);
    b.row_stride_in_bytes[1] = size_t(p.width / 2);
    b.row_stride_in_bytes[2] = size_t(p.width / 2);
    b.plane_size_in_bytes[0] = p.y.size();
    b.plane_size_in_bytes[1] = p.cb.size();
    b.plane_size_in_bytes[2] = p.cr.size();
    b.width = p.width;
    b.height = p.height;
    b.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
    return b;
}

Planes sized(int w, int h)
{
    Planes p;
    p.width = w;
    p.height = h;
    p.y.assign(size_t(w) * size_t(h), 0);
    p.cb.assign(size_t(w / 2) * size_t(h / 2), 0);
    p.cr.assign(size_t(w / 2) * size_t(h / 2), 0);
    return p;
}

bool decode(pyrowave_device device, const std::vector<uint8_t>& stream, Planes& out)
{
    int w = 0;
    int h = 0;
    if (!xrpilot::GpuDecoder::frameSize(stream.data(), stream.size(), w, h))
        return false;
    pyrowave_decoder decoder = nullptr;
    const pyrowave_decoder_create_info info = {device, w, h, PYROWAVE_CHROMA_SUBSAMPLING_420, false};
    if (pyrowave_decoder_create(&info, &decoder) != PYROWAVE_SUCCESS)
        return false;
    out = sized(w, h);
    const pyrowave_cpu_buffer buffer = cpuBuffer(out);
    const bool ok = pyrowave_decoder_push_packet(decoder, stream.data(), stream.size()) == PYROWAVE_SUCCESS &&
                    pyrowave_decoder_decode_is_ready(decoder, false) &&
                    pyrowave_decoder_decode_cpu_buffer_synchronous(decoder, &buffer) == PYROWAVE_SUCCESS;
    pyrowave_decoder_destroy(decoder);
    return ok;
}

// BT.709 full range, as the runtime's encoder converts, with the sRGB curve undone to scene-linear.
float toLinear(float v)
{
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}
float fromLinear(float v)
{
    v = std::clamp(v, 0.0f, 1.0f);
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

std::vector<float> toRgb(const Planes& p)
{
    std::vector<float> rgb(size_t(p.width) * size_t(p.height) * 3);
    for (int y = 0; y < p.height; ++y)
    {
        for (int x = 0; x < p.width; ++x)
        {
            const size_t c = size_t(y / 2) * size_t(p.width / 2) + size_t(x / 2);
            const float Y = p.y[size_t(y) * size_t(p.width) + size_t(x)] / 255.0f;
            const float Cb = p.cb[c] / 255.0f - 0.5f;
            const float Cr = p.cr[c] / 255.0f - 0.5f;
            float* o = &rgb[(size_t(y) * size_t(p.width) + size_t(x)) * 3];
            o[0] = toLinear(std::clamp(Y + 1.5748f * Cr, 0.0f, 1.0f));
            o[1] = toLinear(std::clamp(Y - 0.1873f * Cb - 0.4681f * Cr, 0.0f, 1.0f));
            o[2] = toLinear(std::clamp(Y + 1.8556f * Cb, 0.0f, 1.0f));
        }
    }
    return rgb;
}

Planes fromRgb(const std::vector<float>& rgb, int w, int h)
{
    Planes p = sized(w, h);
    std::vector<float> cb(size_t(w) * size_t(h));
    std::vector<float> cr(size_t(w) * size_t(h));
    for (size_t i = 0; i < size_t(w) * size_t(h); ++i)
    {
        const float r = fromLinear(rgb[i * 3]);
        const float g = fromLinear(rgb[i * 3 + 1]);
        const float b = fromLinear(rgb[i * 3 + 2]);
        const float Y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        p.y[i] = uint8_t(std::lround(std::clamp(Y, 0.0f, 1.0f) * 255.0f));
        cb[i] = (b - Y) / 1.8556f;
        cr[i] = (r - Y) / 1.5748f;
    }
    for (int y = 0; y < h / 2; ++y)
    {
        for (int x = 0; x < w / 2; ++x)
        {
            const size_t a = size_t(2 * y) * size_t(w) + size_t(2 * x);
            const float mb = (cb[a] + cb[a + 1] + cb[a + size_t(w)] + cb[a + size_t(w) + 1]) / 4.0f;
            const float mr = (cr[a] + cr[a + 1] + cr[a + size_t(w)] + cr[a + size_t(w) + 1]) / 4.0f;
            p.cb[size_t(y) * size_t(w / 2) + size_t(x)] = uint8_t(std::lround(std::clamp(mb + 0.5f, 0.0f, 1.0f) * 255.0f));
            p.cr[size_t(y) * size_t(w / 2) + size_t(x)] = uint8_t(std::lround(std::clamp(mr + 0.5f, 0.0f, 1.0f) * 255.0f));
        }
    }
    return p;
}

double psnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    double sum = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
        sum += double(int(a[i]) - int(b[i])) * double(int(a[i]) - int(b[i]));
    const double mse = sum / double(a.size());
    return mse <= 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

int decodeCommand(pyrowave_device device, const std::string& dbPath, const std::string& which, const std::string& out)
{
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(dbPath.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
        return std::fprintf(stderr, "cannot open %s\n", dbPath.c_str()), 1;
    const std::string sql = which == "latest"
                                ? "SELECT asset_id, bytes FROM asset JOIN asset_kind USING (asset_kind_id) WHERE name = "
                                  "'pyrowave' ORDER BY asset_id DESC LIMIT 1"
                                : "SELECT asset_id, bytes FROM asset WHERE asset_id = " + std::to_string(std::stoll(which));
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &s, nullptr);
    std::vector<uint8_t> stream;
    long long id = 0;
    if (sqlite3_step(s) == SQLITE_ROW)
    {
        id = sqlite3_column_int64(s, 0);
        const uint8_t* bytes = static_cast<const uint8_t*>(sqlite3_column_blob(s, 1));
        stream.assign(bytes, bytes + sqlite3_column_bytes(s, 1));
    }
    sqlite3_finalize(s);
    sqlite3_close(db);
    Planes planes;
    if (stream.empty() || !decode(device, stream, planes))
        return std::fprintf(stderr, "FAIL no decodable frame %s\n", which.c_str()), 1;
    const std::vector<float> rgb = toRgb(planes);
    writeFile(out + ".rgbf32", rgb.data(), rgb.size() * sizeof(float));
    std::vector<uint8_t> yuv = planes.y;
    yuv.insert(yuv.end(), planes.cb.begin(), planes.cb.end());
    yuv.insert(yuv.end(), planes.cr.begin(), planes.cr.end());
    writeFile(out + ".yuv", yuv.data(), yuv.size());
    writeFile(out + ".pyrowave", stream.data(), stream.size());
    // The conversion alone, no codec: the floor any codec round trip is measured against.
    const Planes back = fromRgb(rgb, planes.width, planes.height);
    std::printf("asset %lld: %dx%d, %zu bytes; RGB<->YCbCr without a codec: Y %.2f dB, Cb %.2f dB, Cr %.2f dB\n", id,
                planes.width, planes.height, stream.size(), psnr(planes.y, back.y), psnr(planes.cb, back.cb),
                psnr(planes.cr, back.cr));
    std::printf("%d %d\n", planes.width, planes.height);
    return 0;
}

int encodeCommand(pyrowave_device device, const std::string& in, const std::string& ref)
{
    const std::vector<uint8_t> original = readFile(ref + ".pyrowave");
    const std::vector<uint8_t> refYuv = readFile(ref + ".yuv");
    int w = 0;
    int h = 0;
    if (!xrpilot::GpuDecoder::frameSize(original.data(), original.size(), w, h))
        return std::fprintf(stderr, "FAIL no reference frame %s\n", ref.c_str()), 1;
    const std::vector<uint8_t> raw = readFile(in);
    if (raw.size() != size_t(w) * size_t(h) * 3 * sizeof(float))
        return std::fprintf(stderr, "FAIL %s is %zu bytes, not %dx%d RGB float\n", in.c_str(), raw.size(), w, h), 1;
    std::vector<float> rgb(raw.size() / sizeof(float));
    std::memcpy(rgb.data(), raw.data(), raw.size());
    Planes planes = fromRgb(rgb, w, h);

    pyrowave_encoder encoder = nullptr;
    const pyrowave_encoder_create_info info = {device, w, h, PYROWAVE_CHROMA_SUBSAMPLING_420};
    if (pyrowave_encoder_create(&info, &encoder) != PYROWAVE_SUCCESS)
        return std::fprintf(stderr, "FAIL encoder\n"), 1;
    const pyrowave_cpu_buffer buffer = cpuBuffer(planes);
    const pyrowave_rate_control rate = {original.size()};
    size_t packets = 0;
    std::vector<uint8_t> stream(original.size() * 2 + 4096);
    std::vector<pyrowave_packet> layout(1);
    const size_t boundary = stream.size();
    bool ok = pyrowave_encoder_encode_cpu_synchronous(encoder, &buffer, &rate) == PYROWAVE_SUCCESS &&
              pyrowave_encoder_compute_num_packets(encoder, boundary, &packets) == PYROWAVE_SUCCESS;
    layout.resize(std::max<size_t>(packets, 1));
    size_t outPackets = 0;
    ok = ok && pyrowave_encoder_packetize(encoder, layout.data(), boundary, &outPackets, stream.data(), stream.size()) ==
                   PYROWAVE_SUCCESS;
    pyrowave_encoder_destroy(encoder);
    if (!ok || outPackets != 1)
        return std::fprintf(stderr, "FAIL encode (%zu packets)\n", outPackets), 1;
    stream.assign(stream.begin() + std::ptrdiff_t(layout[0].offset),
                  stream.begin() + std::ptrdiff_t(layout[0].offset + layout[0].size));
    Planes again;
    if (!decode(device, stream, again))
        return std::fprintf(stderr, "FAIL the re-encoded frame does not decode\n"), 1;
    const size_t ySize = size_t(w) * size_t(h);
    const size_t cSize = ySize / 4;
    const std::vector<uint8_t> refY(refYuv.begin(), refYuv.begin() + std::ptrdiff_t(ySize));
    const std::vector<uint8_t> refCb(refYuv.begin() + std::ptrdiff_t(ySize), refYuv.begin() + std::ptrdiff_t(ySize + cSize));
    const std::vector<uint8_t> refCr(refYuv.begin() + std::ptrdiff_t(ySize + cSize), refYuv.end());
    std::printf("re-encoded %zu bytes (budget %zu); against the first decode: Y %.2f dB, Cb %.2f dB, Cr %.2f dB\n",
                stream.size(), original.size(), psnr(refY, again.y), psnr(refCb, again.cb), psnr(refCr, again.cr));
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        std::fprintf(stderr, "usage: trace_roundtrip decode <db> <asset_id|latest> <out> | encode <in.rgbf32> <ref>\n");
        return 2;
    }
    pyrowave_device device = nullptr;
    if (pyrowave_create_default_device(&device) != PYROWAVE_SUCCESS)
        return std::fprintf(stderr, "FAIL no PyroWave device\n"), 1;
    const std::string mode = argv[1];
    int rc = 2;
    if (mode == "decode" && argc == 5)
        rc = decodeCommand(device, argv[2], argv[3], argv[4]);
    else if (mode == "encode" && argc == 4)
        rc = encodeCommand(device, argv[2], argv[3]);
    pyrowave_device_destroy(device);
    return rc;
}

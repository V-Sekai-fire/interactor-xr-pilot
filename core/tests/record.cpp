// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// .pwrec recording tests; one case per ctest entry.

#include "xrpilot/StreamRecord.h"

#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>

using namespace xrpilot;

namespace
{

int failures = 0;

void check(bool ok, const char* what)
{
    if (!ok)
    {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

std::vector<StreamFrame> sample()
{
    std::vector<StreamFrame> frames;
    for (uint32_t i = 0; i < 3; ++i)
    {
        StreamFrame f;
        f.presentationNs = 1'000'000'000'000LL + int64_t(i) * 11'111'111;
        f.receiveNs = f.presentationNs + 4'000'000;
        f.frameIndex = 40 + i;
        for (uint32_t b = 0; b < 1000 + i * 7; ++b)
            f.payload.push_back(uint8_t(b * 31 + i));
        frames.push_back(f);
    }
    return frames;
}

std::string path(const char* name)
{
    return std::string(RECORD_TEST_DIR) + "/" + name;
}

std::vector<StreamFrame> readAll(const std::string& p, std::string& error)
{
    std::vector<StreamFrame> out;
    StreamRecordReader r;
    if (!r.open(p, &error))
        return out;
    StreamFrame f;
    while (r.next(f, &error))
        out.push_back(f);
    return out;
}

const std::map<std::string, std::function<void()>> cases = {
    {"record.round-trip",
     [] {
         const std::vector<StreamFrame> in = sample();
         StreamRecordWriter w;
         check(w.open(path("rt.pwrec")), "the recording opens");
         for (const StreamFrame& f : in)
             check(w.write(f), "each frame writes");
         check(w.close() && w.frames() == 3, "it closes with three frames");
         std::string error;
         const std::vector<StreamFrame> out = readAll(path("rt.pwrec"), error);
         check(error.empty() && out.size() == 3, "three frames read back with no error");
         for (size_t i = 0; i < out.size() && i < in.size(); ++i)
             check(out[i].presentationNs == in[i].presentationNs && out[i].receiveNs == in[i].receiveNs &&
                       out[i].frameIndex == in[i].frameIndex && out[i].payload == in[i].payload,
                   "each frame comes back byte for byte with its times");
     }},
    {"record.damage-is-named",
     [] {
         StreamRecordWriter w;
         w.open(path("cut.pwrec"));
         for (const StreamFrame& f : sample())
             w.write(f);
         w.close();
         std::FILE* f = std::fopen(path("cut.pwrec").c_str(), "rb");
         std::vector<uint8_t> bytes(1 << 16);
         bytes.resize(std::fread(bytes.data(), 1, bytes.size(), f));
         std::fclose(f);
         // A file cut inside the last payload: two frames read, then an error, never three.
         f = std::fopen(path("cut2.pwrec").c_str(), "wb");
         std::fwrite(bytes.data(), 1, bytes.size() - 5, f);
         std::fclose(f);
         std::string error;
         const std::vector<StreamFrame> two = readAll(path("cut2.pwrec"), error);
         check(two.size() == 2 && !error.empty(), "a cut payload stops at two frames and says so");
         // Another file under the name: refused at open.
         f = std::fopen(path("png.pwrec").c_str(), "wb");
         std::fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
         std::fclose(f);
         error.clear();
         check(readAll(path("png.pwrec"), error).empty() && !error.empty(), "a file of another kind is refused");
         // Control: the intact file reads clean, so the damage above is what the errors came from.
         error.clear();
         check(readAll(path("cut.pwrec"), error).size() == 3 && error.empty(), "control: the intact file reads all three");
     }},
};

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2 || cases.count(argv[1]) == 0)
    {
        std::fprintf(stderr, "usage: xrpilot-record-tests <case>\n");
        return 2;
    }
    cases.at(argv[1])();
    return failures == 0 ? 0 : 1;
}

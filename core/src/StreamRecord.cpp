// SPDX-License-Identifier: Apache-2.0 OR MIT

#include "xrpilot/StreamRecord.h"

#include <cstring>

namespace xrpilot
{

namespace
{

constexpr uint8_t Magic[8] = {'P', 'W', 'R', 'E', 'C', 0, 0, 1};
constexpr uint32_t MaxPayload = 256u * 1024u * 1024u;

void put(std::vector<uint8_t>& out, uint64_t v, int bytes)
{
    for (int i = 0; i < bytes; ++i)
        out.push_back(uint8_t(v >> (8 * i)));
}

uint64_t get(const uint8_t* p, int bytes)
{
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i)
        v |= uint64_t(p[i]) << (8 * i);
    return v;
}

} // namespace

StreamRecordWriter::~StreamRecordWriter()
{
    close();
}

bool StreamRecordWriter::open(const std::string& path)
{
    close();
    file_ = std::fopen(path.c_str(), "wb");
    ok_ = file_ != nullptr && std::fwrite(Magic, 1, sizeof(Magic), file_) == sizeof(Magic);
    frames_ = 0;
    return ok_;
}

bool StreamRecordWriter::write(const StreamFrame& frame)
{
    if (file_ == nullptr || frame.payload.size() > MaxPayload)
        return ok_ = false;
    std::vector<uint8_t> head;
    put(head, uint64_t(frame.presentationNs), 8);
    put(head, uint64_t(frame.receiveNs), 8);
    put(head, frame.frameIndex, 4);
    put(head, frame.payload.size(), 4);
    ok_ = ok_ && std::fwrite(head.data(), 1, head.size(), file_) == head.size() &&
          std::fwrite(frame.payload.data(), 1, frame.payload.size(), file_) == frame.payload.size();
    frames_ += ok_ ? 1 : 0;
    return ok_;
}

bool StreamRecordWriter::close()
{
    if (file_ == nullptr)
        return ok_;
    ok_ = std::fclose(file_) == 0 && ok_;
    file_ = nullptr;
    return ok_;
}

StreamRecordReader::~StreamRecordReader()
{
    if (file_ != nullptr)
        std::fclose(file_);
}

bool StreamRecordReader::open(const std::string& path, std::string* error)
{
    file_ = std::fopen(path.c_str(), "rb");
    uint8_t magic[8];
    if (file_ == nullptr)
        return *error = "cannot open " + path, false;
    if (std::fread(magic, 1, 8, file_) != 8 || std::memcmp(magic, Magic, 8) != 0)
        return *error = path + " is not a .pwrec recording", false;
    return true;
}

bool StreamRecordReader::next(StreamFrame& frame, std::string* error)
{
    uint8_t head[24];
    const size_t got = std::fread(head, 1, sizeof(head), file_);
    if (got == 0)
        return false;
    if (got != sizeof(head))
        return *error = "a record header is cut short", false;
    frame.presentationNs = int64_t(get(head, 8));
    frame.receiveNs = int64_t(get(head + 8, 8));
    frame.frameIndex = uint32_t(get(head + 16, 4));
    const uint32_t size = uint32_t(get(head + 20, 4));
    if (size > MaxPayload)
        return *error = "a record claims " + std::to_string(size) + " bytes", false;
    frame.payload.resize(size);
    if (std::fread(frame.payload.data(), 1, size, file_) != size)
        return *error = "frame " + std::to_string(frame.frameIndex) + " is cut short", false;
    return true;
}

} // namespace xrpilot

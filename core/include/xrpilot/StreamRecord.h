// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// The runtime's video stream kept as it arrived: every OXRSys frame is a whole PyroWave frame, so a
// recording costs a file write and no encode, and pyro2cfhd turns it into CineForm afterwards.
//
// A .pwrec file is the 8-byte magic "PWREC\0\0\1", then one record per frame, all little-endian:
//   u64 presentation ns, u64 receive ns, u32 frame index, u32 payload bytes, payload.

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace xrpilot
{

struct StreamFrame
{
    int64_t presentationNs = 0;
    int64_t receiveNs = 0;
    uint32_t frameIndex = 0;
    std::vector<uint8_t> payload;
};

class StreamRecordWriter
{
public:
    ~StreamRecordWriter();
    bool open(const std::string& path);
    bool write(const StreamFrame& frame);
    // Flushes and closes; false if any write failed.
    bool close();
    bool isOpen() const { return file_ != nullptr; }
    uint64_t frames() const { return frames_; }

private:
    std::FILE* file_ = nullptr;
    bool ok_ = true;
    uint64_t frames_ = 0;
};

class StreamRecordReader
{
public:
    ~StreamRecordReader();
    bool open(const std::string& path, std::string* error);
    // False at the clean end of the file; a record cut short sets error.
    bool next(StreamFrame& frame, std::string* error);

private:
    std::FILE* file_ = nullptr;
};

} // namespace xrpilot

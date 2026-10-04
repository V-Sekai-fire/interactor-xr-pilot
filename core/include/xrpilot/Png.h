// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// A minimal PNG writer: RGBA8, zlib stored blocks (no compression), so no codec dependency.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xrpilot
{

std::vector<uint8_t> encodePng(const uint8_t* rgba, int width, int height);
bool writePng(const std::string& path, const uint8_t* rgba, int width, int height);

uint32_t crc32(const uint8_t* data, size_t size, uint32_t crc = 0);

} // namespace xrpilot

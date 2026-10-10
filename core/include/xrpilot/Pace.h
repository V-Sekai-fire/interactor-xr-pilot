// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// Sleeps for loops that pace at a headset's refresh rate. A plain sleep on Windows wakes on the 15.6 ms
// system tick unless the process raised its timer resolution, which turns a 90 Hz loop into bursts.

#pragma once

#include <cstdint>

namespace xrpilot
{

// Sleeps the calling thread for about ns nanoseconds, to well under a millisecond on every platform.
void sleepPreciseNs(int64_t ns);

} // namespace xrpilot

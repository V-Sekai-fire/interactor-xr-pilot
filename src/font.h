// SPDX-License-Identifier: Apache-2.0 OR MIT
#pragma once

namespace panelspun::detail {

inline constexpr const char* kDefaultFont = "Inter";

// Registers the bundled Inter Regular with ThorVG under kDefaultFont.
bool registerDefaultFont();

}

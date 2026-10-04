// SPDX-License-Identifier: Apache-2.0 OR MIT
#include "font.h"

#include <thorvg.h>

#include <cstddef>
#include <cstdint>

namespace panelspun::embedded {
extern const unsigned char inter_regular_ttf[];
extern const std::size_t inter_regular_ttf_size;
}

namespace panelspun::detail {

bool registerDefaultFont() {
    return tvg::Text::load(kDefaultFont, reinterpret_cast<const char*>(embedded::inter_regular_ttf),
                           static_cast<std::uint32_t>(embedded::inter_regular_ttf_size), "ttf", false) ==
           tvg::Result::Success;
}

}

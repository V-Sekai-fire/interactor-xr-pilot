// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// A small JSON reader and writer that keeps object members in file order and numbers as written, so
// a settings file another program owns is rewritten with only the change made to it.

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace xrpilot
{

struct Json
{
    enum class Kind
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object,
    };

    Kind kind = Kind::Null;
    bool boolean = false;
    std::string text; // a string's value, or a number exactly as written
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> members;

    static Json string(std::string value);

    // The member named key, or null when absent or not an object.
    const Json* find(const std::string& key) const;
    Json* find(const std::string& key);
    // Replaces the member in place, or appends it.
    void set(const std::string& key, Json value);
    bool erase(const std::string& key);
    // The string value, or empty for any other kind.
    const std::string& str() const;
};

// Parses the whole of text; false when it is not one JSON value.
bool parseJson(const std::string& text, Json& out);
// Four-space indented, members in their stored order.
std::string writeJson(const Json& value);

} // namespace xrpilot

// SPDX-License-Identifier: Apache-2.0 OR MIT
//
// The stream's bitrate and adaptive bitrate as named tracks to pick from, not numbers to type, and the edit
// that writes a pick into the OXRSys runtime's config file (oxrsys-runtime.toml, [streaming]).

#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace xrpilot
{

struct BitrateTrack
{
    const char* name;
    uint32_t mbps; // bitrate_mbps
};

// Ordered from least to most; the runtime accepts 1 to 200 Mbps.
inline const std::vector<BitrateTrack>& bitrateTracks()
{
    static const std::vector<BitrateTrack> tracks = {
        {"Low", 25}, {"Medium", 50}, {"High", 100}, {"Very high", 150}, {"Highest", 200},
    };
    return tracks;
}

struct AbrTrack
{
    const char* name;
    const char* mode; // abr_mode
};

inline const std::vector<AbrTrack>& abrTracks()
{
    static const std::vector<AbrTrack> tracks = {
        {"Fixed", "off"},
        {"Adapt the bitrate", "bitrate"},
        {"Adapt the bitrate and resolution", "full"},
    };
    return tracks;
}

// The track a configured bitrate falls on: the nearest, the lower one on a tie.
inline size_t bitrateTrackFor(uint32_t mbps)
{
    const std::vector<BitrateTrack>& tracks = bitrateTracks();
    size_t best = 0;
    for (size_t i = 1; i < tracks.size(); ++i)
    {
        const auto gap = [&](size_t t) { return std::labs(long(tracks[t].mbps) - long(mbps)); };
        if (gap(i) < gap(best))
            best = i;
    }
    return best;
}

// The track for an abr_mode; the runtime reads anything it does not know as "bitrate".
inline size_t abrTrackFor(const std::string& mode)
{
    const std::vector<AbrTrack>& tracks = abrTracks();
    for (size_t i = 0; i < tracks.size(); ++i)
        if (mode == tracks[i].mode)
            return i;
    return 1;
}

namespace toml_edit
{

inline std::string trim(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos)
        return {};
    return s.substr(a, s.find_last_not_of(" \t\r") - a + 1);
}

// "[name]" for a section header line, else empty.
inline std::string sectionOf(const std::string& line)
{
    const std::string t = trim(line);
    if (t.size() < 2 || t.front() != '[' || t.back() != ']')
        return {};
    return trim(t.substr(1, t.size() - 2));
}

inline bool isKey(const std::string& line, const std::string& key)
{
    const std::string t = trim(line);
    const size_t eq = t.find('=');
    return eq != std::string::npos && t[0] != '#' && trim(t.substr(0, eq)) == key;
}

inline std::vector<std::string> lines(const std::string& text)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (start < text.size())
    {
        size_t end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        out.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

} // namespace toml_edit

// The value of key in [section], unquoted, or empty when it is not set.
inline std::string tomlValue(const std::string& text, const std::string& section, const std::string& key)
{
    using namespace toml_edit;
    std::string current;
    for (const std::string& line : lines(text))
    {
        const std::string s = sectionOf(line);
        if (!s.empty())
        {
            current = s;
            continue;
        }
        if (current != section || !isKey(line, key))
            continue;
        std::string v = trim(line.substr(line.find('=') + 1));
        if (!v.empty() && v[0] == '"')
        {
            const size_t close = v.find('"', 1);
            return close == std::string::npos ? v.substr(1) : v.substr(1, close - 1);
        }
        const size_t hash = v.find('#');
        return trim(hash == std::string::npos ? v : v.substr(0, hash));
    }
    return {};
}

// The text with key in [section] set to value (already TOML: quote strings yourself). Every other line,
// comments included, is kept; a missing key goes at the end of its section, a missing section at the end.
inline std::string setTomlValue(const std::string& text, const std::string& section, const std::string& key,
                                const std::string& value)
{
    using namespace toml_edit;
    const std::vector<std::string> in = lines(text);
    std::vector<std::string> out;
    const std::string line = key + " = " + value;
    std::string current;
    bool done = false;
    size_t sectionEnd = std::string::npos; // where the section's last non-blank line ends
    for (const std::string& l : in)
    {
        const std::string s = sectionOf(l);
        if (!s.empty())
        {
            if (current == section && !done && sectionEnd != std::string::npos)
            {
                out.insert(out.begin() + long(sectionEnd), line);
                done = true;
            }
            current = s;
            out.push_back(l);
            if (current == section)
                sectionEnd = out.size();
            continue;
        }
        if (current == section && !done && isKey(l, key))
        {
            out.push_back(line);
            done = true;
            continue;
        }
        out.push_back(l);
        if (current == section && !trim(l).empty())
            sectionEnd = out.size();
    }
    if (!done)
    {
        if (current == section && sectionEnd != std::string::npos)
            out.insert(out.begin() + long(sectionEnd), line);
        else
        {
            if (!out.empty() && !trim(out.back()).empty())
                out.push_back("");
            out.push_back("[" + section + "]");
            out.push_back(line);
        }
    }
    std::string result;
    for (const std::string& l : out)
        result += l + "\n";
    return result;
}

} // namespace xrpilot

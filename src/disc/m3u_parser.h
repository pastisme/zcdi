#pragma once
#include <string>
#include <vector>

namespace zcdi {

struct M3uEntry {
    std::string path; // as written, relative to the playlist's folder unless absolute
    std::string label; // after '|' (libretro convention); empty if none
};

// Parses m3u text, not a path: one image per line, '#' lines skipped.
bool parse_m3u(const std::string& text, std::vector<M3uEntry>& out);

// `entry` resolved against the folder of `m3u_path`, unless absolute.
std::string m3u_entry_path(const std::string& m3u_path, const std::string& entry);

} // namespace zcdi

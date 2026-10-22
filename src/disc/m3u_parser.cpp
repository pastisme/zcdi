#include "disc/m3u_parser.h"

#include <sstream>

#include "disc/file_io.h"

namespace zcdi {

namespace {
std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    auto a = s.find_first_not_of(ws);
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(ws) - a + 1);
}
// "C:\..." or "\\server\..." also counts, whatever the host OS.
bool is_absolute(const std::string& p) {
    if (p.empty()) return false;
    if (p[0] == '/' || p[0] == '\\') return true;
    return p.size() >= 2 && p[1] == ':';
}
} // namespace

bool parse_m3u(const std::string& text, std::vector<M3uEntry>& out) {
    out.clear();
    std::istringstream in(text);
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        // UTF-8 BOM, as Windows editors write.
        if (first && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
        first = false;
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        M3uEntry entry;
        auto bar = line.find('|');
        if (bar != std::string::npos) {
            entry.label = trim(line.substr(bar + 1));
            line = trim(line.substr(0, bar));
            if (line.empty()) continue;
        }
        entry.path = line;
        out.push_back(entry);
    }
    return !out.empty();
}

std::string m3u_entry_path(const std::string& m3u_path, const std::string& entry) {
    if (is_absolute(entry)) return entry;
    return (disc_generic_path(m3u_path).parent_path() / disc_generic_path(entry)).lexically_normal().string();
}

} // namespace zcdi

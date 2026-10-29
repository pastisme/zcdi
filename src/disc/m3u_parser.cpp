#include "disc/m3u_parser.h"

#include <sstream>

#include "core/util.h"

namespace zcdi {

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

} // namespace zcdi

#include "symbols.h"

#include <cctype>
#include <cstdio>
#include <fstream>

namespace {

// Six or more hex digits at `s`, as an address; -1 if none.
long parse_hex_address(const std::string& s, std::size_t pos, std::size_t* end) {
    std::size_t i = pos;
    while (i < s.size() && std::isxdigit(static_cast<unsigned char>(s[i])))
        ++i;
    if (i - pos < 6) return -1;
    if (end) *end = i;
    return std::stol(s.substr(pos, i - pos), nullptr, 16);
}

std::string trim(const std::string& s) {
    const std::size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return {};
    const std::size_t b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

} // namespace

bool Symbols::load(const std::string& path) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (header && line.rfind(";   ", 0) == 0) {
            // ";   4045f8 kernel         system": the module table.
            std::size_t end = 0;
            const long addr = parse_hex_address(line, 4, &end);
            if (addr >= 0 && end < line.size() && line[end] == ' ') {
                const std::string rest = trim(line.substr(end));
                modules_[static_cast<uint32_t>(addr)] = rest.substr(0, rest.find(' '));
            }
            continue;
        }
        if (line.rfind("; ---- ", 0) == 0) {
            header = false;
            const std::size_t sub = line.find("sub_");
            if (sub == std::string::npos) continue;
            const long addr = parse_hex_address(line, sub + 4, nullptr);
            if (addr >= 0) entries_[static_cast<uint32_t>(addr)] = trim(line.substr(7));
            continue;
        }
        // "  40076a  41fa fe89   lea ...  ; note": address, maybe "/offset".
        if (line.size() < 10 || line[0] != ' ' || line[1] != ' ') continue;
        header = false;
        std::size_t end = 0;
        const long addr = parse_hex_address(line, 2, &end);
        if (addr < 0) continue;
        // Notes follow "  | ", or "  ; " except on dc.b lines (ASCII column).
        std::size_t at = line.find("  | ", end);
        if (at == std::string::npos && line.find("dc.", end) == std::string::npos) at = line.find("  ; ", end);
        if (at == std::string::npos) continue;
        const std::string note = trim(line.substr(at + 4));
        if (!note.empty()) notes_[static_cast<uint32_t>(addr)] = note;
    }
    return true;
}

std::string Symbols::where(uint32_t addr) const {
    auto it = modules_.upper_bound(addr);
    if (it == modules_.begin()) return {};
    --it;
    // Modules are under 1 MB; past the last one is not inside it.
    if (addr - it->first >= 0x100000) return {};
    char buf[96]{};
    std::snprintf(buf, sizeof(buf), "%s+$%x", it->second.c_str(), static_cast<unsigned>(addr - it->first));
    return buf;
}

const std::string* Symbols::entry(uint32_t addr) const {
    const auto it = entries_.find(addr);
    return it == entries_.end() ? nullptr : &it->second;
}

const std::string* Symbols::note(uint32_t addr) const {
    const auto it = notes_.find(addr);
    return it == notes_.end() ? nullptr : &it->second;
}

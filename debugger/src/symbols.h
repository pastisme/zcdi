#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>

// Module table, sub_ entries and notes from os9dasm .asm listings.
class Symbols {
public:
    // False if the file can't be read. Later files add to earlier ones.
    bool load(const std::string& path);
    bool empty() const { return modules_.empty() && notes_.empty(); }

    // "kernel+$1a2" for an address inside a listed module, else "".
    std::string where(uint32_t addr) const;
    // The entry header ("sub_40063a  called from ..."), or nullptr.
    const std::string* entry(uint32_t addr) const;
    // The hand notes on that address, or nullptr. Several are joined by " / ".
    const std::string* note(uint32_t addr) const;

private:
    std::map<uint32_t, std::string> modules_; // start -> name
    std::unordered_map<uint32_t, std::string> entries_;
    std::unordered_map<uint32_t, std::string> notes_;
};

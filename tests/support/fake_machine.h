#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "core/machine.h"
#include "core/state_archive.h"

namespace zcdi_test {

// A counter loop storing to RAM. One Machine at a time (Musashi singleton).
inline std::vector<std::byte> fake_bios(uint8_t variant = 0) {
    std::vector<std::byte> rom(0x80000, std::byte{0});
    const uint8_t code[] = {
        0x00, 0x07, 0x00, 0x00,  // reset SSP $70000
        0x00, 0x40, 0x00, 0x08,  // reset PC $400008
        0x52, 0x80,              // loop: addq.l #1,d0
        0x21, 0xc0, 0x10, 0x00,  //   move.l d0,$1000.w
        0x60, 0xf8,              //   bra.s loop
    };
    for (size_t i = 0; i < sizeof(code); ++i) rom[i] = std::byte{code[i]};
    rom[0x100] = std::byte{variant};  // a different BIOS, same program
    return rom;
}

inline std::unique_ptr<zcdi::Machine> build_fake_machine(uint8_t variant = 0) {
    std::vector<std::byte> bios = fake_bios(variant);
    std::unique_ptr<zcdi::Machine> m = std::make_unique<zcdi::Machine>(bios, zcdi::VideoStandard::kPal);
    m->cold_boot();
    return m;
}

inline std::vector<uint8_t> save_machine(zcdi::Machine& m) {
    std::vector<uint8_t> out;
    zcdi::StateWriter w(out);
    m.save_state(w);
    return out;
}

inline bool load_machine(zcdi::Machine& m, const std::vector<uint8_t>& bytes) {
    zcdi::StateReader r(bytes);
    return m.load_state(r);
}

// Zeroes the payload of every section tagged `tag` (e.g. "RTC "), for
// comparing two states that may differ only there.
inline void blank_section(std::vector<uint8_t>& state, const char (&tag)[5]) {
    for (size_t i = 0; i + 8 <= state.size(); ++i) {
        if (std::memcmp(&state[i], tag, 4) != 0) continue;
        uint32_t len = uint32_t(state[i + 4]) | uint32_t(state[i + 5]) << 8 |
                       uint32_t(state[i + 6]) << 16 | uint32_t(state[i + 7]) << 24;
        if (i + 8 + len > state.size()) continue;
        std::memset(&state[i + 8], 0, len);
        i += 7 + len;
    }
}

}  // namespace zcdi_test

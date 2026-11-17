#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

#include "core/machine.h"
#include "core/state_archive.h"

namespace zcdi_test {

// A 512 KiB BIOS ROM holding `code` from offset 0: the reset SSP and PC
// (0x400008 is the byte after them), then the program.
inline std::vector<std::byte> bios_with_program(std::span<const uint8_t> code) {
    std::vector<std::byte> rom(0x80000, std::byte{0});
    for (std::size_t i = 0; i < code.size(); ++i) rom[i] = std::byte{code[i]};
    return rom;
}

// A counter loop storing to RAM. One Machine at a time (Musashi singleton).
inline std::vector<std::byte> fake_bios(uint8_t variant = 0) {
    const uint8_t code[] = {
        0x00, 0x07, 0x00, 0x00,  // reset SSP $70000
        0x00, 0x40, 0x00, 0x08,  // reset PC $400008
        0x52, 0x80,              // loop: addq.l #1,d0
        0x21, 0xc0, 0x10, 0x00,  //   move.l d0,$1000.w
        0x60, 0xf8,              //   bra.s loop
    };
    std::vector<std::byte> rom = bios_with_program(code);
    rom[0x100] = std::byte{variant};  // a different BIOS, same program
    return rom;
}

// Cold-booted, PAL.
inline std::unique_ptr<zcdi::Machine> build_fake_machine_with(const zcdi::Machine::InitParams& params,
                                                              uint8_t variant = 0) {
    std::unique_ptr<zcdi::Machine> m =
        std::make_unique<zcdi::Machine>(fake_bios(variant), zcdi::VideoStandard::kPal, params);
    m->cold_boot();
    return m;
}

inline std::unique_ptr<zcdi::Machine> build_fake_machine(uint8_t variant = 0) {
    return build_fake_machine_with({}, variant);
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

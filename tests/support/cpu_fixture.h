#pragma once

#include <cstdint>
#include <initializer_list>

#include "core/board.h"
#include "core/cpu/musashi_core.h"

namespace zcdi_test {

// One at a time: Musashi is a process-wide singleton.
struct CpuFixture {
    static constexpr uint32_t kInitialSsp = 0x00070000;
    static constexpr uint32_t kCodePc = 0x00001000;
    static constexpr uint32_t kHandlerPc = 0x00002000;
    static constexpr uint32_t kNormalPc = 0x00003000;
    static constexpr uint16_t kBraSelf = 0x60fe;  // BRA.S *-2

    static constexpr int kBusErrorVector = 2;
    static constexpr int kAddressErrorVector = 3;

    zcdi::Board board;
    zcdi::MusashiCore cpu{board};

    CpuFixture() {
        board.write32(0x00000000, kInitialSsp);
        board.write32(0x00000004, kCodePc);
        board.write16(kNormalPc, kBraSelf);
    }

    void park_vector(int vector) {
        board.write32(uint32_t(vector) * 4, kHandlerPc);
        board.write16(kHandlerPc, kBraSelf);
    }

    // Writes 16-bit words at kCodePc onwards.
    void code(std::initializer_list<uint16_t> words) {
        uint32_t a = kCodePc;
        for (uint16_t w : words) {
            board.write16(a, w);
            a += 2;
        }
    }

    void reset_and_run(int cycles = 200) {
        cpu.reset();
        cpu.run(cycles);
    }

    // Field offsets of the SCC68070 Format 15 frame, from the stack pointer.
    uint16_t frame_vector_word() { return board.read16(cpu.sp() + 6); }
    uint32_t frame_fault_address() { return board.read32(cpu.sp() + 20); }
};

// Word helpers for code(): split a 32-bit absolute address or immediate.
constexpr uint16_t hi16(uint32_t v) { return uint16_t(v >> 16); }
constexpr uint16_t lo16(uint32_t v) { return uint16_t(v); }

}  // namespace zcdi_test

#pragma once
// MCD212 registers as software sets them: MMIO words, and the registers ICA/DCA
// instructions load (set_register, the instructions' own entry point).
#include <cstdint>

#include "chips/mcd212/mcd212.h"
#include "support/mcd212_private.h"

namespace zcdi_test {

// `offset` from 0x4fffe0, e.g. mcd212::kDcr1.
inline void write_mmio16(zcdi::chips::Mcd212& mcd, uint32_t offset, uint16_t value) {
    mcd.write_byte(offset, uint8_t(value >> 8));
    mcd.write_byte(offset + 1, uint8_t(value));
}

// As an ICA/DCA LOAD instruction on `path` (0 = plane A) would.
inline void load_register(zcdi::chips::Mcd212& mcd, int path, uint8_t reg, uint32_t value) {
    call_private::set_register(mcd, path, reg, value);
}

// CLUT entry `index` of `bank` (64 entries each), loaded through plane A.
inline void load_clut(zcdi::chips::Mcd212& mcd, int bank, int index, uint32_t color) {
    load_register(mcd, 0, zcdi::chips::mcd212::kRegClutBank, uint32_t(bank));
    load_register(mcd, 0, uint8_t(zcdi::chips::mcd212::kRegClut + index), color);
}

}  // namespace zcdi_test

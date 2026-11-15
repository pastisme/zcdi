#pragma once
// MCD212 registers as software sets them: MMIO words, and the registers ICA/DCA
// instructions load (set_register, the instructions' own entry point).
#include <cstdint>

#include "chips/mcd212/mcd212.h"
#include "support/private_access.h"

ZCDI_PRIVATE_MEMBER(Mcd212SetRegister, zcdi::chips::Mcd212, set_register, void(int, uint8_t, uint32_t));

namespace zcdi_test {

// `offset` from 0x4fffe0, e.g. mcd212::kDcr1.
inline void write_mmio16(zcdi::chips::Mcd212& mcd, uint32_t offset, uint16_t value) {
    mcd.write_byte(offset, uint8_t(value >> 8));
    mcd.write_byte(offset + 1, uint8_t(value));
}

// As an ICA/DCA LOAD instruction on `path` (0 = plane A) would.
inline void load_register(zcdi::chips::Mcd212& mcd, int path, uint8_t reg, uint32_t value) {
    priv<Mcd212SetRegister>(mcd, path, reg, value);
}

}  // namespace zcdi_test

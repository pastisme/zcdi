#pragma once

#include <cstdint>

#include "chips/slave/slave.h"
#include "core/board.h"

namespace zcdi_test {

constexpr uint32_t kSlaveBase = zcdi::address_map::kSlave.base;

// The odd (data) byte of SLAVE channel `channel`.
constexpr uint32_t channel_addr(int channel) { return kSlaveBase + uint32_t(channel) * 2 + 1; }

// One 8.19 ms output compare tick (CPU cycles): 16 ticks of the 0.5 MHz counter's high byte.
constexpr long kOcTick = 15000000L * 256 / 500000 * 16;

// `cycles` CPU cycles, in 1000-cycle steps.
inline void run(zcdi::chips::SlaveHle& s, long cycles) {
    for (long done = 0; done < cycles; done += 1000) s.tick_cycles(1000);
}

}  // namespace zcdi_test

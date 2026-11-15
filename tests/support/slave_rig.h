#pragma once

#include <cstdint>

#include "core/board.h"

namespace zcdi_test {

constexpr uint32_t kSlaveBase = zcdi::address_map::kSlave.base;

// The odd (data) byte of SLAVE channel `channel`.
constexpr uint32_t channel_addr(int channel) { return kSlaveBase + uint32_t(channel) * 2 + 1; }

}  // namespace zcdi_test

#pragma once

#include <cstdint>

namespace zcdi_test {

constexpr uint32_t kSlaveBase = 0x310000;

// The odd (data) byte of SLAVE channel `channel`.
constexpr uint32_t channel_addr(int channel) { return kSlaveBase + uint32_t(channel) * 2 + 1; }

}  // namespace zcdi_test

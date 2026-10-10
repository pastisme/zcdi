#pragma once

#include <array>
#include <cstddef>

#include "core/bus.h"

namespace zcdi_test {

// The cartridge RAM at 0xd00000 exists only while a cartridge ROM is loaded.
inline void insert_dvc_cart(zcdi::Bus& bus) {
    const std::array<std::byte, 16> rom{};
    bus.load_dvc(rom);
}

}  // namespace zcdi_test

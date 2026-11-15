#pragma once

#include <array>
#include <cstddef>

#include "core/board.h"

namespace zcdi_test {

// The cartridge RAM at 0xd00000 exists only while a cartridge ROM is loaded.
inline void insert_dvc_cart(zcdi::Board& board) {
    const std::array<std::byte, 16> rom{};
    board.load_dvc(rom);
}

}  // namespace zcdi_test

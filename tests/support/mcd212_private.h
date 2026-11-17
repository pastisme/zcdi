#pragma once
// Mcd212's decoders, mixer and register loads, for tests of one part at a time.
#include <cstddef>
#include <cstdint>

#include "chips/mcd212/mcd212.h"
#include "support/access_private.h"

ACCESS_PRIVATE_FUN(zcdi::chips::Mcd212, void(int, uint8_t, uint32_t), set_register)
ACCESS_PRIVATE_FUN(zcdi::chips::Mcd212, uint32_t(int, uint8_t, uint8_t) const, clut_pixel)
ACCESS_PRIVATE_FUN(zcdi::chips::Mcd212, bool(int, uint32_t, int) const, plane_transparent)
ACCESS_PRIVATE_FUN(zcdi::chips::Mcd212, void(int, uint32_t*, bool*, int) const, apply_pixel_hold)
ACCESS_PRIVATE_FUN(zcdi::chips::Mcd212, void(int, const uint8_t*, std::size_t, uint32_t*, bool*), decode_plane_line)
ACCESS_PRIVATE_FUN(zcdi::chips::Mcd212, bool(int) const, plane_parked)
ACCESS_PRIVATE_FUN(zcdi::chips::Mcd212, void(int, const uint8_t*, std::size_t, uint32_t*, bool*),
                   decode_double_res_line)
ACCESS_PRIVATE_FUN(zcdi::chips::Mcd212, uint32_t(uint32_t, bool, uint32_t, bool, uint8_t, uint8_t) const,
                   composite_pixel)
ACCESS_PRIVATE_FUN(zcdi::chips::Mcd212, bool(int, bool) const, rgb555_transparent)
ACCESS_PRIVATE_FUN(zcdi::chips::Mcd212, void(), update_region_arrays)

namespace zcdi_test {
// The region arrays, per path and Double Resolution column (aliases: the macro can't spell array references).
using RegionWeights = uint8_t[2][zcdi::chips::Mcd212SaveState::kFbWidth];
using RegionFlags = bool[2][zcdi::chips::Mcd212SaveState::kFbWidth];
}  // namespace zcdi_test

ACCESS_PRIVATE_FIELD(zcdi::chips::Mcd212, zcdi_test::RegionWeights, weight_factor_line_)
ACCESS_PRIVATE_FIELD(zcdi::chips::Mcd212, zcdi_test::RegionFlags, region_flag_line_)
ACCESS_PRIVATE_FIELD(zcdi::chips::Mcd212, bool, region_pairs_uniform_)

namespace zcdi_test {

// Mcd212's saved state, writable.
inline zcdi::chips::Mcd212SaveState& state_of(zcdi::chips::Mcd212& mcd) {
    return saved_state<zcdi::chips::Mcd212SaveState>(mcd);
}

}  // namespace zcdi_test

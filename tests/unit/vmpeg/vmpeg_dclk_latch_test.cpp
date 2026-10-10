#include "doctest.h"

#include <cstdint>

#include "vmpeg/vmpeg.h"
#include "support/private_access.h"
#include "support/vmpeg_regs.h"

using zcdi::mamecdi::VmpegDevice;
using zcdi::mamecdi::VmpegSaveState;

// fma_dclk_ is private: the counter is advanced through the real
// tick_cycles() path, and only read back here to know what to expect.
ZCDI_PRIVATE_MEMBER(VmpegFmaDclk, VmpegSaveState, fma_dclk_, uint32_t);

TEST_SUITE("vmpeg") {

TEST_CASE("DCLK high/low reads return one consistent counter value") {
    VmpegDevice v;

    // Well past a 16-bit rollover, so the low word is nonzero.
    for (int i = 0; i < 5; ++i) v.tick_cycles(1'000'000);
    const uint32_t dclk = zcdi_test::priv<VmpegFmaDclk>(v);
    REQUIRE(dclk != 0);
    // A coincidental 0 low word would mask the bug.
    REQUIRE((dclk & 0xffffu) != 0);

    const uint16_t hi = zcdi_test::read16(v, zcdi_test::kFmaDclkOffset);
    const uint16_t lo = zcdi_test::read16(v, zcdi_test::kFmaDclkOffset + 2);
    CHECK(hi == uint16_t(dclk >> 16));
    CHECK(lo == uint16_t(dclk));
}

}  // TEST_SUITE

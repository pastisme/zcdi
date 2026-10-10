#include "doctest.h"

#include <cstdint>
#include <vector>

#include "mk48t08/mk48t08.h"
#include "support/state_roundtrip.h"

using zcdi::mamecdi::Mk48t08;

TEST_SUITE("mk48t08") {

TEST_CASE("a restored MK48T08 keeps its RAM and its clock") {
    Mk48t08 a;
    a.write(0x100, 0x5a);
    a.tick_cycles(15'000'000 * 90);  // 90 s after boot

    const std::vector<uint8_t> state = zcdi_test::save_state(a);
    Mk48t08 b;
    REQUIRE(zcdi_test::load_state(b, state));
    CHECK(zcdi_test::save_state(b) == state);
    CHECK(b.read(0x100) == 0x5a);
    for (uint32_t off = 0x1ff9; off <= 0x1fff; ++off) {
        CAPTURE(off);
        CHECK(b.read(off) == a.read(off));
    }
}

}  // TEST_SUITE

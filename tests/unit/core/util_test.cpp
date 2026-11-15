#include "doctest.h"

#include <cstdint>

#include "core/util.h"

using namespace zcdi;

TEST_SUITE("util") {

TEST_CASE("an even offset is a 16-bit register's high byte") {
    CHECK(byte_lane(0x1234, 0x10) == 0x12);
    CHECK(byte_lane(0x1234, 0x11) == 0x34);
    CHECK(set_byte_lane(0x1234, 0x10, 0xab) == 0xab34);
    CHECK(set_byte_lane(0x1234, 0x11, 0xab) == 0x12ab);
}

TEST_CASE("set_half replaces one 16-bit half") {
    CHECK(set_half(0x11223344u, 16, 0xaabb) == 0xaabb3344u);
    CHECK(set_half(0x11223344u, 0, 0xaabb) == 0x1122aabbu);
}

}  // TEST_SUITE

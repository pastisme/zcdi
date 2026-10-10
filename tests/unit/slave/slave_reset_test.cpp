// 0x8a on channel 2 (0x310005, cdichips mc6805slave.md).
#include "doctest.h"

#include <cstdint>

#include "slave/slave.h"
#include "support/slave_rig.h"

using zcdi_test::channel_addr;

TEST_SUITE("slave") {

TEST_CASE("0x8a on channel 2 requests a reset once") {
    zcdi::mamecdi::SlaveHle slave;
    CHECK_FALSE(slave.consume_reset_request());
    slave.write_byte(channel_addr(2), 0x8a);
    CHECK(slave.consume_reset_request());
    CHECK_FALSE(slave.consume_reset_request());
}

TEST_CASE("0x8a on other channels does not request a reset") {
    zcdi::mamecdi::SlaveHle slave;
    for (int channel : {0, 1, 3}) slave.write_byte(channel_addr(channel), 0x8a);
    CHECK_FALSE(slave.consume_reset_request());
}

}  // TEST_SUITE

// Channel 3 status queries and their channel 2 replies, as SLAVE firmware 2.0
// answers them (reply builder 0xf19) and sldriv parses them (cdi220b 0x432424).
#include "doctest.h"

#include <cstdint>

#include "slave/slave.h"
#include "support/slave_rig.h"

using zcdi::mamecdi::SlaveHle;
using zcdi_test::channel_addr;

namespace {

// Reads channel 2 until the 0xff idle byte, at most four bytes.
uint32_t read_reply(SlaveHle& slave) {
    uint32_t reply = 0;
    for (int i = 0; i < 4; ++i) {
        uint8_t b = slave.read_byte(channel_addr(2));
        if (b == 0xff) break;
        reply = reply << 8 | b;
    }
    return reply;
}

}  // namespace

TEST_SUITE("slave") {

TEST_CASE("0xf0 replies revision 2.0 and the servo byte") {
    SlaveHle slave;
    slave.write_byte(channel_addr(3), 0xf0);
    CHECK(read_reply(slave) == 0xf02000);
}

TEST_CASE("0xf5 replies the master level") {
    SlaveHle slave;
    slave.write_byte(channel_addr(2), 0x88); // master volume on
    slave.write_byte(channel_addr(2), 0x90);
    slave.write_byte(channel_addr(2), 0x80 | 0x12); // plain level: replies at once
    slave.write_byte(channel_addr(2), 0xff);
    CHECK(read_reply(slave) == 0xf512);
    slave.write_byte(channel_addr(3), 0xf5);
    CHECK(read_reply(slave) == 0xf512);
}

TEST_CASE("0x87 stops the ramp and replies the master level") {
    SlaveHle slave;
    slave.write_byte(channel_addr(2), 0x88);
    slave.write_byte(channel_addr(2), 0x86); // one step softer, ramp on
    slave.write_byte(channel_addr(2), 0x87);
    CHECK(read_reply(slave) == 0xf501);
}

TEST_CASE("channel 2 replies raise IN2 only between 0xf8 and 0xf9") {
    SlaveHle slave;
    slave.write_byte(channel_addr(3), 0xf6); // the BIOS polls this before 0xf8
    CHECK(!slave.irq2_asserted());
    slave.write_byte(channel_addr(3), 0xf8); // a reply already waiting raises it now
    CHECK(slave.irq2_asserted());
    CHECK(read_reply(slave) == 0xf601);
    CHECK(!slave.irq2_asserted());

    slave.write_byte(channel_addr(3), 0xf3);
    CHECK(slave.irq2_asserted());
    CHECK(read_reply(slave) == 0xf302);

    slave.write_byte(channel_addr(3), 0xf9);
    slave.write_byte(channel_addr(3), 0xf4);
    CHECK(!slave.irq2_asserted());
    CHECK(read_reply(slave) == 0xf400);
}

}  // TEST_SUITE

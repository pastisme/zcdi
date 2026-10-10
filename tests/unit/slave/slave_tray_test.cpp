// CDi_MiSTer servo_hle.sv.
#include "doctest.h"

#include <cstdint>

#include "slave/slave.h"
#include "support/slave_rig.h"

using zcdi::mamecdi::SlaveHle;
using zcdi_test::channel_addr;

namespace {

// Runs frames until a response is pending (at most `frames`) and reads the
// four disc status bytes back from channel 3; 0 when none arrived.
uint32_t wait_disc_status(SlaveHle& slave, int frames) {
    for (int i = 0; i < frames && !slave.irq2_asserted(); ++i) slave.tick_frame();
    if (!slave.irq2_asserted()) return 0;
    uint32_t status = 0;
    for (int i = 0; i < 4; ++i) status = status << 8 | slave.read_byte(channel_addr(3));
    return status;
}

}  // namespace

TEST_SUITE("slave") {

TEST_CASE("opening and closing the tray reports the new disc status") {
    SlaveHle slave;
    slave.set_disc_present(true, false);
    slave.write_byte(channel_addr(3), 0xfa);
    slave.write_byte(channel_addr(2), 0x89);
    CHECK(wait_disc_status(slave, 50) == 0xb0000321);
    slave.write_byte(channel_addr(2), 0x92);
    CHECK(wait_disc_status(slave, 50) == 0xb0000425);
}

TEST_CASE("an LCD update does not swallow the next channel 2 commands") {
    SlaveHle slave;
    slave.write_byte(channel_addr(3), 0xfa);
    slave.write_byte(channel_addr(2), 0xf0);
    for (int i = 0; i < 16; ++i) slave.write_byte(channel_addr(1), 0x40);
    slave.write_byte(channel_addr(2), 0x8a);
    CHECK(slave.consume_reset_request());

    slave.write_byte(channel_addr(2), 0xf0);
    slave.write_byte(channel_addr(2), 0x89);
    CHECK(wait_disc_status(slave, 50) == 0xb0000321);
}

TEST_CASE("disc status is forwarded only between 0xfa and 0xfb") {
    SlaveHle slave;
    slave.set_disc_present(true, false);
    for (uint8_t v : {0xb0, 0x00, 0x00, 0x00}) slave.write_byte(channel_addr(3), v);
    CHECK(wait_disc_status(slave, 50) == 0);

    slave.write_byte(channel_addr(3), 0xfa);
    for (uint8_t v : {0xb0, 0x00, 0x00, 0x00}) slave.write_byte(channel_addr(3), v);
    CHECK(wait_disc_status(slave, 50) == 0xb0000425);

    slave.write_byte(channel_addr(3), 0xfb);
    slave.write_byte(channel_addr(2), 0x89);
    CHECK(wait_disc_status(slave, 50) == 0);
}

TEST_CASE("a closed tray reports whether a disc is in it") {
    SlaveHle slave;
    slave.write_byte(channel_addr(3), 0xfa);
    auto request = [&slave] {
        for (uint8_t v : {0xb0, 0x00, 0x00, 0x00}) slave.write_byte(channel_addr(3), v);
        return wait_disc_status(slave, 50);
    };
    CHECK(request() == 0xb0000325);
    slave.set_disc_present(true, false);
    CHECK(request() == 0xb0000425);
    slave.set_disc_present(true, true);
    CHECK(request() == 0xb0000125);
}

TEST_CASE("the tray moved from outside reports like 0x89/0x92") {
    SlaveHle slave;
    slave.set_disc_present(true, false);
    slave.write_byte(channel_addr(3), 0xfa);
    slave.set_tray_open(true);
    CHECK(slave.tray_open());
    CHECK(wait_disc_status(slave, 50) == 0xb0000321);
    slave.set_tray_open(true);  // already open: nothing to report
    CHECK(wait_disc_status(slave, 50) == 0);
    slave.set_tray_open(false);
    CHECK(wait_disc_status(slave, 50) == 0xb0000425);
}

}  // TEST_SUITE

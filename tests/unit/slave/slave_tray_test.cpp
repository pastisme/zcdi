// Tray moves and disc status as SERVO firmware 2.1 reports them; final states as CDi_MiSTer servo_hle.sv.
#include "doctest.h"

#include <cstdint>
#include <vector>

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

// Every status reported over `frames` frames, the one already pending first.
std::vector<uint32_t> reports(SlaveHle& slave, int frames) {
    std::vector<uint32_t> out;
    for (int i = 0; i <= frames; ++i) {
        if (slave.irq2_asserted()) {
            uint32_t status = 0;
            for (int b = 0; b < 4; ++b) status = status << 8 | slave.read_byte(channel_addr(3));
            out.push_back(status);
        }
        if (i < frames) slave.tick_frame();
    }
    return out;
}

const std::vector<uint32_t> kOpening = {0xb0000026, 0xb0000027, 0xb0000028, 0xb0000029, 0xb0000021};
const std::vector<uint32_t> kClosingCdi = {0xb0000022, 0xb0000023, 0xb0000024, 0xb0000025, 0xb0000425};

}  // namespace

TEST_SUITE("slave") {

TEST_CASE("opening and closing the tray report every step") {
    SlaveHle slave;
    slave.set_disc_present(true, false);
    slave.write_byte(channel_addr(3), 0xfa);
    slave.write_byte(channel_addr(2), 0x89);
    CHECK(slave.tray_open());
    CHECK(!slave.tray_closed());
    CHECK(reports(slave, 200) == kOpening);
    slave.write_byte(channel_addr(2), 0x92);
    CHECK(!slave.tray_open());
    CHECK(!slave.tray_closed());  // still moving: no disc for the CDIC yet
    CHECK(reports(slave, 200) == kClosingCdi);
    CHECK(slave.tray_closed());
}

TEST_CASE("a move reverses half way as the servo tables say") {
    SlaveHle slave;
    slave.set_disc_present(true, false);
    slave.write_byte(channel_addr(3), 0xfa);
    slave.write_byte(channel_addr(2), 0x89);
    CHECK(reports(slave, 0) == std::vector<uint32_t>{0xb0000026});
    slave.write_byte(channel_addr(2), 0x92);  // not moving yet: closed again, TOC read again
    CHECK(reports(slave, 200) == std::vector<uint32_t>{0xb0000025, 0xb0000425});

    slave.write_byte(channel_addr(2), 0x89);
    CHECK(reports(slave, 15).back() == 0xb0000027);
    slave.write_byte(channel_addr(2), 0x92);  // moving out: back in from step 2
    CHECK(reports(slave, 200) == kClosingCdi);
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
    CHECK(reports(slave, 0) == std::vector<uint32_t>{0xb0000026});
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
    CHECK(reports(slave, 200).empty());
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
    CHECK(reports(slave, 200) == kOpening);
    slave.set_tray_open(true);  // already open: nothing to report
    CHECK(reports(slave, 200).empty());
    slave.set_tray_open(false);
    CHECK(reports(slave, 200) == kClosingCdi);
}

TEST_CASE("0x93 stops the disc and opens the tray (servo A8)") {
    SlaveHle slave;
    slave.set_disc_present(true, false);
    slave.write_byte(channel_addr(3), 0xfa);
    slave.write_byte(channel_addr(2), 0x93);
    CHECK(slave.tray_open());
    CHECK(reports(slave, 200) == kOpening);
}

TEST_CASE("raw servo commands on channel 3 move the tray") {
    SlaveHle slave;
    slave.set_disc_present(true, false);
    slave.write_byte(channel_addr(3), 0xfa);
    for (uint8_t b : {0xa6, 0x00, 0x00, 0x00}) slave.write_byte(channel_addr(3), b);
    CHECK(slave.tray_open());
    CHECK(reports(slave, 200) == kOpening);
    for (uint8_t b : {0xa6, 0x00, 0x00, 0x00}) slave.write_byte(channel_addr(3), b);  // already open
    CHECK(reports(slave, 200).empty());
    for (uint8_t b : {0xa7, 0x00, 0x00, 0x00}) slave.write_byte(channel_addr(3), b);
    CHECK(!slave.tray_open());
    CHECK(reports(slave, 200) == kClosingCdi);
    for (uint8_t b : {0xa8, 0x00, 0x00, 0x00}) slave.write_byte(channel_addr(3), b);
    CHECK(slave.tray_open());
}

TEST_CASE("servo version and echo replies are forwarded only after 0xfa") {
    SlaveHle slave;
    for (uint8_t b : {0xa3, 0x00, 0x00, 0x00}) slave.write_byte(channel_addr(3), b);
    CHECK(!slave.irq2_asserted());
    slave.write_byte(channel_addr(3), 0xfa);
    for (uint8_t b : {0xa3, 0x00, 0x00, 0x00}) slave.write_byte(channel_addr(3), b);
    CHECK(wait_disc_status(slave, 1) == 0xa3200000);
    for (uint8_t b : {0xa4, 0x0c, 0x0d, 0x0e}) slave.write_byte(channel_addr(3), b);
    CHECK(wait_disc_status(slave, 1) == 0xa40c0d0e);
}

}  // TEST_SUITE

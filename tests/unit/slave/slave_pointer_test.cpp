// The front socket device on the SLAVE's SCI, after firmware 2.0 (slave20.asm
// 0x67a SCI interrupt, 0xd90 pointer, 0xe0d report, 0x961 timer).
#include "doctest.h"

#include <cstdint>
#include <vector>

#include "core/pointing_device.h"
#include "slave/slave.h"
#include "support/slave_rig.h"

using zcdi::mamecdi::FrontDeviceType;
using zcdi::mamecdi::Pc6;
using zcdi::mamecdi::SlaveHle;
using zcdi_test::channel_addr;

namespace {

// One 8.19 ms output compare tick: 16 ticks of the 0.5 MHz counter's high byte.
constexpr long kOcTick = 15000000L * 256 / 500000 * 16;

void run(SlaveHle& s, long cycles) {
    for (long done = 0; done < cycles; done += 1000) s.tick_cycles(1000);
}

// Waits out a detection cycle: RTS high, then low after 5 ticks.
void finish_detection(SlaveHle& s) {
    REQUIRE(s.front_rts_negated());
    run(s, 5 * kOcTick + 1000);
    REQUIRE(!s.front_rts_negated());
}

uint8_t front_type(SlaveHle& s) {
    s.write_byte(channel_addr(3), 0xf3);
    REQUIRE(s.read_byte(channel_addr(2)) == 0xf3);
    return s.read_byte(channel_addr(2));
}

void send(SlaveHle& s, std::vector<uint8_t> bytes) {
    for (uint8_t b : bytes) s.sci_receive(uint8_t(b | 0x80), false);
}

// Through the power-on cycle (the first byte is lost) and the 9600 baud one
// (a 1200 baud device gives a framing error), to an identified device.
SlaveHle identified(zcdi::DeviceId id) {
    SlaveHle s;
    finish_detection(s);
    send(s, {uint8_t(id)});
    CHECK(s.sci_at_9600_baud());
    finish_detection(s);
    s.sci_receive(uint8_t(id) | 0x80, true);
    CHECK(!s.sci_at_9600_baud());
    finish_detection(s);
    send(s, {uint8_t(id)});
    return s;
}

std::vector<uint8_t> read_report(SlaveHle& s) {
    std::vector<uint8_t> r;
    for (int i = 0; i < 4; ++i) r.push_back(s.read_byte(channel_addr(0)));
    return r;
}

}  // namespace

TEST_SUITE("slave") {

TEST_CASE("the front device is identified on the third detection cycle") {
    SlaveHle s;
    CHECK(front_type(s) == uint8_t(FrontDeviceType::kPowerOn));
    s.sci_receive('J' | 0x80, false);  // ignored: RTS still holds the device
    CHECK(s.front_rts_negated());
    SlaveHle t = identified(zcdi::DeviceId::kManeuvering);
    CHECK(front_type(t) == uint8_t(FrontDeviceType::kManeuvering));
    CHECK(!t.front_rts_negated());
}

TEST_CASE("every ID maps to its type; anything else loses the device") {
    SlaveHle m = identified(zcdi::DeviceId::kRelative);
    CHECK(front_type(m) == 1);
    SlaveHle t = identified(zcdi::DeviceId::kAbsoluteCoordinate);
    CHECK(front_type(t) == 3);
    SlaveHle a = identified(zcdi::DeviceId::kAbsoluteScreen);
    CHECK(front_type(a) == 4);
    SlaveHle s;
    finish_detection(s);
    send(s, {'J'});
    finish_detection(s);
    s.sci_receive('J' | 0x80, true);
    finish_detection(s);
    send(s, {'X'});
    CHECK(front_type(s) == 0);
    CHECK(s.front_rts_negated());
}

TEST_CASE("relative packets move the pointer 1:1, clamped, and every packet is reported") {
    SlaveHle s = identified(zcdi::DeviceId::kManeuvering);
    s.write_byte(channel_addr(0), 0x83);
    // dx +5, dy -3, button 1.
    send(s, {0x6c, 0x05, 0x3d});
    CHECK(s.irq2_asserted());
    CHECK(read_report(s) == std::vector<uint8_t>{0x98, 0x05, 0x00, 0x00});  // y clamped at 0
    CHECK(!s.irq2_asserted());
    send(s, {0x40, 0x00, 0x00});  // no motion: still a report
    CHECK(read_report(s) == std::vector<uint8_t>{0x88, 0x05, 0x00, 0x00});
    for (int i = 0; i < 10; ++i) send(s, {0x55, 0x3f, 0x3f});  // +127, +127, button 2
    CHECK(read_report(s) == std::vector<uint8_t>{0xad, 0x7f, 0x04, 0x2f});  // 767, 559
}

TEST_CASE("IN2 waits for 0x83; a report already waiting raises it then") {
    SlaveHle s = identified(zcdi::DeviceId::kRelative);
    send(s, {0x40, 0x01, 0x01});
    CHECK(!s.irq2_asserted());
    s.write_byte(channel_addr(0), 0x93);  // decoded on the low nibble: 0x83
    CHECK(s.irq2_asserted());
    s.write_byte(channel_addr(0), 0x84);
    CHECK(!s.irq2_asserted());
}

TEST_CASE("one cut packet is tolerated, a second in a row loses the device") {
    SlaveHle s = identified(zcdi::DeviceId::kManeuvering);
    send(s, {0x40, 0x00, 0x00});
    send(s, {0x40, 0x01});  // cut
    send(s, {0x40, 0x00, 0x00});
    CHECK(front_type(s) == 2);
    send(s, {0x40, 0x40, 0x40});  // a good sync, then two cut packets
    CHECK(front_type(s) == 0);
    CHECK(s.front_rts_negated());
}

TEST_CASE("absolute packets are scaled to 3/4 X and 19/32 Y, not clamped") {
    SlaveHle s = identified(zcdi::DeviceId::kAbsoluteScreen);
    s.write_byte(channel_addr(0), 0x83);
    send(s, {0x6f, 0x2f, 0x3f, 0x3f});  // 1023, 1023, button 1, pen down
    // 766, 605: x 101 1111110, y 100 1011101; pd shows as bit 3, which 0x88 sets anyway
    CHECK(read_report(s) == std::vector<uint8_t>{0x9d, 0x7e, 0x04, 0x5d});
}

TEST_CASE("Set Pointer Position moves the pointer and drops the unread report") {
    SlaveHle s = identified(zcdi::DeviceId::kManeuvering);
    s.write_byte(channel_addr(0), 0x83);
    send(s, {0x40, 0x01, 0x00});
    CHECK(s.irq2_asserted());
    // x = 0x123 (010 0100011), y = 0x1a5 (0110 100101): 11 y5-0, 0 x9-7 y9-6, 0 x6-0.
    s.write_byte(channel_addr(0), 0xc0 | 0x25);
    s.write_byte(channel_addr(0), 0x20 | 0x06);
    s.write_byte(channel_addr(0), 0x23);
    CHECK(!s.irq2_asserted());
    send(s, {0x40, 0x00, 0x00});
    CHECK(read_report(s) == std::vector<uint8_t>{0x8a, 0x23, 0x03, 0x25});
}

TEST_CASE("0xfc drives PC6 high for 2 ticks then low; 0xfd releases it") {
    SlaveHle s;
    CHECK(s.state().pc6_ == Pc6::kReleased);
    CHECK(s.back_rts_negated());
    s.write_byte(channel_addr(3), 0xfc);
    CHECK(s.state().pc6_ == Pc6::kHigh);
    run(s, 2 * kOcTick + 1000);
    CHECK(s.state().pc6_ == Pc6::kLow);
    CHECK(!s.back_rts_negated());
    s.write_byte(channel_addr(3), 0xfd);
    CHECK(s.back_rts_negated());
}

TEST_CASE("IN2 stays up while another enabled channel has a reply") {
    SlaveHle s = identified(zcdi::DeviceId::kManeuvering);
    s.write_byte(channel_addr(0), 0x83);
    s.write_byte(channel_addr(3), 0xf8);  // channel 2 replies raise IN2
    s.write_byte(channel_addr(3), 0xf4);
    send(s, {0x40, 0x00, 0x00});
    CHECK(s.read_byte(channel_addr(2)) == 0xf4);
    CHECK(s.irq2_asserted());
    s.read_byte(channel_addr(2));
    CHECK(s.irq2_asserted());  // the pointer report is still waiting
    read_report(s);
    CHECK(!s.irq2_asserted());
}

}  // TEST_SUITE

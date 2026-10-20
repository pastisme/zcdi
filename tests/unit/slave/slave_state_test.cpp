#include "doctest.h"

#include <cstdint>
#include <vector>

#include "slave/slave.h"
#include "support/slave_rig.h"
#include "support/state_roundtrip.h"

using zcdi_test::channel_addr;

namespace {

constexpr long kOcTick = 15000000L * 256 / 500000 * 16;

void run(zcdi::mamecdi::SlaveHle& s, long cycles) {
    for (long done = 0; done < cycles; done += 1000) s.tick_cycles(1000);
}

}  // namespace

TEST_SUITE("slave") {

TEST_CASE("a SLAVE restored mid-detection and mid-packet behaves the same") {
    zcdi::mamecdi::SlaveHle a;
    a.write_byte(channel_addr(0), 0x83);
    run(a, 2 * kOcTick + 500);  // detecting
    std::vector<uint8_t> state = zcdi_test::save_state(a);
    zcdi::mamecdi::SlaveHle b;
    REQUIRE(zcdi_test::load_state(b, state));
    CHECK(zcdi_test::save_state(b) == state);

    // Both go through the lost power-on byte, the 9600 baud try, then a 'J'.
    auto drive = [](zcdi::mamecdi::SlaveHle& s) {
        run(s, 4 * kOcTick);
        s.sci_receive('J' | 0x80, false);
        run(s, 6 * kOcTick);
        s.sci_receive('J' | 0x80, true);
        run(s, 6 * kOcTick);
        s.sci_receive('J' | 0x80, false);
        s.sci_receive(0x6c | 0x80, false);
        s.sci_receive(0x05 | 0x80, false);  // mid-packet
    };
    drive(a);
    drive(b);
    state = zcdi_test::save_state(a);
    CHECK(zcdi_test::save_state(b) == state);
    zcdi::mamecdi::SlaveHle c;
    REQUIRE(zcdi_test::load_state(c, state));
    a.sci_receive(0x3d | 0x80, false);
    c.sci_receive(0x3d | 0x80, false);
    CHECK(zcdi_test::save_state(c) == zcdi_test::save_state(a));
    CHECK(c.irq2_asserted());
}

TEST_CASE("a SLAVE state with an out-of-range command length is rejected") {
    zcdi::mamecdi::SlaveHle a;
    std::vector<uint8_t> state = zcdi_test::save_state(a);
    // Section header (8) then channel 0: in[4], then in_len (4 bytes LE).
    state[8 + 4] = 5;
    zcdi::mamecdi::SlaveHle b;
    CHECK(!zcdi_test::load_state(b, state));
}

}  // TEST_SUITE

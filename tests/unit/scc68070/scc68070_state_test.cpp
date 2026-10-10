#include "doctest.h"

#include <cstdint>
#include <vector>

#include "scc68070/scc68070.h"
#include "support/state_roundtrip.h"

using zcdi::mamecdi::Scc68070Periph;

namespace {

constexpr uint32_t kTimerTsrTcr = 0x80002020;
constexpr uint32_t kTimerReload = 0x80002022;
constexpr uint32_t kTimer0 = 0x80002024;
constexpr uint32_t kTimer1 = 0x80002026;
constexpr uint32_t kTimer2 = 0x80002028;
constexpr uint32_t kPicr1 = 0x80002045;

uint16_t timer0(Scc68070Periph& s) { return uint16_t(s.read_byte(kTimer0) << 8 | s.read_byte(kTimer0 + 1)); }

}  // namespace

TEST_SUITE("scc68070") {

TEST_CASE("a restored SCC68070 timer counts and interrupts like the original") {
    Scc68070Periph a;
    a.write_byte(kTimerReload, 0xf0);
    a.write_byte(kTimerReload + 1, 0x00);
    a.write_byte(kPicr1, 0x04);  // timer at level 4
    a.tick(12345);         // leaves a fractional cycle carry

    const std::vector<uint8_t> state = zcdi_test::save_state(a);
    Scc68070Periph b;
    REQUIRE(zcdi_test::load_state(b, state));
    CHECK(zcdi_test::save_state(b) == state);

    for (int i = 0; i < 400; ++i) {
        CAPTURE(i);
        a.tick(997);
        b.tick(997);
        CHECK(timer0(b) == timer0(a));
        CHECK(b.current_ipl() == a.current_ipl());
        CHECK(b.read_byte(kTimerTsrTcr) == a.read_byte(kTimerTsrTcr));
    }
}

// scc68070.sv: the timer requests its interrupt while TSR OV0 is set; only a
// TSR write clears it, not the interrupt acknowledge.
TEST_CASE("the timer interrupt holds until TSR is cleared") {
    Scc68070Periph s;
    s.write_byte(kTimerReload, 0xff);
    s.write_byte(kTimerReload + 1, 0xf0);
    s.write_byte(kTimer0, 0xff);
    s.write_byte(kTimer0 + 1, 0xf0);
    s.write_byte(kPicr1, 0x04);  // timer at level 4
    for (int i = 0; i < 100 && s.current_ipl() == 0; ++i) s.tick(96);
    REQUIRE(s.current_ipl() == 4);
    s.iack(4);
    CHECK(s.current_ipl() == 4);
    s.write_byte(kTimerTsrTcr, 0x80);  // write 1 to OV0
    CHECK(s.current_ipl() == 0);
}

// Datasheet: in match mode, T0 reaching T1 (T2) sets TSR MA1 (MA2) and
// requests the timer interrupt; inhibited timers never match.
TEST_CASE("timers 1 and 2 flag a match with timer 0 in match mode") {
    Scc68070Periph s;
    s.write_byte(kTimer1, 0x00);
    s.write_byte(kTimer1 + 1, 0x10);
    s.write_byte(kTimer2, 0x00);
    s.write_byte(kTimer2 + 1, 0x20);
    s.write_byte(kPicr1, 0x04);  // timer at level 4
    s.tick(96 * 0x30);
    CHECK(s.read_byte(kTimerTsrTcr) == 0);
    CHECK(s.current_ipl() == 0);

    s.write_byte(kTimer0, 0x00);
    s.write_byte(kTimer0 + 1, 0x00);
    s.write_byte(kTimerTsrTcr + 1, 0x11);  // T1 and T2 in match mode
    s.tick(96 * 0x0f);
    CHECK(s.read_byte(kTimerTsrTcr) == 0);
    s.tick(96);
    CHECK(s.read_byte(kTimerTsrTcr) == 0x40);  // MA1
    CHECK(s.current_ipl() == 4);
    s.tick(96 * 0x10);
    CHECK(s.read_byte(kTimerTsrTcr) == 0x48);  // MA1 | MA2
    s.write_byte(kTimerTsrTcr, 0x48);
    CHECK(s.current_ipl() == 0);
}

}  // TEST_SUITE

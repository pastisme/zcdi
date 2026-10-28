#include "doctest.h"

#include <chrono>
#include <cstdint>

#include "mk48t08/mk48t08.h"

using zcdi::mamecdi::Mk48t08;
using namespace std::chrono;

TEST_SUITE("mk48t08") {

TEST_CASE("a pinned clock counts emulated seconds across a leap day") {
    Mk48t08 rtc;
    rtc.set_boot_time(sys_days{2000y / February / 28} + 23h + 59min + 59s);
    CHECK(rtc.read(0x1ffd) == 0x28);  // date
    CHECK(rtc.read(0x1ffc) == 0x02);  // Monday, Sun=1

    rtc.tick_cycles(15'000'000);  // one second
    CHECK(rtc.read(0x1ff9) == 0x00);  // second
    CHECK(rtc.read(0x1ffa) == 0x00);  // minute
    CHECK(rtc.read(0x1ffb) == 0x00);  // hour
    CHECK(rtc.read(0x1ffc) == 0x03);  // Tuesday
    CHECK(rtc.read(0x1ffd) == 0x29);
    CHECK(rtc.read(0x1ffe) == 0x02);
    CHECK(rtc.read(0x1fff) == 0x00);
    CHECK(rtc.read(0x1ff1) == 0x20);  // century
}

TEST_CASE("a clock pinned before 1970 reads its date") {
    Mk48t08 rtc;
    rtc.set_boot_time(sys_days{1969y / December / 31} + 23h + 59min + 59s);
    CHECK(rtc.read(0x1ffb) == 0x23);
    CHECK(rtc.read(0x1ffc) == 0x04);  // Wednesday
    CHECK(rtc.read(0x1ffd) == 0x31);
    CHECK(rtc.read(0x1fff) == 0x69);
    CHECK(rtc.read(0x1ff1) == 0x19);
}

}  // TEST_SUITE

#include "doctest.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/machine.h"
#include "core/machine_inspector.h"
#include "support/fake_machine.h"
#include "support/state_roundtrip.h"

using zcdi_test::build_fake_machine;
using zcdi_test::save_machine;

namespace {

constexpr uint32_t kCounter = 0x00001000;  // where the fake BIOS stores d0
constexpr uint32_t kNvramBase = 0x320000;  // MK48T08, even bytes

}  // namespace

TEST_SUITE("core") {

TEST_CASE("peek reads memory without changing the machine") {
    std::unique_ptr<zcdi::Machine> m = build_fake_machine();
    m->run_frame();
    zcdi::MachineInspector view(*m);
    CHECK(view.peek32(kCounter) == m->bus().read32(kCounter));
    CHECK(view.peek8(0x4fffe0) == 0xff);  // MCD212 register

    const std::vector<uint8_t> before = save_machine(*m);
    for (uint32_t a = 0; a < 0x1000000; a += 0x100) view.peek8(a);
    for (uint32_t a = 0x80000000; a < 0x80010000; ++a) view.peek8(a);
    CHECK(zcdi_test::first_difference(before, save_machine(*m)) == -1);
}

TEST_CASE("the RTC can be pinned and survives a cold boot") {
    std::vector<std::byte> bios = zcdi_test::fake_bios();
    std::unique_ptr<zcdi::Machine> m = std::make_unique<zcdi::Machine>(bios, zcdi::VideoStandard::kPal);
    using namespace std::chrono;
    m->set_rtc_boot_time(sys_days{1995y / July / 14} + 12h);
    m->cold_boot();
    CHECK(m->bus().read8(kNvramBase + 0x1fff * 2) == 0x95);  // year, BCD
    CHECK(m->bus().read8(kNvramBase + 0x1ffe * 2) == 0x07);  // month
    CHECK(m->bus().read8(kNvramBase + 0x1ffd * 2) == 0x14);  // date
    CHECK(m->bus().read8(kNvramBase + 0x1ff1 * 2) == 0x19);  // century, a stored byte
}

// Mid-frame lines are for a debugger stopped inside run_frame().
TEST_CASE("line() is the lines per field between frames") {
    std::unique_ptr<zcdi::Machine> m = build_fake_machine();
    const zcdi::MachineInspector view(*m);
    CHECK(view.line() == zcdi::field_lines(zcdi::VideoStandard::kPal));
    m->run_frame();
    CHECK(view.line() == zcdi::field_lines(zcdi::VideoStandard::kPal));
}

}  // TEST_SUITE

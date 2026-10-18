#include "doctest.h"

#include <cstdint>

#include "support/cpu_fixture.h"

using zcdi_test::CpuFixture;

namespace {
// SCC68070 datasheet timings, zero wait states. The first run() after reset()
// also charges the reset-vector fetch.
int cycles_of_first_instruction(CpuFixture& f, uint16_t opcode) {
    f.code({opcode});
    f.cpu.reset();
    f.cpu.run(1);
    return f.cpu.run(1);
}
}  // namespace

TEST_SUITE("cpu") {

TEST_CASE("MOVEQ takes 7 cycles") {
    CpuFixture f;
    CHECK(cycles_of_first_instruction(f, 0x7005) == 7);  // MOVEQ #5,D0
    CHECK(f.cpu.d(0) == 5);
}

TEST_CASE("taken BRA.S takes 13 cycles") {
    CpuFixture f;
    CHECK(cycles_of_first_instruction(f, 0x60fe) == 13);
}

TEST_CASE("BSR.S takes 17 cycles") {
    CpuFixture f;
    CHECK(cycles_of_first_instruction(f, 0x6102) == 17);
}

}  // TEST_SUITE

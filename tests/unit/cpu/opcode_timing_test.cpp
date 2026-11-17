#include "doctest.h"

#include <cstdint>

#include "core/board.h"
#include "support/cpu_fixture.h"

using zcdi_test::CpuFixture;

namespace {
// Costs include Board::sdram_wait_cycles() per bus access, queried live. The
// first run() after reset() also charges the reset-vector fetch.
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
    CHECK(cycles_of_first_instruction(f, 0x7005) == 7 + f.board.sdram_wait_cycles(CpuFixture::kCodePc));  // MOVEQ #5,D0
    CHECK(f.cpu.d(0) == 5);
}

TEST_CASE("taken BRA.S takes 13 cycles") {
    CpuFixture f;
    CHECK(cycles_of_first_instruction(f, 0x60fe) == 13 + f.board.sdram_wait_cycles(CpuFixture::kCodePc));
}

TEST_CASE("BSR.S takes 17 cycles") {
    CpuFixture f;
    // The return address is pushed at initial SSP - 4.
    CHECK(cycles_of_first_instruction(f, 0x6102) ==
          17 + f.board.sdram_wait_cycles(CpuFixture::kCodePc) +
              2 * f.board.sdram_wait_cycles(CpuFixture::kInitialSsp - 4));
}

// 2 is what The 7th Guest needs: with 0 the chessboard races the disc and the
// title reboots to the BIOS, with 1 the crypt after the maze hangs. The real
// player's count is unmeasured.
TEST_CASE("an SDRAM access costs 2 wait cycles, a chip access none") {
    using namespace zcdi::address_map;
    CpuFixture f;
    for (uint32_t a : {kPlane0.base, kPlane1.base, kBios.base, kDvcCartRam.base, kDvcCartRom.base, kDvcDecodeRam.base}) {
        CAPTURE(a);
        CHECK(f.board.sdram_wait_cycles(a) == 2);
    }
    for (uint32_t a : {kExtraRam.base, kNvram.base, kCdicRam.base, kCdicRegs.base, kSlave.base, kMcd212.base}) {
        CAPTURE(a);
        CHECK(f.board.sdram_wait_cycles(a) == 0);
    }
    // The CPU's direct path to RAM charges the same: MOVEQ is one fetch.
    CHECK(cycles_of_first_instruction(f, 0x7005) == 7 + 2);
}

}  // TEST_SUITE

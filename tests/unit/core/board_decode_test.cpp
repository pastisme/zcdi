#include "doctest.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/board.h"
#include "support/cpu_fixture.h"
#include "support/dvc_cart.h"

using zcdi::Board;
using zcdi_test::CpuFixture;
using zcdi_test::hi16;
using zcdi_test::insert_dvc_cart;
using zcdi_test::lo16;

TEST_SUITE("core") {

// Overlay off, with a DVC cartridge. Not checked: 0x500000-0x57ffff and some
// holes, which zcdi keeps decoded.
TEST_CASE("bus-error ranges at their edges") {
    struct Probe {
        uint32_t addr;
        bool decoded;
    };
    const Probe probes[] = {
        {0x07ffff, true},  {0x080000, false}, {0x1fffff, false}, {0x200000, true},   // area1
        {0x4fffff, true},  {0x580000, false}, {0xcfffff, false}, {0xd00000, true},   // area2
        {0xe00000, true},  {0xe7ffff, true},                                         // DVC regs, ROM
        {0xf00000, false}, {0xf0ffff, false},                                        // area4
        {0xf10000, false}, {0xffffff, false},                                        // area3
    };
    Board board;
    insert_dvc_cart(board);
    for (const Probe& p : probes) {
        CAPTURE(p.addr);
        CHECK(board.is_decoded(p.addr) == p.decoded);
    }
}

// load_bios() takes any size; a short dump reads 0 past its end, as peek8 does.
TEST_CASE("a BIOS shorter than 512 KB reads 0 past its end") {
    Board board;
    const std::vector<std::byte> bios(16, std::byte{0xaa});
    board.load_bios(bios);
    CHECK(board.read8(0x40000f) == 0xaa);
    CHECK(board.read8(0x400010) == 0);
    CHECK(board.read8(0x47ffff) == 0);
}

TEST_CASE("the DVC cartridge RAM exists only with a cartridge") {
    Board board;
    CHECK(!board.is_decoded(0xd00000));
    CHECK(!board.is_decoded(0xdfffff));
    board.write8(0xd10000, 0x5a);
    CHECK(board.read8(0xd10000) == 0);

    insert_dvc_cart(board);
    CHECK(board.is_decoded(0xd00000));
    CHECK(board.is_decoded(0xdfffff));
    CHECK(!board.is_decoded(0xcfffff));
    board.write8(0xd10000, 0x5a);
    CHECK(board.read8(0xd10000) == 0x5a);
}

TEST_CASE("the DVC decode-RAM gap opens after 64 VMPEG register writes") {
    constexpr uint32_t kDvcRegBase = zcdi::address_map::kDvcRegs.base;
    constexpr uint32_t kDvcDecodeRamBase = zcdi::address_map::kDvcDecodeRam.base;
    constexpr uint32_t kExtraBase = zcdi::address_map::kExtraRam.base;

    CpuFixture f;
    Board& board = f.board;

    // Before the gate it faults, which the boot RAM probe relies on.
    CHECK(!board.is_decoded(kDvcDecodeRamBase));
    CHECK(!board.is_decoded(kDvcDecodeRamBase + 0x1000));
    // kExtraBase is not tied to the gate.
    CHECK(board.is_decoded(kExtraBase));

    // Completing writes to the VMPEG window (Dvc::note_bus_write_access).
    for (int i = 0; i < 64; ++i) board.write8(kDvcRegBase + 1, 0);

    // After it, vmpega.rom's accesses must not fault.
    CHECK(board.is_decoded(kDvcDecodeRamBase));
    CHECK(board.is_decoded(kDvcDecodeRamBase + 0x1000));
    CHECK(board.is_decoded(kExtraBase));
}

// kDvcCartRomBase (0xe40000) ends where the gated kDvcDecodeRamBase begins.
TEST_CASE("a 32-bit access straddling into an undecoded range faults on its second half") {
    SUBCASE("straddling access faults at a+2") {
        constexpr uint32_t kStraddleAddr = 0x00e7fffe;
        CpuFixture f;
        f.park_vector(CpuFixture::kBusErrorVector);
        f.code({0x2039, hi16(kStraddleAddr), lo16(kStraddleAddr)});  // MOVE.L (xxx).L,D0
        f.reset_and_run();

        REQUIRE(f.cpu.pc() == CpuFixture::kHandlerPc);
        const uint16_t vector_word = f.frame_vector_word();
        CHECK((vector_word & 0xf000) == 0xf000);    // Format 15 marker
        CHECK((vector_word & 0x0fff) == (2u << 2));  // bus error vector
        CHECK(f.frame_fault_address() == kStraddleAddr + 2);
    }
    SUBCASE("fully decoded access next to it does not fault") {
        constexpr uint32_t kSafeAddr = 0x00e40000;
        CpuFixture f;
        f.park_vector(CpuFixture::kBusErrorVector);
        f.code({0x2039, hi16(kSafeAddr), lo16(kSafeAddr),                              // MOVE.L (xxx).L,D0
                0x4ef9, hi16(CpuFixture::kNormalPc), lo16(CpuFixture::kNormalPc)});    // JMP (xxx).L
        f.reset_and_run();
        CHECK(f.cpu.pc() == CpuFixture::kNormalPc);
    }
}

// Only A0-A23 leave the chip; A31:30 = 10 in supervisor
// mode selects the on-chip peripherals.
TEST_CASE("the external bus ignores the top address byte") {
    SUBCASE("a tagged pointer reaches RAM") {
        CpuFixture f;
        f.park_vector(CpuFixture::kBusErrorVector);
        f.code({0x13fc, 0x005a, 0x4024, 0x9726,                                      // MOVE.B #$5a,($40249726).L
                0x4ef9, hi16(CpuFixture::kNormalPc), lo16(CpuFixture::kNormalPc)});  // JMP (xxx).L
        f.reset_and_run();
        CHECK(f.cpu.pc() == CpuFixture::kNormalPc);
        CHECK(f.board.read8(0x00249726) == 0x5a);
    }
    SUBCASE("an alias of an undecoded 24-bit address still faults") {
        // 0x7e892f28 -> 0x892f28, inside area2.
        CpuFixture f;
        f.park_vector(CpuFixture::kBusErrorVector);
        f.code({0x1039, 0x7e89, 0x2f28});  // MOVE.B ($7e892f28).L,D0
        f.reset_and_run();
        CHECK(f.cpu.pc() == CpuFixture::kHandlerPc);
    }
    SUBCASE("in user mode, the peripheral window is external memory") {
        CpuFixture f;
        f.park_vector(CpuFixture::kBusErrorVector);
        f.code({0x46fc, 0x0000,                                                      // MOVE #$0000,SR (to user mode)
                0x13fc, 0x005a, 0x8000, 0x1234,                                      // MOVE.B #$5a,($80001234).L
                0x4ef9, hi16(CpuFixture::kNormalPc), lo16(CpuFixture::kNormalPc)});  // JMP (xxx).L
        f.reset_and_run();
        CHECK(f.cpu.pc() == CpuFixture::kNormalPc);
        CHECK(f.board.read8(0x00001234) == 0x5a);
    }
    SUBCASE("in supervisor mode, the peripheral window is not RAM") {
        CpuFixture f;
        f.park_vector(CpuFixture::kBusErrorVector);
        f.code({0x13fc, 0x005a, 0x8000, 0x1234,                                      // MOVE.B #$5a,($80001234).L
                0x4ef9, hi16(CpuFixture::kNormalPc), lo16(CpuFixture::kNormalPc)});  // JMP (xxx).L
        f.reset_and_run();
        CHECK(f.cpu.pc() == CpuFixture::kNormalPc);
        CHECK(f.board.read8(0x00001234) == 0x00);
    }
}

}  // TEST_SUITE

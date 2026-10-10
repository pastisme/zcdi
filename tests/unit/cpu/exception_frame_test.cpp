#include "doctest.h"

#include <cstdint>

#include "support/cpu_fixture.h"

using zcdi_test::CpuFixture;
using zcdi_test::hi16;
using zcdi_test::lo16;

TEST_SUITE("cpu") {

TEST_CASE("an address error pushes a Format 15 frame with the fault address") {
    CpuFixture f;
    f.park_vector(CpuFixture::kAddressErrorVector);

    // An odd PC raises an address error on every 68k variant.
    constexpr uint32_t kOddTarget = 0x00003001;
    f.code({0x4ef9, hi16(kOddTarget), lo16(kOddTarget)});  // JMP (xxx).L
    f.reset_and_run();

    REQUIRE(f.cpu.pc() == CpuFixture::kHandlerPc);  // parked in the handler's self-loop

    // Below SP: +0 SR, +2 PC, +6 vector offset, +8 SSW, +16 data out,
    // +20 fault address, +24 data in, +30 instruction in (m68ki_stack_frame_1111).
    const uint32_t sp = f.cpu.sp();
    const uint16_t vector_word = f.frame_vector_word();
    CHECK((vector_word & 0xf000) == 0xf000);  // Format 15 marker
    // As MAME m68ki_exception_address_error: the bus error vector's offset.
    CHECK((vector_word & 0x0fff) == (2u << 2));
    CHECK(f.bus.read16(sp + 8) == 0);   // special status word
    CHECK(f.bus.read32(sp + 16) == 0);  // data output buffer
    CHECK(f.bus.read32(sp + 24) == 0);  // data input buffer
    CHECK(f.bus.read16(sp + 30) == 0);  // instruction input buffer

    // The odd address that was jumped to.
    CHECK(f.frame_fault_address() == kOddTarget);
}

}  // TEST_SUITE

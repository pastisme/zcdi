// CPU accesses that miss plain memory: chip registers, and undecoded addresses.
#include "doctest.h"

#include <cstdint>

#include "chips/scc68070/scc68070.h"
#include "support/cpu_fixture.h"

using zcdi_test::CpuFixture;
using zcdi_test::hi16;
using zcdi_test::lo16;
using namespace zcdi::chips::scc68070;

namespace {

constexpr uint32_t kMac = kDmaBase + kDmaMac, kMtc = kDmaBase + kDmaMtc, kOcr = kDmaBase + kDmaOcr + 1;
constexpr uint32_t kUndecoded = 0x00600000;  // between extra RAM and the DVC cartridge
constexpr uint16_t kJmpAbs = 0x4ef9;

}  // namespace

TEST_SUITE("cpu") {

TEST_CASE("byte, word and long CPU accesses reach a chip's registers") {
    CpuFixture f;
    f.code({0x23fc, 0x1122, 0x3344, hi16(kMac), lo16(kMac),  // MOVE.L #$11223344,(MAC).L
            0x2039, hi16(kMac), lo16(kMac),                  // MOVE.L (MAC).L,D0
            0x33fc, 0x5566, hi16(kMtc), lo16(kMtc),          // MOVE.W #$5566,(MTC).L
            0x3239, hi16(kMtc), lo16(kMtc),                  // MOVE.W (MTC).L,D1
            0x13fc, 0x0077, hi16(kOcr), lo16(kOcr),          // MOVE.B #$77,(OCR).L
            0x1439, hi16(kOcr), lo16(kOcr),                  // MOVE.B (OCR).L,D2
            kJmpAbs, hi16(CpuFixture::kNormalPc), lo16(CpuFixture::kNormalPc)});
    f.reset_and_run(400);
    REQUIRE(f.cpu.pc() == CpuFixture::kNormalPc);
    CHECK(f.cpu.d(0) == 0x11223344u);
    CHECK((f.cpu.d(1) & 0xffff) == 0x5566);
    CHECK((f.cpu.d(2) & 0xff) == 0x77);
}

TEST_CASE("every access width bus-errors at an undecoded address") {
    struct Case {
        const char* what;
        uint16_t opcode;
    };
    const Case cases[] = {
        {"MOVE.B D0,(xxx).L", 0x13c0}, {"MOVE.W D0,(xxx).L", 0x33c0}, {"MOVE.L D0,(xxx).L", 0x23c0},
        {"MOVE.B (xxx).L,D0", 0x1039}, {"MOVE.W (xxx).L,D0", 0x3039},
    };
    for (const Case& c : cases) {
        CAPTURE(c.what);
        CpuFixture f;
        f.park_vector(CpuFixture::kBusErrorVector);
        f.code({c.opcode, hi16(kUndecoded), lo16(kUndecoded)});
        f.reset_and_run();
        REQUIRE(f.cpu.pc() == CpuFixture::kHandlerPc);
        CHECK(f.frame_fault_address() == kUndecoded);
    }
}

// Two 16-bit bus cycles, decoded one at a time, as reads do.
TEST_CASE("a long write straddling into an undecoded range faults on its second half") {
    constexpr uint32_t kStraddle = 0x0057fffe;  // the last word of extra RAM
    CpuFixture f;
    f.park_vector(CpuFixture::kBusErrorVector);
    f.code({0x203c, 0x1234, 0x5678,  // MOVE.L #$12345678,D0
            0x23c0, hi16(kStraddle), lo16(kStraddle)});
    f.reset_and_run();
    REQUIRE(f.cpu.pc() == CpuFixture::kHandlerPc);
    CHECK(f.frame_fault_address() == kStraddle + 2);
}

TEST_CASE("a long write to chip registers is two word writes") {
    constexpr uint32_t kTimers = 0x80002024;  // timer 0, then timer 1
    CpuFixture f;
    f.code({0x23fc, 0xabcd, 0x1234, hi16(kTimers), lo16(kTimers),  // MOVE.L #$abcd1234,(T0).L
            kJmpAbs, hi16(CpuFixture::kNormalPc), lo16(CpuFixture::kNormalPc)});
    f.reset_and_run();
    REQUIRE(f.cpu.pc() == CpuFixture::kNormalPc);
    CHECK(f.board.read16(kTimer0) == 0xabcd);
    CHECK(f.board.read16(kTimer1) == 0x1234);
}

}  // TEST_SUITE

// ICA and DCA instructions (Green Book V.6), each on its own.
#include "doctest.h"

#include <cstdint>
#include <initializer_list>
#include <vector>

#include "chips/mcd212/mcd212.h"
#include "support/mcd212_field.h"
#include "support/mcd212_regs.h"

using zcdi::chips::Mcd212;
using zcdi::chips::Mcd212SaveState;
using zcdi_test::load_register;
using zcdi_test::put_list;
using zcdi_test::run_field;
using zcdi_test::write_mmio16;
using namespace zcdi::chips::mcd212;

namespace {

constexpr int kPalLines = 312, kFirstActive = 26;
constexpr uint32_t kStop = 0x00000000u, kNop = 0x10000000u, kInterrupt = 0x60000000u, kUndefined = 0x70000000u;
constexpr uint32_t kDcaList = 0x1000;

struct Planes {
    std::vector<uint8_t> a = std::vector<uint8_t>(0x80000), b = std::vector<uint8_t>(0x80000);
};

int field(Mcd212& mcd, Planes& p) { return run_field(mcd, p.a.data(), p.b.data(), p.a.size(), kPalLines); }

// IT1/IT2, cleared by the read.
uint8_t interrupts(Mcd212& mcd) { return mcd.read_byte(kCsr2R) & (kCsr2rIt1 | kCsr2rIt2); }

uint32_t vsr(const Mcd212& mcd, int path) {
    return uint32_t(mcd.state().dcr_[path] & 0x3f) << 16 | mcd.state().vsr_[path];
}
uint32_t dcp(const Mcd212& mcd, int path) {
    return uint32_t(mcd.state().ddr_[path] & 0x3f) << 16 | mcd.state().dcp_[path];
}

// Plane A's DCA runs `kDcaList` from the first active line of the second
// field on; `next_line()` ends one more line.
struct DcaRig {
    Planes p;
    Mcd212 mcd;
    int line = 0;

    explicit DcaRig(std::initializer_list<uint32_t> dca) {
        put_list(p.a.data(), 0x400, {0x30000000u | kDcaList});  // ICA: RELOAD DCP + STOP
        put_list(p.a.data(), kDcaList, dca);
        write_mmio16(mcd, kDcr1, kDcrIc);  // ICA only: sets DCP
        field(mcd, p);
        REQUIRE(dcp(mcd, 0) == kDcaList);
        write_mmio16(mcd, kDcr1, kDcrDe | kDcrIc | kDcrDc);
        mcd.begin_field(kPalLines);
        while (line <= kFirstActive) next_line();
    }
    void next_line() { mcd.end_of_line(line++, p.a.data(), p.b.data(), p.a.size()); }
};

}  // namespace

TEST_SUITE("mcd212") {

TEST_CASE("the ICA passes over NOP and undefined instructions, and RELOAD DCP + STOP ends it") {
    Planes p;
    put_list(p.a.data(), 0x400, {
        kNop,
        0x20001230u,  // RELOAD DCP
        kUndefined,
        kInterrupt,
        0x30002000u,  // RELOAD DCP + STOP
        0xd800000cu,  // not reached: backdrop colour
    });
    Mcd212 mcd;
    write_mmio16(mcd, kDcr1, kDcrIc);
    field(mcd, p);
    CHECK(interrupts(mcd) == kCsr2rIt1);
    CHECK(dcp(mcd, 0) == 0x2000);
    CHECK(mcd.state().backdrop_color_ == 0);
}

TEST_CASE("RELOAD DCP drops the low two address bits") {
    Planes p;
    put_list(p.a.data(), 0x400, {0x30012346u});  // RELOAD DCP + STOP
    Mcd212 mcd;
    write_mmio16(mcd, kDcr1, kDcrIc);
    field(mcd, p);
    CHECK(dcp(mcd, 0) == 0x012344);
}

TEST_CASE("an ICA STOP ends the list") {
    Planes p;
    put_list(p.a.data(), 0x400, {kStop, kInterrupt});
    Mcd212 mcd;
    write_mmio16(mcd, kDcr1, kDcrIc);
    field(mcd, p);
    CHECK(interrupts(mcd) == 0);
}

TEST_CASE("in the ICA, RELOAD VSR only jumps; RELOAD VSR + STOP loads VSR") {
    Planes p;
    put_list(p.a.data(), 0x400, {0x40000800u});  // RELOAD VSR: continue at 0x800
    put_list(p.a.data(), 0x800, {kInterrupt, 0x50012340u, kInterrupt});
    Mcd212 mcd;
    write_mmio16(mcd, kDcr1, kDcrIc);
    field(mcd, p);
    CHECK(interrupts(mcd) == kCsr2rIt1);  // the jump target ran
    CHECK(vsr(mcd, 0) == 0x012340);

    put_list(p.a.data(), 0x800, {kStop});
    Mcd212 jump_only;
    write_mmio16(jump_only, kDcr1, kDcrIc);
    field(jump_only, p);
    CHECK(vsr(jump_only, 0) == 0);
}

TEST_CASE("RELOAD DISPLAY PARAMETERS sets DDR's file type and DCR's CM") {
    Planes p;
    put_list(p.a.data(), 0x400, {0x7800001bu});
    Mcd212 mcd;
    write_mmio16(mcd, kDcr1, kDcrIc);
    write_mmio16(mcd, kDdr1, 0xf005);
    field(mcd, p);
    CHECK(mcd.state().ddr_[0] == 0xfb05);  // bits 8-11 replaced, DCP's high bits kept
    CHECK((mcd.state().dcr_[0] & kDcrCm) != 0);
}

TEST_CASE("the ICA loads registers: opcode 0x80 and above is the register number") {
    Planes p;
    put_list(p.a.data(), 0x400, {0xd800000cu, 0xc2000005u});  // backdrop, plane order
    Mcd212 mcd;
    write_mmio16(mcd, kDcr1, kDcrIc);
    field(mcd, p);
    CHECK(mcd.state().backdrop_color_ == 0xc);
    CHECK(mcd.state().plane_order_ == 5);
}

TEST_CASE("plane B's ICA runs from plane B's memory and raises IT2") {
    Planes p;
    put_list(p.b.data(), 0x400, {kInterrupt, 0x50000100u});
    Mcd212 mcd;
    write_mmio16(mcd, kDcr2, kDcrIc);
    field(mcd, p);
    CHECK(interrupts(mcd) == kCsr2rIt2);
    CHECK(vsr(mcd, 1) == 0x100);
    CHECK(vsr(mcd, 0) == 0);
}

TEST_CASE("with DI an INTERRUPT still latches IT1 but leaves IRQ1 low") {
    Planes p;
    put_list(p.a.data(), 0x400, {kInterrupt});
    Mcd212 masked;
    write_mmio16(masked, kCsr1W, kCsrWDi);
    write_mmio16(masked, kDcr1, kDcrIc);
    field(masked, p);
    CHECK_FALSE(masked.irq1_asserted());
    CHECK(interrupts(masked) == kCsr2rIt1);

    Mcd212 unmasked;
    write_mmio16(unmasked, kDcr1, kDcrIc);
    field(unmasked, p);
    CHECK(unmasked.irq1_asserted());
    interrupts(unmasked);
    CHECK_FALSE(unmasked.irq1_asserted());  // the CSR2R read acknowledges it
}

TEST_CASE("in the DCA, RELOAD DCP is ignored and RELOAD VSR loads VSR and goes on") {
    DcaRig rig({
        0x20005000u,  // RELOAD DCP: ignored mid-DCA
        kNop,
        0x40000100u,  // RELOAD VSR
        kUndefined,
        0x7800000fu,  // RELOAD DISPLAY PARAMETERS
        0xd800000cu,  // backdrop colour
        kInterrupt,
        kStop,
    });
    CHECK(dcp(rig.mcd, 0) == kDcaList);
    CHECK(vsr(rig.mcd, 0) == 0x100);
    CHECK(((rig.mcd.state().ddr_[0] >> 8) & 0xf) == 0xf);
    CHECK(rig.mcd.state().backdrop_color_ == 0xc);
    CHECK(interrupts(rig.mcd) == kCsr2rIt1);
}

TEST_CASE("a DCA line takes 64 bytes wherever its STOP or RELOAD VSR + STOP falls") {
    DcaRig rig({0x50000200u, kInterrupt});  // RELOAD VSR + STOP
    CHECK(vsr(rig.mcd, 0) == 0x200);
    CHECK(interrupts(rig.mcd) == 0);
    CHECK(rig.mcd.state().dca_[0] == kDcaList + 64);
    rig.next_line();  // all STOP
    CHECK(rig.mcd.state().dca_[0] == kDcaList + 128);
    CHECK(uint16_t(rig.mcd.read_byte(kDca1) << 8 | rig.mcd.read_byte(kDca1 + 1)) == kDcaList + 128);
}

TEST_CASE("a DCA RELOAD DCP + STOP jumps the next line's DCA") {
    DcaRig rig({0x30002000u, kInterrupt});
    put_list(rig.p.a.data(), 0x2000, {kInterrupt});
    CHECK(rig.mcd.state().dca_[0] == 0x2000);
    CHECK(dcp(rig.mcd, 0) == 0x2000);
    CHECK(interrupts(rig.mcd) == 0);
    rig.next_line();
    CHECK(interrupts(rig.mcd) == kCsr2rIt1);
}

TEST_CASE("a DCA line runs at most 16 instructions") {
    DcaRig rig({kNop, kNop, kNop, kNop, kNop, kNop, kNop, kNop, kNop, kNop, kNop, kNop, kNop, kNop, kNop, kNop,
                kInterrupt});
    CHECK(interrupts(rig.mcd) == 0);
    rig.next_line();
    CHECK(interrupts(rig.mcd) == kCsr2rIt1);  // the 17th, on the next line
}

// Green Book V.5: the A registers belong to path 0, the B registers to path 1.
TEST_CASE("each path loads only its own plane's registers") {
    struct Case {
        uint8_t reg;
        int path;
        uint32_t value, expected;
        uint32_t (*read)(const Mcd212SaveState&);
    };
    const Case cases[] = {
        {kRegPlaneOrder, 0, 0xff, 0x07, [](const Mcd212SaveState& s) { return uint32_t(s.plane_order_); }},
        {kRegTransparentColorB, 1, 0xffffff, kColorBits,
         [](const Mcd212SaveState& s) { return s.transparent_color_[1]; }},
        {kRegMaskColorB, 1, 0xffffff, kColorBits, [](const Mcd212SaveState& s) { return s.mask_color_[1]; }},
        {kRegDyuvAbsStartA, 0, 0x108080, 0x108080, [](const Mcd212SaveState& s) { return s.dyuv_abs_start_[0]; }},
        {kRegDyuvAbsStartB, 1, 0x108080, 0x108080, [](const Mcd212SaveState& s) { return s.dyuv_abs_start_[1]; }},
        {kRegPixelHoldB, 1, 0x800004, 0x800004, [](const Mcd212SaveState& s) { return s.pixel_hold_[1]; }},
        // ICF'(5-0), Green Book V.5.9.
        {kRegWeightFactorA, 0, 0xff, 0x3f, [](const Mcd212SaveState& s) { return uint32_t(s.weight_factor_[0]); }},
        {kRegWeightFactorB, 1, 0xff, 0x3f, [](const Mcd212SaveState& s) { return uint32_t(s.weight_factor_[1]); }},
    };
    for (const Case& c : cases) {
        CAPTURE(int(c.reg));
        Mcd212 mcd;
        load_register(mcd, 1 - c.path, c.reg, c.value);
        CHECK(c.read(mcd.state()) == 0);
        load_register(mcd, c.path, c.reg, c.value);
        CHECK(c.read(mcd.state()) == c.expected);
    }
}

TEST_CASE("either path loads Region Control") {
    Mcd212 mcd;
    load_register(mcd, 0, uint8_t(kRegRegionControl + 2), 0x123456);
    load_register(mcd, 1, uint8_t(kRegRegionControl + 7), 0x654321);
    CHECK(mcd.state().region_control_[2] == 0x123456);
    CHECK(mcd.state().region_control_[7] == 0x654321);
}

}  // TEST_SUITE

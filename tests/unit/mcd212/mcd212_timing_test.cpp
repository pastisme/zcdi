// When the MCD212 does its line work.
#include "doctest.h"

#include <cstddef>
#include <cstdint>

#include "chips/mcd212/mcd212.h"
#include "support/mcd212_field.h"
#include "support/mcd212_regs.h"

using zcdi::chips::Mcd212;
using zcdi_test::load_register;
using zcdi_test::write_mmio16;
using namespace zcdi::chips::mcd212;


namespace {

constexpr int kPalLines = 312, kFirstActive = 26, kActive = 280;  // PAL, 280 lines

void put32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}

bool display_active(Mcd212& mcd) { return (mcd.read_byte(kCsr1R) & 0x80) != 0; }

}  // namespace

TEST_SUITE("mcd212") {

TEST_CASE("Display Active is set during the active lines only") {
    static uint8_t planes[2][0x80000] = {};
    Mcd212 mcd;
    mcd.begin_field(kPalLines);
    CHECK_FALSE(display_active(mcd));
    for (int line = 0; line < kPalLines; ++line) {
        CAPTURE(line);
        mcd.end_of_line(line, planes[0], planes[1], sizeof(planes[0]));
        // Seen during the next line.
        const bool next_active = line + 1 >= kFirstActive && line + 1 < kFirstActive + kActive;
        CHECK(display_active(mcd) == next_active);
    }
    mcd.end_field();
}

TEST_CASE("the ICA interrupts as blanking starts, the first DCA after the first line") {
    static uint8_t planes[2][0x80000] = {};
    uint8_t* a = planes[0];
    put32(a + 0x400, 0x60000000u);  // ICA: INTERRUPT
    put32(a + 0x404, 0x30001000u);  // RELOAD DCP 0x1000 + STOP
    put32(a + 0x1000, 0x60000000u);  // DCA, first line: INTERRUPT
    put32(a + 0x1004, 0x00000000u);  // STOP
    Mcd212 mcd;
    write_mmio16(mcd, kDcr1, kDcrIc);  // IC; DC once the ICA has set DCP
    auto ack = [&] { return (mcd.read_byte(kCsr2R) & 0x04) != 0; };  // IT1, read clears

    mcd.begin_field(kPalLines);
    for (int line = 0; line < kFirstActive + kActive - 1; ++line) mcd.end_of_line(line, a, planes[1], sizeof(planes[0]));
    CHECK_FALSE(ack());
    mcd.end_of_line(kFirstActive + kActive - 1, a, planes[1], sizeof(planes[0]));  // last active line
    CHECK(ack());  // ICA at the start of vertical blanking
    for (int line = kFirstActive + kActive; line < kPalLines; ++line) mcd.end_of_line(line, a, planes[1], sizeof(planes[0]));
    mcd.end_field();

    write_mmio16(mcd, kDcr1, kDcrDe | kDcrIc | kDcrDc);
    mcd.begin_field(kPalLines);
    for (int line = 0; line < kFirstActive; ++line) mcd.end_of_line(line, a, planes[1], sizeof(planes[0]));
    CHECK_FALSE(ack());
    mcd.end_of_line(kFirstActive, a, planes[1], sizeof(planes[0]));
    CHECK(ack());  // the DCA ran in the blanking after the first active line
}

// The DCA and the picture need DE; the ICA doesn't.
TEST_CASE("with DE clear, the DCA doesn't run and the display is black") {
    static uint8_t planes[2][0x80000] = {};
    uint8_t* a = planes[0];
    put32(a + 0x400, 0x30001000u);  // ICA: RELOAD DCP 0x1000 + STOP
    put32(a + 0x1000, 0x60000000u);  // DCA: INTERRUPT
    put32(a + 0x1004, 0x00000000u);  // STOP
    Mcd212 mcd;
    load_register(mcd, 0, kRegBackdropColor, 0xc);  // red
    auto ack = [&] { return (mcd.read_byte(kCsr2R) & 0x04) != 0; };
    auto first_active_pixel = [&] { return mcd.framebuffer()[0]; };

    write_mmio16(mcd, kDcr1, kDcrIc);  // sets DCP
    zcdi_test::run_field(mcd, a, planes[1], sizeof(planes[0]));
    write_mmio16(mcd, kDcr1, kDcrIc | kDcrDc);  // no DE
    zcdi_test::run_field(mcd, a, planes[1], sizeof(planes[0]));
    CHECK_FALSE(ack());
    CHECK(first_active_pixel() == 0x00101010u);

    write_mmio16(mcd, kDcr1, kDcrDe | kDcrIc | kDcrDc);
    zcdi_test::run_field(mcd, a, planes[1], sizeof(planes[0]));
    CHECK(ack());
    CHECK(first_active_pixel() != 0x00101010u);
}

}  // TEST_SUITE

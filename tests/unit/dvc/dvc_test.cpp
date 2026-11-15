// The cartridge as the board drives it: registers, channel-1 DMA, lines and fields.
#include "doctest.h"

#include <cstdint>

#include "dvc/dvc.h"
#include "fixtures/dvc_mpeg1_fixture.h"
#include "support/vmpeg_regs.h"

using zcdi::dvc::Dvc;
using namespace zcdi_test;

namespace {

constexpr int kFieldLines = 312, kFirstActive = 26, kActiveLines = 280;
constexpr int kCyclesPerField = 300000;  // 15 MHz at 50 Hz

void run_field(Dvc& dvc) {
    dvc.tick_cycles(kCyclesPerField);
    for (int line = 0; line < kFieldLines; ++line) dvc.end_of_line(line, kFirstActive, kActiveLines);
    (void)dvc.tick_field(50.0);
}

}  // namespace

TEST_SUITE("dvc") {

TEST_CASE("a stream sent by DMA is decoded, shown with PIC, and dropped by Decoder Off") {
    Dvc dvc;
    dvc.set_cartridge_present(true);
    write16(dvc, kFmvSyscmdOffset, 0x1000);  // Decoder On
    write16(dvc, kFmvSyscmdOffset, 0x8000);  // start DMA, to video
    REQUIRE(dvc.dma_active());
    uint32_t words = 0;
    for (size_t i = 0; i + 1 < kDvcMpeg1FixtureSize; i += 2, ++words)
        dvc.feed_dma_word(uint16_t(kDvcMpeg1FixtureBytes[i] << 8 | kDvcMpeg1FixtureBytes[i + 1]));
    dvc.complete_dma(words);
    CHECK_FALSE(dvc.dma_active());

    write16(dvc, kFmvSyscmdOffset, 0x0008);  // Play
    for (int field = 0; field < 50 && !dvc.has_frame(); ++field) run_field(dvc);
    REQUIRE(dvc.has_frame());
    CHECK(dvc.width() == kDvcMpeg1Width);
    CHECK(dvc.height() == kDvcMpeg1Height);
    // PIC comes with the picture's first line, in the next field.
    CHECK((read16(dvc, kFmvIsrOffset) & zcdi::dvc::vmpeg::kIntPic) == 0);
    for (int line = 0; line < kFirstActive; ++line) dvc.end_of_line(line, kFirstActive, kActiveLines);
    CHECK((read16(dvc, kFmvIsrOffset) & zcdi::dvc::vmpeg::kIntPic) != 0);

    write16(dvc, kFmvSyscmdOffset, 0x2000);  // Decoder Off
    CHECK(dvc.tick_field(50.0));
    CHECK_FALSE(dvc.has_frame());
    CHECK_FALSE(dvc.tick_field(50.0));
}

}

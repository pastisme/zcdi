// CDi_MiSTer vmpeg.sv fmv_demuxer_scr_update_request_cnt (440e4a3).
#include "doctest.h"

#include <cstdint>
#include <vector>

#include "vmpeg/vmpeg.h"
#include "support/vmpeg_regs.h"

using zcdi::mamecdi::VmpegDevice;
using namespace zcdi_test;

namespace {
constexpr int kCyclesPerDclkTick = 1000 / 3 + 1;  // 3 ticks per 1000 CPU cycles

// A pack header with SCR = `scr` (90 kHz), MPEG-1 layout.
void feed_pack(VmpegDevice& v, int64_t scr) {
    const std::vector<uint8_t> pack = {0,
                                       0,
                                       1,
                                       0xba,
                                       uint8_t(0x21 | ((scr >> 30) & 7) << 1),
                                       uint8_t(scr >> 22),
                                       uint8_t(((scr >> 15) & 0x7f) << 1 | 1),
                                       uint8_t(scr >> 7),
                                       uint8_t((scr & 0x7f) << 1 | 1),
                                       0x80,
                                       0,
                                       1};
    for (size_t i = 0; i < pack.size(); i += 2) v.feed_dma_word(uint16_t(pack[i] << 8 | pack[i + 1]));
}

void tick_dclk(VmpegDevice& v, int ticks) {
    for (int i = 0; i < ticks; ++i) v.tick_cycles(kCyclesPerDclkTick);
}
}  // namespace

TEST_SUITE("vmpeg") {

TEST_CASE("the FMV clock takes a pack's SCR 15 dclk ticks after it arrives") {
    VmpegDevice v;
    write16(v, kFmvSyscmdOffset, 0x1000);  // Decoder On
    feed_pack(v, 90000);                   // 1 s: dclk 45000, GEN_SYSCR 703
    tick_dclk(v, 14);
    CHECK(read16(v, kFmvGenSyscrOffset) == 0);
    tick_dclk(v, 1);
    CHECK(read16(v, kFmvGenSyscrOffset) == 703);
}

TEST_CASE("a GEN_SYSCR write cancels the pending SCR load") {
    VmpegDevice v;
    write16(v, kFmvSyscmdOffset, 0x1000);
    feed_pack(v, 90000);
    tick_dclk(v, 5);
    write16(v, kFmvGenSyscrOffset, 100);
    tick_dclk(v, 20);
    CHECK(read16(v, kFmvGenSyscrOffset) == 100);
}

}  // TEST_SUITE

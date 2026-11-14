// The VMPEG clocks: DCLK reads, the TIM timer, and the FMV clock loading a
// pack's SCR unless the CPU writes GEN_SYSCR first.
#include "doctest.h"

#include <cstdint>
#include <vector>

#include "vmpeg/vmpeg.h"
#include "support/vmpeg_regs.h"

using zcdi::mamecdi::VmpegDevice;
using namespace zcdi_test;

namespace {
constexpr uint16_t kIntTim = zcdi::mamecdi::vmpeg::kIntTim;
constexpr uint16_t kDecoderOn = zcdi::mamecdi::vmpeg::kSysCmdDecoderOn;
constexpr int kCyclesPerDclkTick = 1000 / 3 + 1;  // 3 ticks per 1000 CPU cycles

// DCLK ticks until TIM is next raised, one tick at a time.
int ticks_until_tim(VmpegDevice& v) {
    for (int ticks = 1; ticks <= 10000; ++ticks) {
        const uint32_t start = v.free_running_dclk();
        while (v.free_running_dclk() == start) v.tick_cycles(1);
        if (v.fmv_isr() & kIntTim) return ticks;
    }
    return -1;
}

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

TEST_CASE("DCLK high/low reads return one consistent counter value") {
    VmpegDevice v;

    // Well past a 16-bit rollover, so the low word is nonzero.
    for (int i = 0; i < 5; ++i) v.tick_cycles(1'000'000);
    const uint32_t dclk = v.free_running_dclk();
    REQUIRE(dclk != 0);
    // A coincidental 0 low word would mask the bug.
    REQUIRE((dclk & 0xffffu) != 0);

    const uint16_t hi = read16(v, kFmaDclkOffset);
    const uint16_t lo = read16(v, kFmaDclkOffset + 2);
    CHECK(hi == uint16_t(dclk >> 16));
    CHECK(lo == uint16_t(dclk));
}

// The firmware programs TCNT = 55 (448 ticks, 100.446 Hz).
TEST_CASE("TIM fires every (TCNT + 1) * 8 DCLK ticks") {
    VmpegDevice v;
    write16(v, kFmvTcntOffset, 55);
    write16(v, kFmvTrldOffset, 0);  // restart the count

    CHECK(ticks_until_tim(v) == 448);
    v.read_byte(kFmvIsrOffset);
    v.read_byte(kFmvIsrOffset + 1);  // clears ISR
    REQUIRE((v.fmv_isr() & kIntTim) == 0);
    CHECK(ticks_until_tim(v) == 448);
}

TEST_CASE("the FMV clock takes a pack's SCR 15 dclk ticks after it arrives") {
    VmpegDevice v;
    write16(v, kFmvSyscmdOffset, kDecoderOn);
    feed_pack(v, 90000);  // 1 s: dclk 45000, GEN_SYSCR 703
    tick_dclk(v, 14);
    CHECK(read16(v, kFmvGenSyscrOffset) == 0);
    tick_dclk(v, 1);
    CHECK(read16(v, kFmvGenSyscrOffset) == 703);
}

TEST_CASE("a GEN_SYSCR write cancels the pending SCR load") {
    VmpegDevice v;
    write16(v, kFmvSyscmdOffset, kDecoderOn);
    feed_pack(v, 90000);
    tick_dclk(v, 5);
    write16(v, kFmvGenSyscrOffset, 100);
    tick_dclk(v, 20);
    CHECK(read16(v, kFmvGenSyscrOffset) == 100);
}

}  // TEST_SUITE

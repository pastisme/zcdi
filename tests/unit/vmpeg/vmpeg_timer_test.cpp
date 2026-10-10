// CDi_MiSTer vmpeg.sv; the firmware programs TCNT = 55 (448 ticks, 100.446 Hz).
#include "doctest.h"

#include <cstdint>

#include "vmpeg/vmpeg.h"
#include "support/private_access.h"
#include "support/vmpeg_regs.h"

using zcdi::mamecdi::VmpegDevice;
using zcdi::mamecdi::VmpegSaveState;
using namespace zcdi_test;

ZCDI_PRIVATE_MEMBER(VmpegTimerFmaDclk, VmpegSaveState, fma_dclk_, uint32_t);

namespace {
constexpr uint16_t kIntTim = 1u << 8;

// DCLK ticks until TIM is next raised, one tick at a time.
int ticks_until_tim(VmpegDevice& v) {
    for (int ticks = 1; ticks <= 10000; ++ticks) {
        const uint32_t start = zcdi_test::priv<VmpegTimerFmaDclk>(v);
        while (zcdi_test::priv<VmpegTimerFmaDclk>(v) == start) v.tick_cycles(1);
        if (v.fmv_isr() & kIntTim) return ticks;
    }
    return -1;
}
}  // namespace

TEST_SUITE("vmpeg") {

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

}  // TEST_SUITE

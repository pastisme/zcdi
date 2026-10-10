// CDi_MiSTer vmpeg.sv:678-694.
#include "doctest.h"

#include <cstdint>

#include "vmpeg/vmpeg.h"
#include "support/vmpeg_regs.h"

using zcdi::mamecdi::VmpegDevice;
using zcdi_test::kFmvSyscmdOffset;
using zcdi_test::kFmvVidcmdOffset;
using zcdi_test::write16;

namespace {
constexpr uint16_t kPlay = 0x0008;
constexpr uint16_t kRegsUpd = 0x0008;
constexpr uint16_t kScroll = 0x0004;
constexpr uint16_t kIntVcup = 1u << 13;
constexpr uint16_t kIntDcl = 1u << 7;

bool vcup_dcl(const VmpegDevice& v) { return (v.fmv_isr() & (kIntVcup | kIntDcl)) == (kIntVcup | kIntDcl); }
}  // namespace

TEST_SUITE("vmpeg") {

// mpeg_video.sv:918-921: event_potential_picture_starts_display fires at the
// vsync after each frame period tick, whether a picture is waiting or not.
TEST_CASE("a non-scroll RegsUpd commits at the frame player's display slot") {
    VmpegDevice v;
    v.set_video_frame_decoded(368, 272, /*picture_rate_code=*/3);  // 25 Hz
    write16(v, kFmvVidcmdOffset, kRegsUpd);
    v.notify_vsync();
    CHECK_FALSE(vcup_dcl(v));
    write16(v, kFmvSyscmdOffset, kPlay);
    v.tick_cycles(2'000'000);  // past the start delay
    REQUIRE(v.video_display_enabled());
    CHECK_FALSE(v.frame_player_field(/*field_clocks=*/600'000, /*pictures_ready=*/0).show);
    CHECK_FALSE(vcup_dcl(v));  // half a frame period
    CHECK_FALSE(v.frame_player_field(600'000, 0).show);
    CHECK(vcup_dcl(v));  // no picture to show, the registers latch anyway
}

TEST_CASE("a scroll RegsUpd commits on vsync, not on a picture") {
    VmpegDevice v;
    write16(v, kFmvVidcmdOffset, kRegsUpd | kScroll);
    v.set_video_frame_ready(368, 272);
    CHECK_FALSE(vcup_dcl(v));
    v.notify_vsync();
    CHECK(vcup_dcl(v));
}

TEST_CASE("without a pending RegsUpd a picture raises no VCUP") {
    VmpegDevice v;
    v.set_video_frame_ready(368, 272);
    CHECK((v.fmv_isr() & kIntVcup) == 0);
}

}  // TEST_SUITE

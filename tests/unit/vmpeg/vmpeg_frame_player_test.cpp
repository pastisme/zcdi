// CDi_MiSTer mpeg_video.sv.
#include "doctest.h"

#include <cstdint>
#include <vector>

#include "vmpeg/vmpeg.h"
#include "support/private_access.h"
#include "support/vmpeg_regs.h"

using zcdi::mamecdi::VmpegDevice;
using zcdi::mamecdi::VmpegSaveState;
using zcdi_test::kFmvGenDecCmdOffset;
using zcdi_test::kFmvSyscmdOffset;
using zcdi_test::kFmvSysScrOffset;
using zcdi_test::write16;

ZCDI_PRIVATE_MEMBER(PlayerPlaybackActive, VmpegSaveState, fmv_playback_active_, bool);
ZCDI_PRIVATE_MEMBER(PlayerFmvDclk, VmpegSaveState, fmv_dclk_, uint32_t);
ZCDI_PRIVATE_MEMBER(PlayerDtsFifo, VmpegSaveState, fmv_dts_fifo_, std::vector<int64_t>);
ZCDI_PRIVATE_MEMBER(PlayerDemux, VmpegSaveState, fmv_demux_, zcdi::mamecdi::PsDemux);
ZCDI_PRIVATE_MEMBER(PlayerScrValid, VmpegSaveState, fmv_demux_scr_valid_, bool);

namespace {
constexpr uint32_t kPalFieldClocks = 600000;       // 30 MHz / 50 Hz
constexpr int kRate25 = 3;
constexpr int kRate2997 = 4;

void start_playback(VmpegDevice& v, int rate_code) {
    zcdi_test::priv<PlayerPlaybackActive>(v) = true;
    v.set_video_frame_decoded(352, 288, rate_code);
}

// Fields, out of `fields`, on which a picture is shown, with `ready`
// pictures always waiting.
std::vector<int> shown_fields(VmpegDevice& v, int fields, size_t ready = 8) {
    std::vector<int> shown;
    for (int f = 1; f <= fields; ++f) {
        if (v.frame_player_field(kPalFieldClocks, ready).show) shown.push_back(f);
    }
    return shown;
}

// A DTS (90 kHz) that puts dclk[20:6] - DTS[21:7] at `desync`.
int64_t dts_at_desync(uint32_t dclk, int desync) {
    return int64_t(int((dclk >> 6) & 0x7fff) - desync) << 7;
}
}  // namespace

TEST_SUITE("vmpeg") {

TEST_CASE("a 25 Hz stream shows on every other PAL field") {
    VmpegDevice v;
    start_playback(v, kRate25);
    std::vector<int> shown = shown_fields(v, 20);
    REQUIRE(shown.size() == 10u);
    for (size_t i = 0; i < shown.size(); ++i) CHECK(shown[i] == int(2 * (i + 1)));
}

TEST_CASE("a 29.97 Hz stream keeps its rate on 50 Hz fields") {
    VmpegDevice v;
    start_playback(v, kRate2997);
    // 1000 fields of 600000 clocks over periods of 1001001 clocks.
    std::vector<int> shown = shown_fields(v, 1000);
    CHECK(shown.size() >= 598u);
    CHECK(shown.size() <= 600u);
}

TEST_CASE("nothing is shown while playback is stopped or no picture waits") {
    VmpegDevice v;
    v.set_video_frame_decoded(352, 288, kRate25);
    CHECK(shown_fields(v, 10).empty());  // playback not active

    zcdi_test::priv<PlayerPlaybackActive>(v) = true;
    CHECK(shown_fields(v, 10, /*ready=*/0).empty());
}

TEST_CASE("synchronous mode shortens the period while the clock leads the DTS") {
    // Asynchronous reference: 150 pictures in 300 fields.
    VmpegDevice ref;
    start_playback(ref, kRate25);
    REQUIRE(shown_fields(ref, 300).size() == 150u);

    VmpegDevice v;
    write16(v, kFmvSysScrOffset, 0x0004);  // synchronous
    const uint32_t dclk = 0x100000;
    zcdi_test::priv<PlayerFmvDclk>(v) = dclk;
    // Every queued picture's DTS is 20 units (64 dclk ticks each) behind.
    zcdi_test::priv<PlayerDtsFifo>(v).assign(300, dts_at_desync(dclk, 20));
    start_playback(v, kRate25);
    // Each period is up to 20 * 2048 clocks shorter, so more than 150.
    const size_t shown = shown_fields(v, 300).size();
    CHECK(shown > 150u);
    CHECK(shown <= 156u);
}

TEST_CASE("Step shows one picture while playback is stopped") {
    VmpegDevice v;
    v.set_video_frame_decoded(352, 288, kRate25);
    write16(v, kFmvSyscmdOffset, 0x0040);  // Step
    // Armed at the first vsync, shown at the second (mpeg_video.sv:962-965).
    CHECK(shown_fields(v, 10) == std::vector<int>{2});

    SUBCASE("GEN_DEC_CMD 0x22xx steps too, but not while paused") {
        write16(v, kFmvGenDecCmdOffset, 0x2200);
        CHECK(shown_fields(v, 10) == std::vector<int>{2});
        write16(v, kFmvSyscmdOffset, 0x0010);  // Pause
        write16(v, kFmvGenDecCmdOffset, 0x2200);
        CHECK(shown_fields(v, 10).empty());
    }
}

TEST_CASE("slow motion multiplies the picture period") {
    VmpegDevice v;
    write16(v, kFmvSyscmdOffset, 0x0008 | 1);  // Play at half speed
    start_playback(v, kRate25);
    CHECK(shown_fields(v, 40).size() == 10u);  // every 4th PAL field
}

TEST_CASE("synchronous mode drops pictures far behind the SCR") {
    VmpegDevice v;
    write16(v, kFmvSysScrOffset, 0x0004);
    start_playback(v, kRate25);
    const int64_t scr = 1000000;
    zcdi_test::priv<PlayerDemux>(v).scr = scr;
    zcdi_test::priv<PlayerScrValid>(v) = true;
    // Two pictures more than 15000 behind the SCR, then one in step.
    zcdi_test::priv<PlayerDtsFifo>(v) = {scr - 20000, scr - 16000, scr - 1000};
    VmpegDevice::FramePlayerStep step = v.frame_player_field(kPalFieldClocks, 3);
    CHECK(step.skipped == 2);
    CHECK_FALSE(step.show);

    SUBCASE("asynchronous mode never drops") {
        write16(v, kFmvSysScrOffset, 0x0000);
        CHECK(v.frame_player_field(kPalFieldClocks, 3).skipped == 0);
    }
}

}  // TEST_SUITE

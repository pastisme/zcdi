// The frame player: picture timing, DTS sync and stepping.
#include "doctest.h"

#include <cstdint>
#include <vector>

#include "chips/vmpeg/vmpeg.h"
#include "support/vmpeg_private.h"
#include "support/vmpeg_regs.h"

using zcdi::chips::VmpegDevice;
using zcdi_test::kFmvGenDecCmdOffset;
using zcdi_test::kFmvSyscmdOffset;
using zcdi_test::kFmvSysScrOffset;
using zcdi_test::write16;

namespace {
constexpr uint32_t kPalFieldClocks = 600000;       // 30 MHz / 50 Hz
constexpr int kRate25 = 3;
constexpr int kRate2997 = 4;

void start_playback(VmpegDevice& v, int rate_code) {
    zcdi_test::set_playback_active(v, true);
    v.set_video_frame_decoded(rate_code);
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

// The desync measured for each shown picture, popping it from the dts_fifo as Bus does.
std::vector<int> shown_desyncs(VmpegDevice& v, size_t count) {
    std::vector<int> out;
    for (int f = 0; f < 100 && out.size() < count; ++f) {
        if (!v.frame_player_field(kPalFieldClocks, 8).show) continue;
        out.push_back(v.state().player_desync_q_);
        v.note_picture_displayed();
    }
    return out;
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
    v.set_video_frame_decoded(kRate25);
    CHECK(shown_fields(v, 10).empty());  // playback not active

    zcdi_test::set_playback_active(v, true);
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
    zcdi_test::set_fmv_dclk(v, dclk);
    // Every queued picture's DTS is 20 units (64 dclk ticks each) behind.
    zcdi_test::dts_fifo(v).assign(300, dts_at_desync(dclk, 20));
    start_playback(v, kRate25);
    // Each period is up to 20 * 2048 clocks shorter, so more than 150.
    const size_t shown = shown_fields(v, 300).size();
    CHECK(shown > 150u);
    CHECK(shown <= 156u);
}

TEST_CASE("Step shows one picture while playback is stopped") {
    VmpegDevice v;
    v.set_video_frame_decoded(kRate25);
    write16(v, kFmvSyscmdOffset, 0x0040);  // Step
    // Armed at the first vsync, shown at the second.
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

TEST_CASE("far behind the SCR pictures come at the shortest period and none is dropped") {
    VmpegDevice v;
    start_playback(v, kRate25);
    const int64_t scr = 1000000;
    zcdi_test::set_demux_scr(v, scr);
    zcdi_test::dts_fifo(v).assign(300, scr - 20000);
    // 7/8 of 1200000 clocks: 300 fields of 600000 hold 171 periods of 1050001.
    CHECK(shown_fields(v, 300).size() == 171u);
}

TEST_CASE("the period stays within 1/8 of the picture period") {
    for (int desync : {200, -200}) {
        CAPTURE(desync);
        VmpegDevice v;
        write16(v, kFmvSysScrOffset, 0x0004);
        const uint32_t dclk = 0x100000;
        zcdi_test::set_fmv_dclk(v, dclk);
        zcdi_test::dts_fifo(v).assign(300, dts_at_desync(dclk, desync));
        start_playback(v, kRate25);
        zcdi_test::set_dts_start_satisfied(v);  // past the start wait, whatever the DTS
        // 200 units * 2048 clocks is far more than 1/8 of 1200000. The first
        // picture comes after a full period, before any desync is measured.
        const size_t shown = shown_fields(v, 300).size();
        if (desync > 0) CHECK(shown == 171u);  // then periods of 1050001
        else CHECK(shown == 133u);             // then periods of 1350001
    }
}

TEST_CASE("a measured desync holds for 3 pictures without a timestamp") {
    VmpegDevice v;
    write16(v, kFmvSysScrOffset, 0x0004);
    const uint32_t dclk = 0x100000;
    zcdi_test::set_fmv_dclk(v, dclk);
    // No Decoder On: no 90 kHz period, so no predicted DTS.
    zcdi_test::dts_fifo(v) = {dts_at_desync(dclk, 20), 0, 0, 0, 0, 0};
    start_playback(v, kRate25);
    std::vector<int> desyncs = shown_desyncs(v, 5);
    REQUIRE(desyncs.size() == 5u);
    CHECK(desyncs[0] != 0);
    for (int i = 1; i <= 3; ++i) CHECK(desyncs[size_t(i)] == desyncs[0]);
    CHECK(desyncs[4] == 0);
}

TEST_CASE("a picture without a timestamp syncs to the last DTS plus a picture period") {
    auto second_desync = [](bool second_has_dts) {
        VmpegDevice v;
        write16(v, kFmvSyscmdOffset, 0x1000);  // Decoder On: the 90 kHz period counts
        write16(v, kFmvSysScrOffset, 0x0004);
        const uint32_t dclk = 0x100000;
        zcdi_test::set_fmv_dclk(v, dclk);
        const int64_t first = dts_at_desync(dclk, 20);
        zcdi_test::dts_fifo(v) = {first, second_has_dts ? first + 3600 : 0, 0};
        start_playback(v, kRate25);
        return shown_desyncs(v, 2);
    };
    const std::vector<int> predicted = second_desync(false);
    REQUIRE(predicted.size() == 2u);
    CHECK(predicted == second_desync(true));
}

TEST_CASE("slow motion doesn't sync to the DTS") {
    VmpegDevice v;
    write16(v, kFmvSysScrOffset, 0x0004);
    write16(v, kFmvSyscmdOffset, 0x0008 | 1);  // Play at half speed
    const uint32_t dclk = 0x100000;
    zcdi_test::set_fmv_dclk(v, dclk);
    zcdi_test::dts_fifo(v).assign(300, dts_at_desync(dclk, 20));
    start_playback(v, kRate25);
    CHECK(shown_fields(v, 40).size() == 10u);  // every 4th PAL field, no shortening
}

}  // TEST_SUITE

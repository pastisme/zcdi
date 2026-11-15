// Buffer underflow: NDAT once per play, on line 11.
#include "doctest.h"

#include <cstdint>
#include <vector>

#include "dvc/vmpeg.h"
#include "support/dvc_private.h"
#include "support/vmpeg_regs.h"

using zcdi::dvc::Dvc;
using namespace zcdi::dvc::vmpeg;
using namespace zcdi_test;

namespace {
constexpr uint32_t kPalFieldClocks = 600000;  // 30 MHz / 50 Hz
constexpr int kRate25 = 3;

void run_lines(Dvc& v, int from, int to) {
    for (int line = from; line < to; ++line) v.end_of_line(line, 26, 280);
}

void play(Dvc& v) {
    set_playback_active(v, true);
    set_video_frame_decoded(v, kRate25);
}

// Two PAL fields: one 25 Hz frame period, so playback_active_delayed follows playback.
void run_frame_period(Dvc& v) {
    frame_player_field(v, kPalFieldClocks, 8);
    frame_player_field(v, kPalFieldClocks, 8);
}

// Shows a picture with `in + dec + out` pictures in the DTS FIFO, then runs lines 0-11.
bool ndat_after_show(Dvc& v, int in, int dec, int out) {
    notify_vsync(v);
    read16(v, kFmvIsrOffset);  // reading clears the ISR
    dts_fifo(v).assign(size_t(in + dec + out), 0);
    check_buffer_underflow(v, size_t(out), dec);
    run_lines(v, 0, 12);
    return (fmv_isr(v) & kIntNdat) != 0;
}
}  // namespace

TEST_SUITE("vmpeg") {

TEST_CASE("NDAT is raised on line 11 when the last pictures are left") {
    Dvc v;
    play(v);
    frame_player_field(v, kPalFieldClocks, 8);
    dts_fifo(v).assign(3, 0);  // 1 input, 1 decoder, 1 output
    check_buffer_underflow(v, 1, 1);
    run_lines(v, 0, 10);
    CHECK((fmv_isr(v) & kIntNdat) == 0);
    run_lines(v, 10, 11);
    CHECK((fmv_isr(v) & kIntNdat) != 0);
}

TEST_CASE("NDAT waits while pictures are queued, decoded or still arriving") {
    Dvc v;
    play(v);
    frame_player_field(v, kPalFieldClocks, 8);
    CHECK_FALSE(ndat_after_show(v, 0, 0, 2));
    CHECK_FALSE(ndat_after_show(v, 0, 2, 1));
    CHECK_FALSE(ndat_after_show(v, 2, 1, 1));  // the picture being decoded counts as input
    CHECK(ndat_after_show(v, 1, 1, 1));
}

TEST_CASE("NDAT is raised once until Clear FIFO or Decoder Off") {
    Dvc v;
    play(v);
    run_frame_period(v);  // pictures are shown on frame period ticks
    CHECK(ndat_after_show(v, 0, 0, 0));
    run_frame_period(v);
    CHECK_FALSE(ndat_after_show(v, 0, 0, 0));

    // Pause and Continue keep playback_active_delayed set.
    set_playback_active(v, false);
    frame_player_field(v, kPalFieldClocks, 8);
    set_playback_active(v, true);
    run_frame_period(v);
    CHECK_FALSE(ndat_after_show(v, 0, 0, 0));

    write16(v, kFmvSyscmdOffset, kSysCmdClearFifo);
    play(v);
    run_frame_period(v);
    CHECK(ndat_after_show(v, 0, 0, 0));
}

TEST_CASE("a pending NDAT is dropped at the next vsync") {
    Dvc v;
    play(v);
    frame_player_field(v, kPalFieldClocks, 8);
    check_buffer_underflow(v, 0, 0);
    run_lines(v, 0, 5);
    notify_vsync(v);
    run_lines(v, 0, 20);
    CHECK((fmv_isr(v) & kIntNdat) == 0);
}

}  // TEST_SUITE

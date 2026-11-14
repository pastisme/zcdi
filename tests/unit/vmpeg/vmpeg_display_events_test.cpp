// Picture events at the end of vertical blanking.
#include "doctest.h"

#include <cstdint>

#include "vmpeg/vmpeg.h"
#include "support/vmpeg_regs.h"

using zcdi::mamecdi::VmpegDevice;
using namespace zcdi::mamecdi::vmpeg;
using namespace zcdi_test;

namespace {
constexpr int kFirstActiveLine = 26;  // PAL, 280 lines

void run_lines(VmpegDevice& v, int from, int to) {
    for (int line = from; line < to; ++line) v.notify_line(line, kFirstActiveLine, 280);
}
}  // namespace

TEST_SUITE("vmpeg") {

TEST_CASE("PIC GOP SEQ and EOD are raised when vertical blanking ends") {
    VmpegDevice v;
    v.set_video_frame_ready(352, 240);
    v.set_video_gop_start();
    v.set_video_sequence_start();
    v.set_video_last_picture();
    run_lines(v, 0, kFirstActiveLine - 1);
    CHECK(v.fmv_isr() == 0);
    run_lines(v, kFirstActiveLine - 1, kFirstActiveLine);
    CHECK(v.fmv_isr() == (kIntPic | kIntGop | kIntSeq | kIntEod));
}

TEST_CASE("picture events are raised once") {
    VmpegDevice v;
    v.set_video_frame_ready(352, 240);
    run_lines(v, 0, 312);
    REQUIRE((read16(v, kFmvIsrOffset) & kIntPic) != 0);  // reading clears the ISR
    run_lines(v, 0, 312);
    CHECK(v.fmv_isr() == 0);
}

}  // TEST_SUITE

// Picture events at the end of vertical blanking.
#include "doctest.h"

#include <cstdint>

#include "dvc/dvc.h"
#include "support/dvc_private.h"
#include "support/vmpeg_regs.h"

using zcdi::dvc::Dvc;
using namespace zcdi::dvc::vmpeg;
using namespace zcdi_test;

namespace {
constexpr int kFirstActiveLine = 26;  // PAL, 280 lines

void run_lines(Dvc& v, int from, int to) {
    for (int line = from; line < to; ++line) v.end_of_line(line, kFirstActiveLine, 280);
}
}  // namespace

TEST_SUITE("vmpeg") {

TEST_CASE("PIC GOP SEQ and EOD are raised when vertical blanking ends") {
    Dvc v;
    set_video_frame_ready(v, 352, 240);
    set_video_gop_start(v);
    set_video_sequence_start(v);
    set_video_last_picture(v);
    run_lines(v, 0, kFirstActiveLine - 1);
    CHECK(fmv_isr(v) == 0);
    run_lines(v, kFirstActiveLine - 1, kFirstActiveLine);
    CHECK(fmv_isr(v) == (kIntPic | kIntGop | kIntSeq | kIntEod));
}

TEST_CASE("picture events are raised once") {
    Dvc v;
    set_video_frame_ready(v, 352, 240);
    run_lines(v, 0, 312);
    REQUIRE((read16(v, kFmvIsrOffset) & kIntPic) != 0);  // reading clears the ISR
    run_lines(v, 0, 312);
    CHECK(fmv_isr(v) == 0);
}

}  // TEST_SUITE

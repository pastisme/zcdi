// FMV decoder and display time code registers.
#include "doctest.h"

#include <cstdint>

#include "dvc/dvc.h"
#include "support/dvc_private.h"
#include "support/vmpeg_regs.h"

using zcdi::dvc::Dvc;
using namespace zcdi_test;

namespace {
// 00:01:02:03 in the register layout.
constexpr uint32_t kTimecode = (2u << 22) | (3u << 16) | (0u << 6) | 1u;
}  // namespace

TEST_SUITE("vmpeg") {

TEST_CASE("time codes read back once the decoder is on") {
    Dvc v;
    set_video_decoder_timecode(v, kTimecode);
    set_video_display_timecode(v, kTimecode);
    CHECK(read32(v, kFmvDecoderTimecodeOffset) == 0);
    CHECK(read32(v, kFmvDisplayTimecodeOffset) == 0);

    write16(v, kFmvSyscmdOffset, 0x1000);  // Decoder On
    set_video_decoder_timecode(v, kTimecode);
    CHECK(read32(v, kFmvDecoderTimecodeOffset) == kTimecode);
    CHECK(read32(v, kFmvDisplayTimecodeOffset) == 0);
    set_video_display_timecode(v, kTimecode);
    CHECK(read32(v, kFmvDisplayTimecodeOffset) == kTimecode);
}

TEST_CASE("Clear FIFO and Decoder Off clear the time codes") {
    Dvc v;
    write16(v, kFmvSyscmdOffset, 0x1000);
    set_video_decoder_timecode(v, kTimecode);
    set_video_display_timecode(v, kTimecode);

    SUBCASE("Clear FIFO leaves the decoder on") {
        write16(v, kFmvSyscmdOffset, 0x0100);
        CHECK(read32(v, kFmvDecoderTimecodeOffset) == 0);
        CHECK(read32(v, kFmvDisplayTimecodeOffset) == 0);
        set_video_display_timecode(v, kTimecode);
        CHECK(read32(v, kFmvDisplayTimecodeOffset) == kTimecode);
    }
    SUBCASE("Decoder Off keeps them at 0") {
        write16(v, kFmvSyscmdOffset, 0x2000);
        CHECK(read32(v, kFmvDecoderTimecodeOffset) == 0);
        CHECK(read32(v, kFmvDisplayTimecodeOffset) == 0);
        set_video_display_timecode(v, kTimecode);
        CHECK(read32(v, kFmvDisplayTimecodeOffset) == 0);
    }
}

}  // TEST_SUITE

// CDi_MiSTer vmpeg.sv:527-533.
#include "doctest.h"

#include <cstdint>

#include "vmpeg/vmpeg.h"
#include "support/vmpeg_regs.h"

using zcdi::mamecdi::VmpegDevice;
using namespace zcdi_test;

namespace {
// 00:01:02:03 in the register layout.
constexpr uint32_t kTimecode = (2u << 22) | (3u << 16) | (0u << 6) | 1u;
}  // namespace

TEST_SUITE("vmpeg") {

TEST_CASE("time codes read back once the decoder is on") {
    VmpegDevice v;
    v.set_video_decoder_timecode(kTimecode);
    v.set_video_display_timecode(kTimecode);
    CHECK(read32(v, kFmvDecoderTimecodeOffset) == 0);
    CHECK(read32(v, kFmvDisplayTimecodeOffset) == 0);

    write16(v, kFmvSyscmdOffset, 0x1000);  // Decoder On
    v.set_video_decoder_timecode(kTimecode);
    CHECK(read32(v, kFmvDecoderTimecodeOffset) == kTimecode);
    CHECK(read32(v, kFmvDisplayTimecodeOffset) == 0);
    v.set_video_display_timecode(kTimecode);
    CHECK(read32(v, kFmvDisplayTimecodeOffset) == kTimecode);
}

TEST_CASE("Clear FIFO and Decoder Off clear the time codes") {
    VmpegDevice v;
    write16(v, kFmvSyscmdOffset, 0x1000);
    v.set_video_decoder_timecode(kTimecode);
    v.set_video_display_timecode(kTimecode);

    SUBCASE("Clear FIFO leaves the decoder on") {
        write16(v, kFmvSyscmdOffset, 0x0100);
        CHECK(read32(v, kFmvDecoderTimecodeOffset) == 0);
        CHECK(read32(v, kFmvDisplayTimecodeOffset) == 0);
        v.set_video_display_timecode(kTimecode);
        CHECK(read32(v, kFmvDisplayTimecodeOffset) == kTimecode);
    }
    SUBCASE("Decoder Off keeps them at 0") {
        write16(v, kFmvSyscmdOffset, 0x2000);
        CHECK(read32(v, kFmvDecoderTimecodeOffset) == 0);
        CHECK(read32(v, kFmvDisplayTimecodeOffset) == 0);
        v.set_video_display_timecode(kTimecode);
        CHECK(read32(v, kFmvDisplayTimecodeOffset) == 0);
    }
}

}  // TEST_SUITE

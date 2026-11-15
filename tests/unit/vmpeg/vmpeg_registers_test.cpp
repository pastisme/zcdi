// VMPEG register reads and writes.
#include "doctest.h"

#include <cstdint>

#include "chips/vmpeg/vmpeg.h"
#include "support/vmpeg_regs.h"

using zcdi::VideoStandard;
using zcdi::chips::VmpegDevice;
using namespace zcdi_test;

namespace {
constexpr uint16_t kDecoderOn = zcdi::chips::vmpeg::kSysCmdDecoderOn;
constexpr uint16_t kDecoderOff = zcdi::chips::vmpeg::kSysCmdDecoderOff;
constexpr uint16_t kPlay = zcdi::chips::vmpeg::kSysCmdPlay;
constexpr int kRate25 = 3;  // picture_rate code: 25 Hz
}  // namespace

TEST_SUITE("vmpeg") {

TEST_CASE("GEN_SYSCR reads dclk bits 20 to 6 with bit 21 clear") {
    VmpegDevice v;
    write16(v, kFmvGenSyscrOffset, 0xffff);
    CHECK(read16(v, kFmvGenSyscrOffset) == 0x7fff);
}

TEST_CASE("GEN_SYSCR writes leave dclk bit 21 alone") {
    VmpegDevice v;
    write16(v, kFmvGenSyscrOffset, 0x8000);
    CHECK(read16(v, kFmvGenSyscrOffset) == 0);
    write16(v, kFmvGenSyscrOffset, 0x1234);
    CHECK(read16(v, kFmvGenSyscrOffset) == 0x1234);
}

TEST_CASE("GEN_FRAME_RATE reads 1 whatever was written") {
    VmpegDevice v;
    CHECK(read16(v, kFmvFrameRateOffset) == 1);
    write16(v, kFmvFrameRateOffset, 0x0708);
    CHECK(read16(v, kFmvFrameRateOffset) == 1);
}

TEST_CASE("PICRATE reads twice the display rate until a picture is decoded") {
    VmpegDevice v;
    v.set_video_standard(VideoStandard::kPal);
    write16(v, kFmvSyscmdOffset, kDecoderOn);
    CHECK(read16(v, kFmvPicRateOffset) == 2 * 0x0708);
    v.set_video_frame_decoded(kRate25);
    CHECK(read16(v, kFmvPicRateOffset) == 3600);  // 90 kHz / 25
}

TEST_CASE("Pic Rt reads the displayed picture's rate code and not the decoded one") {
    VmpegDevice v;
    write16(v, kFmvSyscmdOffset, kDecoderOn);
    v.set_video_frame_decoded(kRate25);
    CHECK(read16(v, kFmvPicRtOffset) == 0);
    v.set_video_display_picture_rate(kRate25);
    CHECK(read16(v, kFmvPicRtOffset) == kRate25);
}

TEST_CASE("Pic Rt reads 0 while the decoder is off") {
    VmpegDevice v;
    v.set_video_display_picture_rate(kRate25);
    CHECK(read16(v, kFmvPicRtOffset) == 0);
}

TEST_CASE("Play sets GEN_DEC_CMD bits 6 5 1 and Decoder Off clears them") {
    VmpegDevice v;
    write16(v, kFmvGenDecCmdOffset, 0x0100);
    write16(v, kFmvSyscmdOffset, kDecoderOn);
    write16(v, kFmvSyscmdOffset, kPlay);
    CHECK(read16(v, kFmvGenDecCmdOffset) == 0x0162);
    write16(v, kFmvSyscmdOffset, kDecoderOff);
    CHECK(read16(v, kFmvGenDecCmdOffset) == 0x0100);
}

}  // TEST_SUITE

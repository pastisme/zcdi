// VMPEG register reads and writes.
#include "doctest.h"

#include <cstdint>

#include "dvc/dvc.h"
#include "support/dvc_private.h"
#include "support/vmpeg_regs.h"

using zcdi::VideoStandard;
using zcdi::dvc::Dvc;
using namespace zcdi_test;

namespace {
constexpr uint16_t kDecoderOn = zcdi::dvc::vmpeg::kSysCmdDecoderOn;
constexpr uint16_t kDecoderOff = zcdi::dvc::vmpeg::kSysCmdDecoderOff;
constexpr uint16_t kPlay = zcdi::dvc::vmpeg::kSysCmdPlay;
constexpr int kRate25 = 3;  // picture_rate code: 25 Hz
}  // namespace

TEST_SUITE("vmpeg") {

TEST_CASE("GEN_SYSCR reads dclk bits 20 to 6 with bit 21 clear") {
    Dvc v;
    write16(v, kFmvGenSyscrOffset, 0xffff);
    CHECK(read16(v, kFmvGenSyscrOffset) == 0x7fff);
}

TEST_CASE("GEN_SYSCR writes leave dclk bit 21 alone") {
    Dvc v;
    write16(v, kFmvGenSyscrOffset, 0x8000);
    CHECK(read16(v, kFmvGenSyscrOffset) == 0);
    write16(v, kFmvGenSyscrOffset, 0x1234);
    CHECK(read16(v, kFmvGenSyscrOffset) == 0x1234);
}

TEST_CASE("GEN_FRAME_RATE reads 1 whatever was written") {
    Dvc v;
    CHECK(read16(v, kFmvFrameRateOffset) == 1);
    write16(v, kFmvFrameRateOffset, 0x0708);
    CHECK(read16(v, kFmvFrameRateOffset) == 1);
}

TEST_CASE("PICRATE reads twice the display rate until a picture is decoded") {
    Dvc v;
    v.set_video_standard(VideoStandard::kPal);
    write16(v, kFmvSyscmdOffset, kDecoderOn);
    CHECK(read16(v, kFmvPicRateOffset) == 2 * 0x0708);
    set_video_frame_decoded(v, kRate25);
    CHECK(read16(v, kFmvPicRateOffset) == 3600);  // 90 kHz / 25
}

TEST_CASE("Pic Rt reads the displayed picture's rate code and not the decoded one") {
    Dvc v;
    write16(v, kFmvSyscmdOffset, kDecoderOn);
    set_video_frame_decoded(v, kRate25);
    CHECK(read16(v, kFmvPicRtOffset) == 0);
    set_video_display_picture_rate(v, kRate25);
    CHECK(read16(v, kFmvPicRtOffset) == kRate25);
}

TEST_CASE("Pic Rt reads 0 while the decoder is off") {
    Dvc v;
    set_video_display_picture_rate(v, kRate25);
    CHECK(read16(v, kFmvPicRtOffset) == 0);
}

TEST_CASE("Play sets GEN_DEC_CMD bits 6 5 1 and Decoder Off clears them") {
    Dvc v;
    write16(v, kFmvGenDecCmdOffset, 0x0100);
    write16(v, kFmvSyscmdOffset, kDecoderOn);
    write16(v, kFmvSyscmdOffset, kPlay);
    CHECK(read16(v, kFmvGenDecCmdOffset) == 0x0162);
    write16(v, kFmvSyscmdOffset, kDecoderOff);
    CHECK(read16(v, kFmvGenDecCmdOffset) == 0x0100);
}

}  // TEST_SUITE

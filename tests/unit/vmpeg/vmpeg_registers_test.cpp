// VMPEG register reads and writes.
#include "doctest.h"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "dvc/dvc.h"
#include "support/dvc_private.h"
#include "support/vmpeg_feed.h"
#include "support/vmpeg_regs.h"

using zcdi::VideoStandard;
using zcdi::dvc::Dvc;
using namespace zcdi_test;

namespace {
constexpr uint16_t kDecoderOn = zcdi::dvc::vmpeg::kSysCmdDecoderOn;
constexpr uint16_t kDecoderOff = zcdi::dvc::vmpeg::kSysCmdDecoderOff;
constexpr uint16_t kPlay = zcdi::dvc::vmpeg::kSysCmdPlay;
constexpr int kRate25 = 3;  // picture_rate code: 25 Hz

using namespace zcdi::dvc::vmpeg;

// A register's byte offset from 0xe00000.
constexpr uint32_t at(uint32_t word_index) { return word_index * 2; }

// Runs until the FMA DCLK (45 kHz) reads `ticks`.
void run_to_dclk(Dvc& v, uint32_t ticks) {
    while (v.fma_state().fma_dclk_ < ticks) v.tick_cycles(1);
    REQUIRE(v.fma_state().fma_dclk_ == ticks);
}

// A pack (SCR 0), then an audio PES packet with PTS `pts` (90 kHz).
std::vector<uint8_t> audio_pack(uint32_t pts) {
    std::vector<uint8_t> out = {0x00, 0x00, 0x01, 0xba, 0x21, 0x00, 0x01, 0x00, 0x01, 0x80, 0x00, 0x01};
    out.insert(out.end(), {0x00, 0x00, 0x01, 0xc0, 0x00, 0x07});
    out.insert(out.end(), {uint8_t(0x21 | ((pts >> 29) & 0x0e)), uint8_t(pts >> 22), uint8_t((pts >> 14) | 1),
                           uint8_t(pts >> 7), uint8_t((pts << 1) | 1)});
    out.insert(out.end(), {0xff, 0xfd});
    return out;
}
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

// Constants the RTL's VMPEG returns; their meaning is unknown.
TEST_CASE("the fixed words read their constants, whatever is written") {
    Dvc v;
    const std::pair<uint32_t, uint16_t> fixed[] = {
        {kFmaFixed04, 0x0007}, {kFmaFixed06, 0x0900}, {kFmaFixed0e, 0x0042}, {kFmaDspd, 0x0004},
        {kFmvGenSyncDiff, 0},  {kFmvGenDecDelay, 1},  {kFmvMmuBase, 0},
    };
    for (const auto& [reg, value] : fixed) {
        CAPTURE(reg);
        CHECK(read16(v, at(reg)) == value);
        write16(v, at(reg), 0xffff);
        CHECK(read16(v, at(reg)) == value);
    }
    CHECK((read16(v, at(kFmaStatus)) >> 8) == 0x02);
}

TEST_CASE("the setup registers read back what was written") {
    Dvc v;
    const uint32_t regs[] = {
        kFmaIvec,  kFmaIer,     kFmvIer,     kFmvIvec,    kFmvTcnt,    kFmvVoff,       kFmvHoff,
        kFmvVpix,  kFmvHpix,    kFmvScrposY, kFmvScrposX, kFmvDecwinH, kFmvDecwinW,    kFmvDecoffY,
        kFmvDecoffX, kFmvGenVdiCmd, kFmvImageWidth, kFmvImageHeight, kFmvImageRt,
    };
    for (uint32_t reg : regs) write16(v, at(reg), uint16_t(0x0100 + reg));
    for (uint32_t reg : regs) {
        CAPTURE(reg);
        CHECK(read16(v, at(reg)) == uint16_t(0x0100 + reg));
    }
}

TEST_CASE("stream numbers keep 4 bits") {
    Dvc v;
    write16(v, at(kFmaStreamW), 0x001f);
    CHECK(read16(v, at(kFmaStreamR)) == 0x000f);
    write16(v, at(kFmvStreamNum), 0x0013);
    CHECK(read16(v, at(kFmvStreamNum)) == 0x0003);
}

TEST_CASE("the registers repeat every 64 KB") {
    Dvc v;
    write16(v, 0x10000 + at(kFmvTcnt), 0x0037);
    CHECK(read16(v, at(kFmvTcnt)) == 0x0037);
    CHECK(read16(v, 0x30000 + at(kFmvTcnt)) == 0x0037);
}

TEST_CASE("bit 0 of the VCD register at 0xe01000 picks the 13.5 MHz pixel clock") {
    Dvc v;
    write16(v, 0x1000, 0x0001);
    CHECK(v.display_window().vcd_pixel_clock);
    write16(v, 0x1002, 0x0000);
    CHECK_FALSE(v.display_window().vcd_pixel_clock);
}

// As the RTL: the FMA's vector register is the vector, the FMV's holds it in bits 3-10.
TEST_CASE("IACK takes the FMA's vector while the FMA requests, else the FMV's") {
    Dvc v;
    write16(v, at(kFmaIvec), 0x0064);
    write16(v, at(kFmvIvec), 0x0328);
    write16(v, at(kFmvTcnt), 0);  // TIM every 8 DCLK ticks, raised in both ISRs
    run_to_dclk(v, 8);
    CHECK_FALSE(v.irq_asserted());

    write16(v, at(kFmvIer), kIntTim);
    CHECK(v.irq_asserted());
    CHECK(v.iack_vector() == 0x65);
    write16(v, at(kFmaIer), kFmaIntTim);
    CHECK(v.iack_vector() == 0x64);

    read16(v, at(kFmaIsr));  // cleared by reading
    CHECK(v.iack_vector() == 0x65);
    read16(v, at(kFmvIsr));
    CHECK_FALSE(v.irq_asserted());
}

TEST_CASE("Pause raises PAI 100 DCLK ticks later") {
    Dvc v;
    run_to_dclk(v, 10);
    write16(v, kFmvSyscmdOffset, zcdi::dvc::vmpeg::kSysCmdPause);
    run_to_dclk(v, 109);
    CHECK((fmv_isr(v) & kIntPai) == 0);
    run_to_dclk(v, 110);
    CHECK((fmv_isr(v) & kIntPai) != 0);
}

TEST_CASE("Continue restarts playback 1000 DCLK ticks later") {
    Dvc v;
    write16(v, kFmvSyscmdOffset, kDecoderOn);
    run_to_dclk(v, 5);
    write16(v, kFmvSyscmdOffset, zcdi::dvc::vmpeg::kSysCmdContinue);
    run_to_dclk(v, 1004);
    CHECK_FALSE(v.fmv_state().fmv_playback_active_);
    run_to_dclk(v, 1005);
    CHECK(v.fmv_state().fmv_playback_active_);
}

TEST_CASE("the FMA starts decoding when its DCLK reaches the first PTS") {
    VmpegFeed feed(/*audio=*/true);
    Dvc& v = feed.device();
    const std::vector<uint8_t> stream = audio_pack(900);  // 10 ms after the SCR: 450 DCLK ticks
    feed.push(stream.data(), stream.size(), /*audio=*/true);
    run_to_dclk(v, 449);
    CHECK(read16(v, at(kFmaRun)) == 0);
    run_to_dclk(v, 450);
    CHECK(read16(v, at(kFmaRun)) == 1);
    CHECK((read16(v, at(kFmaIsr)) & kFmaIntDecoding) != 0);
}

TEST_CASE("bytes written to XFER reach the FMV demuxer as DMA words do") {
    const std::vector<uint8_t> es = {0x00, 0x00, 0x01, 0xb3, 0x16, 0x00, 0xf0, 0x13, 0xff, 0xff};
    const std::vector<uint8_t> by_dma = demux_video(es.data(), es.size());
    REQUIRE_FALSE(by_dma.empty());

    Dvc v;
    write16(v, kFmvSyscmdOffset, kDecoderOn);
    for (std::size_t i = 0; i < es.size(); i += 2) write16(v, at(kFmvXfer), uint16_t(es[i] << 8 | es[i + 1]));
    CHECK(drain_fmv_es_bytes(v) == by_dma);

    // While the DMA is routed to the FMA, XFER bytes are dropped.
    Dvc to_fma;
    write16(to_fma, kFmvSyscmdOffset, kDecoderOn);
    write16(to_fma, kFmaCmdOffset, kFmaCmdStartDma);
    for (std::size_t i = 0; i < es.size(); i += 2) write16(to_fma, at(kFmvXfer), uint16_t(es[i] << 8 | es[i + 1]));
    CHECK(drain_fmv_es_bytes(to_fma).empty());
    CHECK(drain_fma_es_bytes(to_fma).empty());
}

TEST_CASE("the decoders' results read back: audio header, display size, display rate") {
    Dvc v;
    set_audio_frame_decoded(v, 0xfffd1004u);
    CHECK(read16(v, at(kFmaAudioHdrH)) == 0xfffd);
    CHECK(read16(v, at(kFmaAudioHdrL)) == 0x1004);
    set_audio_frame_decoded(v, 0);  // no header: the last one stays
    CHECK(read16(v, at(kFmaAudioHdrL)) == 0x1004);

    set_video_frame_ready(v, 0xfff, 0xfff);  // 11 and 9 bits wide
    CHECK(read16(v, at(kFmvDisplayW)) == 0x7ff);
    CHECK(read16(v, at(kFmvDisplayH)) == 0x1ff);

    v.set_video_standard(VideoStandard::kNtsc);
    CHECK(read16(v, at(kFmvDisplayRate)) == kDisplayRateNtsc);
    v.set_video_standard(VideoStandard::kPal);
    CHECK(read16(v, at(kFmvDisplayRate)) == kDisplayRatePal);
}

}  // TEST_SUITE

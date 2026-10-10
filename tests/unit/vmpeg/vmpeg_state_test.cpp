#include "doctest.h"

#include <cstdint>
#include <vector>

#include "vmpeg/vmpeg.h"
#include "support/state_roundtrip.h"
#include "support/vmpeg_regs.h"

using zcdi::mamecdi::VmpegDevice;
using namespace zcdi_test;

TEST_SUITE("vmpeg") {

TEST_CASE("a restored VMPEG keeps its clocks and pending transfer bytes") {
    VmpegDevice a;
    write16(a, kFmvSyscmdOffset, 0x1000);  // Decoder On: the FMV demuxer runs
    a.tick_cycles(1'000'333);
    // A sequence header first: an elementary stream, kept until drained.
    for (uint16_t w : {0x0000, 0x01b3, 0x1234, 0x5678}) a.feed_dma_word(w);

    const std::vector<uint8_t> state = zcdi_test::save_state(a);
    VmpegDevice b;
    REQUIRE(zcdi_test::load_state(b, state));
    CHECK(zcdi_test::save_state(b) == state);

    for (int i = 0; i < 50; ++i) {
        CAPTURE(i);
        a.tick_cycles(777);
        b.tick_cycles(777);
        CHECK(read32(b, kFmaDclkOffset) == read32(a, kFmaDclkOffset));
    }
    const std::vector<uint8_t> es = a.drain_fmv_es_bytes();
    CHECK(es.size() == 8);
    CHECK(b.drain_fmv_es_bytes() == es);
    CHECK(b.drain_fma_es_bytes() == a.drain_fma_es_bytes());
    CHECK(zcdi_test::save_state(b) == zcdi_test::save_state(a));
}

}  // TEST_SUITE

TEST_SUITE("vmpeg") {

// SYS_STS (0xE0405E): vmpeg.sv:523, mpeg_video.sv fifo_full (level > 28000).
TEST_CASE("SYS_STS reports the video input FIFO full above 28000 bytes") {
    VmpegDevice v;
    CHECK(read16(v, kFmvSysStsOffset) == 0x2000);
    v.set_decoder_input_bytes(28000);
    CHECK(read16(v, kFmvSysStsOffset) == 0x2000);
    v.set_decoder_input_bytes(28001);
    CHECK(read16(v, kFmvSysStsOffset) == 0);
}

TEST_CASE("demuxed bytes not yet handed to the decoder count toward full") {
    VmpegDevice v;
    write16(v, kFmvSyscmdOffset, 0x1000);
    for (uint16_t w : {0x0000, 0x01b3, 0x1234}) v.feed_dma_word(w);
    v.set_decoder_input_bytes(28000);
    CHECK(read16(v, kFmvSysStsOffset) == 0);
}

TEST_CASE("Clear FIFO empties the input FIFO") {
    VmpegDevice v;
    v.set_decoder_input_bytes(30000);
    write16(v, kFmvSyscmdOffset, 0x0100);
    CHECK(read16(v, kFmvSysStsOffset) == 0x2000);
}

// mpeg_demuxer.sv, mpeg_video.sv: PES DTS -> dts_fifo (PICS_IN_FIFO) -> display.
TEST_CASE("the demuxed DTS drives PICS_IN_FIFO and the display start") {
    VmpegDevice v;
    auto timestamp = [](uint8_t prefix, int64_t t) {
        return std::vector<uint8_t>{
            uint8_t(prefix << 4 | ((t >> 30) & 7) << 1 | 1), uint8_t(t >> 22),
            uint8_t(((t >> 15) & 0x7f) << 1 | 1), uint8_t(t >> 7), uint8_t((t & 0x7f) << 1 | 1)};
    };
    constexpr int64_t kDts = 90000;  // 1 s; register bits 21:7 = 703
    std::vector<uint8_t> stream = {0, 0, 1, 0xba, 0x21, 0, 1, 0, 1, 0x80, 0, 1};
    std::vector<uint8_t> payload = {0, 0, 1, 0xb3, 0x16, 0, 0, 1, 0, 0};  // sequence, picture
    std::vector<uint8_t> pts = timestamp(3, kDts + 3600), dts = timestamp(1, kDts);
    uint16_t length = uint16_t(pts.size() + dts.size() + payload.size());
    stream.insert(stream.end(), {0, 0, 1, 0xe0, uint8_t(length >> 8), uint8_t(length)});
    stream.insert(stream.end(), pts.begin(), pts.end());
    stream.insert(stream.end(), dts.begin(), dts.end());
    stream.insert(stream.end(), payload.begin(), payload.end());
    // A second packet without a timestamp carrying the next picture.
    stream.insert(stream.end(), {0, 0, 1, 0xe0, 0, 6, 0x0f, 0, 0, 1, 0, 0});
    write16(v, kFmvSyscmdOffset, 0x1000);  // Decoder On
    for (size_t i = 0; i + 1 < stream.size(); i += 2) v.feed_dma_word(uint16_t(stream[i] << 8 | stream[i + 1]));

    CHECK(read16(v, kFmvDtsOffset) == 703);
    CHECK((read16(v, kFmvGenVdiCmdOffset) & 0x4000) != 0);
    CHECK(read16(v, kFmvPicsInFifoOffset) == 2);

    write16(v, kFmvSysScrOffset, 0x0004);  // SYSSCR: synchronous playback
    CHECK_FALSE(v.video_display_enabled());
    write16(v, kFmvSyscmdOffset, 0x0008);  // Play
    v.tick_cycles(2'000'000);
    CHECK_FALSE(v.video_display_enabled());  // dclk far from the DTS
    write16(v, kFmvGenSyscrOffset, 703);  // GEN_SYSCR = dclk bits 21:6
    CHECK(v.video_display_enabled());

    v.note_picture_displayed();
    v.set_decoder_held_pictures(1);
    CHECK(read16(v, kFmvPicsInFifoOffset) == 0);  // during playback the held picture isn't counted

    write16(v, kFmvSyscmdOffset, 0x0100);  // Clear FIFO
    CHECK(read16(v, kFmvPicsInFifoOffset) == 0);
    CHECK(read16(v, kFmvDtsOffset) == 0);  // the demuxer reset clears the DTS (mpeg_demuxer.sv:62)
    CHECK_FALSE(v.video_display_enabled());
}

}  // TEST_SUITE

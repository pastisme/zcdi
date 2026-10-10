// CDi_MiSTer vmpeg.sv fmv_reset_persistent_storage, frameplayer.sv:44.
#include "doctest.h"

#include <cstdint>

#include "vmpeg/vmpeg.h"
#include "support/vmpeg_regs.h"

using zcdi::mamecdi::VmpegDevice;
using namespace zcdi_test;

TEST_SUITE("vmpeg") {

TEST_CASE("Decoder Off is reported once") {
    VmpegDevice v;
    CHECK_FALSE(v.consume_decoder_off());
    write16(v, kFmvSyscmdOffset, 0x2000);
    CHECK(v.consume_decoder_off());
    CHECK_FALSE(v.consume_decoder_off());
}

TEST_CASE("Decoder On and Clear FIFO don't report Decoder Off") {
    VmpegDevice v;
    write16(v, kFmvSyscmdOffset, 0x1000);
    write16(v, kFmvSyscmdOffset, 0x0100);
    CHECK_FALSE(v.consume_decoder_off());
}

// mpeg_video.sv:385/1006 and vmpeg.sv:244: with the decoder off the input
// FIFO and demuxer are held in reset, so bytes fed then are dropped.
TEST_CASE("Decoder Off drops pending and later FMV bytes") {
    VmpegDevice v;
    write16(v, kFmvSyscmdOffset, 0x1000);
    for (uint16_t w : {0x0000, 0x01b3, 0x1234}) v.feed_dma_word(w);
    write16(v, kFmvSyscmdOffset, 0x2000);
    CHECK(v.drain_fmv_es_bytes().empty());
    v.feed_dma_word(0x0000);
    v.feed_dma_word(0x01b3);
    CHECK(v.drain_fmv_es_bytes().empty());
    write16(v, kFmvSyscmdOffset, 0x1000);
    v.feed_dma_word(0x0000);
    v.feed_dma_word(0x01b3);
    CHECK(v.drain_fmv_es_bytes().size() == 4);
}

// vmpeg.sv:719: CSU is assigned from the pending flag at every decoded frame.
TEST_CASE("an unread stream-change interrupt clears at the next audio frame") {
    auto csu_after = [](int frames) {
        VmpegDevice v;
        write16(v, kFmaStreamWOffset, 1);
        for (int i = 0; i < frames; ++i) v.set_audio_frame_decoded(0);
        return (v.read_byte(kFmaIsrOffset + 1) & 0x02) != 0;
    };
    CHECK(csu_after(1));
    CHECK_FALSE(csu_after(2));
}

}  // TEST_SUITE

TEST_SUITE("vmpeg") {

// sw/mpeg_audio/main.c: UNF follows once the decoded audio has played out,
// not a fixed time after the last decode.
TEST_CASE("FMA underflow fires the field the decoded audio runs out") {
    constexpr int32_t kField = 882;           // 44.1 kHz at 50 Hz
    VmpegDevice v;
    auto unf = [&] { return (v.read_byte(kFmaIsrOffset + 1) & 0x08) != 0; };
    v.set_audio_decoding_started();
    v.audio_field(3 * kField, kField);  // three fields of audio at once
    v.audio_field(0, kField);
    v.audio_field(0, kField);
    CHECK_FALSE(unf());
    v.audio_field(0, kField);  // the last of it plays
    CHECK(unf());
    CHECK_FALSE(v.audio_dsp_enabled());
    // With the decoder held (waiting for its PTS) nothing fires.
    v.audio_field(0, kField);
    CHECK_FALSE(unf());
}

}  // TEST_SUITE

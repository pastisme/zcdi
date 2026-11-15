// Decoder Off, CSU and FMA underflow.
#include "doctest.h"

#include <cstdint>

#include "dvc/dvc.h"
#include "support/dvc_private.h"
#include "support/vmpeg_regs.h"

using zcdi::dvc::Dvc;
using namespace zcdi_test;

TEST_SUITE("vmpeg") {

TEST_CASE("Decoder Off is reported once") {
    Dvc v;
    CHECK_FALSE(consume_decoder_off(v));
    write16(v, kFmvSyscmdOffset, 0x2000);
    CHECK(consume_decoder_off(v));
    CHECK_FALSE(consume_decoder_off(v));
}

TEST_CASE("Decoder On and Clear FIFO don't report Decoder Off") {
    Dvc v;
    write16(v, kFmvSyscmdOffset, 0x1000);
    write16(v, kFmvSyscmdOffset, 0x0100);
    CHECK_FALSE(consume_decoder_off(v));
}

// With the decoder off the input
// FIFO and demuxer are held in reset, so bytes fed then are dropped.
TEST_CASE("Decoder Off drops pending and later FMV bytes") {
    Dvc v;
    write16(v, kFmvSyscmdOffset, 0x1000);
    for (uint16_t w : {0x0000, 0x01b3, 0x1234}) v.feed_dma_word(w);
    write16(v, kFmvSyscmdOffset, 0x2000);
    CHECK(drain_fmv_es_bytes(v).empty());
    v.feed_dma_word(0x0000);
    v.feed_dma_word(0x01b3);
    CHECK(drain_fmv_es_bytes(v).empty());
    write16(v, kFmvSyscmdOffset, 0x1000);
    v.feed_dma_word(0x0000);
    v.feed_dma_word(0x01b3);
    CHECK(drain_fmv_es_bytes(v).size() == 4);
}

// CSU is assigned from the pending flag at every decoded frame.
TEST_CASE("an unread stream-change interrupt clears at the next audio frame") {
    auto csu_after = [](int frames) {
        Dvc v;
        write16(v, kFmaStreamWOffset, 1);
        for (int i = 0; i < frames; ++i) set_audio_frame_decoded(v, 0);
        return (v.read_byte(kFmaIsrOffset + 1) & 0x02) != 0;
    };
    CHECK(csu_after(1));
    CHECK_FALSE(csu_after(2));
}

}  // TEST_SUITE

TEST_SUITE("vmpeg") {

// UNF follows once the decoded audio has played out,
// not a fixed time after the last decode.
TEST_CASE("FMA underflow fires the field the decoded audio runs out") {
    constexpr int32_t kField = 882;           // 44.1 kHz at 50 Hz
    Dvc v;
    auto unf = [&] { return (v.read_byte(kFmaIsrOffset + 1) & 0x08) != 0; };
    set_audio_decoding_started(v);
    audio_field(v, 3 * kField, kField);  // three fields of audio at once
    audio_field(v, 0, kField);
    audio_field(v, 0, kField);
    CHECK_FALSE(unf());
    audio_field(v, 0, kField);  // the last of it plays
    CHECK(unf());
    CHECK_FALSE(audio_dsp_enabled(v));
    // With the decoder held (waiting for its PTS) nothing fires.
    audio_field(v, 0, kField);
    CHECK_FALSE(unf());
}

}  // TEST_SUITE

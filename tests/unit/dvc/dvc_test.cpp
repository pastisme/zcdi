// The cartridge as the board drives it: registers, channel-1 DMA, lines and fields.
#include "doctest.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "dvc/dvc.h"
#include "fixtures/dvc_mp2_fixture.h"
#include "fixtures/dvc_mpeg1_fixture.h"
#include "dvc/dvc_audio.h"
#include "support/dvc_private.h"
#include "support/vmpeg_regs.h"

using zcdi::dvc::Dvc;
using namespace zcdi_test;

namespace {

constexpr int kFieldLines = 312, kFirstActive = 26, kActiveLines = 280;
constexpr int kCyclesPerField = 300000;  // 15 MHz at 50 Hz

void run_field(Dvc& dvc) {
    dvc.tick_cycles(kCyclesPerField);
    for (int line = 0; line < kFieldLines; ++line) dvc.end_of_line(line, kFirstActive, kActiveLines);
    (void)dvc.tick_field(50.0);
}

// The MP2 fixture over and over, past the 64 sectors after which audio
// without a pack header is taken as an elementary stream.
std::vector<uint8_t> mp2_stream() {
    std::vector<uint8_t> stream;
    while (stream.size() < 65 * 2324)
        stream.insert(stream.end(), kDvcMp2FixtureBytes, kDvcMp2FixtureBytes + kDvcMp2FixtureSize);
    stream.resize(stream.size() & ~std::size_t(1));
    return stream;
}

void feed_mp2(Dvc& dvc) {
    const std::vector<uint8_t> stream = mp2_stream();
    write16(dvc, kFmaCmdOffset, zcdi::dvc::vmpeg::kFmaCmdStartDma);
    for (std::size_t i = 0; i < stream.size(); i += 2) dvc.feed_dma_word(uint16_t(stream[i] << 8 | stream[i + 1]));
}

// The decoded fixture through the DAC gain matrix (Q0.8; the registers hold 0-255).
std::vector<int16_t> mp2_out(uint8_t l2l, uint8_t l2r, uint8_t r2l, uint8_t r2r) {
    Dvc dvc;
    dvc.set_cartridge_present(true);
    feed_mp2(dvc);
    fma(dvc).mpeg_volume_l2l_ = l2l;
    fma(dvc).mpeg_volume_l2r_ = l2r;
    fma(dvc).mpeg_volume_r2l_ = r2l;
    fma(dvc).mpeg_volume_r2r_ = r2r;
    return dvc.drain_audio();
}

}  // namespace

TEST_SUITE("dvc") {

TEST_CASE("a stream sent by DMA is decoded, shown with PIC, and dropped by Decoder Off") {
    Dvc dvc;
    dvc.set_cartridge_present(true);
    write16(dvc, kFmvSyscmdOffset, 0x1000);  // Decoder On
    write16(dvc, kFmvSyscmdOffset, 0x8000);  // start DMA, to video
    REQUIRE(dvc.dma_active());
    uint32_t words = 0;
    for (size_t i = 0; i + 1 < kDvcMpeg1FixtureSize; i += 2, ++words)
        dvc.feed_dma_word(uint16_t(kDvcMpeg1FixtureBytes[i] << 8 | kDvcMpeg1FixtureBytes[i + 1]));
    dvc.complete_dma(words);
    CHECK_FALSE(dvc.dma_active());

    write16(dvc, kFmvSyscmdOffset, 0x0008);  // Play
    for (int field = 0; field < 50 && !dvc.has_frame(); ++field) run_field(dvc);
    REQUIRE(dvc.has_frame());
    CHECK(dvc.width() == kDvcMpeg1Width);
    CHECK(dvc.height() == kDvcMpeg1Height);
    // PIC comes with the picture's first line, in the next field.
    CHECK((read16(dvc, kFmvIsrOffset) & zcdi::dvc::vmpeg::kIntPic) == 0);
    for (int line = 0; line < kFirstActive; ++line) dvc.end_of_line(line, kFirstActive, kActiveLines);
    CHECK((read16(dvc, kFmvIsrOffset) & zcdi::dvc::vmpeg::kIntPic) != 0);

    write16(dvc, kFmvSyscmdOffset, 0x2000);  // Decoder Off
    CHECK(dvc.tick_field(50.0));
    CHECK_FALSE(dvc.has_frame());
    CHECK_FALSE(dvc.tick_field(50.0));
}

TEST_CASE("decoded MPEG audio is muted until the DAC gains are set, then mixed through them") {
    zcdi::dvc::DvcAudio reference;
    reference.feed(mp2_stream());
    const std::vector<int16_t> pcm = reference.decode_all();
    REQUIRE_FALSE(pcm.empty());

    Dvc dvc;
    dvc.set_cartridge_present(true);
    feed_mp2(dvc);
    const std::vector<int16_t> muted = dvc.drain_audio();
    REQUIRE(muted.size() == pcm.size());
    CHECK(std::all_of(muted.begin(), muted.end(), [](int16_t s) { return s == 0; }));
    CHECK(read16(dvc, 2 * zcdi::dvc::vmpeg::kFmaRun) == 1);  // decoding started with the first frame
    CHECK(read32(dvc, 2 * zcdi::dvc::vmpeg::kFmaAudioHdrH) == reference.last_frame_header());

    const std::vector<int16_t> half = mp2_out(128, 0, 0, 128);
    const std::vector<int16_t> swapped = mp2_out(0, 128, 128, 0);
    const std::vector<int16_t> mixed = mp2_out(255, 255, 255, 255);
    REQUIRE(half.size() == pcm.size());
    REQUIRE(swapped.size() == pcm.size());
    REQUIRE(mixed.size() == pcm.size());
    bool stereo = false;
    std::size_t wrong_half = 0, wrong_swapped = 0, wrong_mixed = 0;
    for (std::size_t i = 0; i + 1 < pcm.size(); i += 2) {
        const int l = pcm[i], r = pcm[i + 1];
        stereo = stereo || l != r;
        wrong_half += half[i] != (l * 128) >> 8 || half[i + 1] != (r * 128) >> 8;
        wrong_swapped += swapped[i] != (r * 128) >> 8 || swapped[i + 1] != (l * 128) >> 8;
        const int sum = std::clamp((l * 255 + r * 255) >> 8, -32768, 32767);
        wrong_mixed += mixed[i] != sum || mixed[i + 1] != sum;
    }
    CHECK(wrong_half == 0);
    CHECK(wrong_swapped == 0);
    CHECK(wrong_mixed == 0);
    CHECK(stereo);  // the swap was tested on different channels
}

TEST_CASE("FMA Stop ends decoding and empties the audio input") {
    Dvc dvc;
    dvc.set_cartridge_present(true);
    feed_mp2(dvc);
    REQUIRE_FALSE(dvc.drain_audio().empty());
    feed_mp2(dvc);
    write16(dvc, kFmaCmdOffset, zcdi::dvc::vmpeg::kFmaCmdStop);
    CHECK(read16(dvc, 2 * zcdi::dvc::vmpeg::kFmaRun) == 0);
    CHECK(read16(dvc, 2 * zcdi::dvc::vmpeg::kFmaAudioHdrH) == 0);
    CHECK(dvc.drain_audio().empty());
}

}  // TEST_SUITE

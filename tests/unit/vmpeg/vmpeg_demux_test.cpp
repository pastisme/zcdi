// The FMV and FMA demuxers hand the decoders only PES payload (vmpeg.sv:89/150,
// mpeg_demuxer.sv mpeg_packet_body) and raise program end themselves.
#include "doctest.h"

#include <cstdint>
#include <vector>

#include "fixtures/dvc_clear_fifo_fixture.h"
#include "fixtures/dvc_mp2_fixture.h"
#include "support/vmpeg_feed.h"
#include "support/vmpeg_regs.h"

using zcdi_test::VmpegFeed;

namespace {
constexpr uint16_t kFmvEii = 1u << 10;

// pack header, then one PES packet of `stream` with a PTS and `payload`.
std::vector<uint8_t> pack_and_pes(uint8_t stream, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out = {0x00, 0x00, 0x01, 0xba, 0x21, 0x00, 0x01, 0x00, 0x01, 0x80, 0x00, 0x01};
    const size_t length = 5 + payload.size();
    out.insert(out.end(), {0x00, 0x00, 0x01, stream, uint8_t(length >> 8), uint8_t(length)});
    out.insert(out.end(), {0x21, 0x00, 0x01, 0x00, 0x01});  // PTS
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}
}  // namespace

TEST_SUITE("vmpeg") {

TEST_CASE("the FMV demuxer passes an elementary stream through whole") {
    const std::vector<uint8_t> es = zcdi_test::demux_video(zcdi_test::kClipAEs, sizeof(zcdi_test::kClipAEs));
    REQUIRE(es.size() >= sizeof(zcdi_test::kClipAEs));
    CHECK(std::vector<uint8_t>(es.begin(), es.begin() + sizeof(zcdi_test::kClipAEs)) ==
          std::vector<uint8_t>(zcdi_test::kClipAEs, zcdi_test::kClipAEs + sizeof(zcdi_test::kClipAEs)));
}

TEST_CASE("the FMV demuxer keeps only the selected video stream's payload") {
    const std::vector<uint8_t> payload = {0x00, 0x00, 0x01, 0xb3, 0x11, 0x22};
    std::vector<uint8_t> stream = pack_and_pes(0xe1, {0xaa, 0xbb});  // another stream: dropped
    const std::vector<uint8_t> wanted = pack_and_pes(0xe0, payload);
    stream.insert(stream.end(), wanted.begin(), wanted.end());
    VmpegFeed feed;
    CHECK(feed.push(stream.data(), stream.size()) == payload);
}

TEST_CASE("program end on the FMV raises EII once, at the demuxer") {
    std::vector<uint8_t> stream = pack_and_pes(0xe0, {0x00, 0x00, 0x01, 0xb3});
    stream.insert(stream.end(), {0x00, 0x00, 0x01, 0xb9});
    VmpegFeed feed;
    feed.push(stream.data(), stream.size());
    feed.flush();
    CHECK((feed.device().fmv_isr() & kFmvEii) != 0);
    CHECK(feed.device().consume_fmv_program_end());
    CHECK_FALSE(feed.device().consume_fmv_program_end());
}

TEST_CASE("the FMA demuxer keeps the audio payload and reports program end") {
    const std::vector<uint8_t> payload = {0xff, 0xfd, 0x10, 0x04, 0x55};
    std::vector<uint8_t> stream = pack_and_pes(0xc0, payload);
    stream.insert(stream.end(), {0x00, 0x00, 0x01, 0xb9});
    VmpegFeed feed(/*audio=*/true);
    CHECK(feed.push(stream.data(), stream.size(), /*audio=*/true) == payload);
    // FMA_ISR bit 0, ISO end (vmpeg.sv:736-742).
    CHECK((zcdi_test::read16(feed.device(), zcdi_test::kFmaIsrOffset) & 1) != 0);
}

TEST_CASE("audio without a pack header is an elementary stream after 64 sectors") {
    VmpegFeed feed(/*audio=*/true);
    std::vector<uint8_t> out;
    size_t fed = 0;
    while (fed < 64 * 2324) {
        std::vector<uint8_t> got =
            feed.push(zcdi_test::kDvcMp2FixtureBytes, zcdi_test::kDvcMp2FixtureSize, /*audio=*/true);
        if (fed + zcdi_test::kDvcMp2FixtureSize < 64 * 2324) CHECK(got.empty());
        out.insert(out.end(), got.begin(), got.end());
        fed += zcdi_test::kDvcMp2FixtureSize;
    }
    CHECK(out.size() == fed - fed % 2);  // all of it, but an odd last byte still waits
    CHECK(std::vector<uint8_t>(out.begin(), out.begin() + 4) ==
          std::vector<uint8_t>(zcdi_test::kDvcMp2FixtureBytes, zcdi_test::kDvcMp2FixtureBytes + 4));
}

}  // TEST_SUITE

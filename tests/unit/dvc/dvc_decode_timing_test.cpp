#include "doctest.h"

#include <cstdint>
#include <vector>

#include "dvc/dvc_video.h"
#include "fixtures/dvc_mpeg1_fixture.h"

using zcdi::dvc::DvcVideo;
using namespace zcdi_test;

namespace {
// The whole fixture (7 small I pictures, a sequence header each), arrived at `time`.
void feed_all_at(DvcVideo& v, uint32_t time) {
    v.feed(std::vector<uint8_t>(kDvcMpeg1FixtureBytes, kDvcMpeg1FixtureBytes + kDvcMpeg1FixtureSize), {{0, time}});
}
// Each fixture picture is well under 1 KiB.
constexpr uint32_t kPictureMax = DvcVideo::kDecodeBaseTicks + DvcVideo::kDecodeTicksPerKiB;
}  // namespace

TEST_SUITE("dvc") {

TEST_CASE("a timed picture is decoded only after its decode time") {
    DvcVideo v;
    feed_all_at(v, 0);
    CHECK_FALSE(v.decode_frame(DvcVideo::kDecodeBaseTicks - 1));
    CHECK_FALSE(v.consume_picture_decoded());
    CHECK(v.decoder_held_pictures() == 0);

    // Decoded; the next sequence header pushes it out.
    CHECK(v.decode_frame(kPictureMax));
    CHECK(v.consume_picture_decoded());
    CHECK(v.queue_depth() == 1);
    // The next picture starts when the first is done.
    CHECK_FALSE(v.decode_frame(2 * DvcVideo::kDecodeBaseTicks - 1));
    CHECK(v.decode_frame(2 * kPictureMax));
    CHECK(v.queue_depth() == 2);
}

TEST_CASE("decoding starts no earlier than the data arrives") {
    DvcVideo v;
    feed_all_at(v, 5000);
    CHECK_FALSE(v.decode_frame(5000 + DvcVideo::kDecodeBaseTicks - 1));
    CHECK_FALSE(v.consume_picture_decoded());
    v.decode_frame(5000 + kPictureMax);
    CHECK(v.consume_picture_decoded());
}

TEST_CASE("a decoded picture waits for room in the output FIFO, then goes at the latch") {
    DvcVideo v;
    feed_all_at(v, 0);
    v.decode_frame(100000);
    // Three queued; the fourth is decoded and waits to be pushed.
    CHECK(v.queue_depth() == DvcVideo::kOutputFifoPictures);
    CHECK(v.decoder_held_pictures() == 1);
    REQUIRE(v.present_next_frame());
    CHECK(v.queue_depth() == DvcVideo::kOutputFifoPictures - 1);
    v.resume_push(100000);
    CHECK(v.queue_depth() == DvcVideo::kOutputFifoPictures);
    CHECK(v.decoder_held_pictures() == 0);
}

}  // TEST_SUITE

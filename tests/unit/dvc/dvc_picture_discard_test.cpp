// Discarded pictures leave the DTS FIFO.
#include "doctest.h"

#include <cstdint>
#include <vector>

#include "dvc/dvc_video.h"
#include "fixtures/dvc_clear_fifo_fixture.h"
#include "support/vmpeg_feed.h"
#include "support/vmpeg_regs.h"

using namespace zcdi::dvc;
using namespace zcdi_test;

namespace {
constexpr size_t kSecondGop = 976;  // clip A ES: SEQ, then I, open-GOP Bs, P, Bs
constexpr uint8_t kSequenceEnd[] = {0x00, 0x00, 0x01, 0xb7};

size_t dts_entries(VmpegFeed& feed) { return feed.device().state().fmv_dts_fifo_.size(); }

// Decodes everything, showing pictures as the queue fills. Returns the pictures discarded.
int decode_all(DvcVideo& v, std::vector<uint8_t>* shown_status = nullptr) {
    int discarded = 0;
    for (;;) {
        while (v.decode_frame()) {
        }
        discarded += v.consume_discarded_pictures();
        if (!v.present_next_frame()) return discarded;
        if (shown_status) shown_status->push_back(v.presented_video_status());
    }
}
}  // namespace

TEST_SUITE("dvc") {

TEST_CASE("pictures shown before the first I picture are discarded and counted") {
    DvcVideo v;
    v.feed(std::vector<uint8_t>(kClipAEs + kSecondGop, kClipAEs + sizeof(kClipAEs)));
    std::vector<uint8_t> shown;
    CHECK(decode_all(v, &shown) == 2);  // the two open-GOP Bs
    CHECK(v.consume_discarded_pictures() == 0);
    REQUIRE(!shown.empty());
    CHECK((shown.front() & 3) == 1);  // an I
}

TEST_CASE("pictures after an I picture are never discarded") {
    DvcVideo v;
    v.feed(std::vector<uint8_t>(kClipAEs, kClipAEs + sizeof(kClipAEs)));
    CHECK(decode_all(v) == 0);
}

TEST_CASE("Clear FIFO forgets uncounted discards") {
    DvcVideo v;
    v.feed(std::vector<uint8_t>(kClipAEs + kSecondGop, kClipAEs + sizeof(kClipAEs)));
    while (v.decode_frame()) {
    }
    v.clear_fifo();
    CHECK(v.consume_discarded_pictures() == 0);
}

TEST_CASE("a discarded picture's DTS leaves the dts_fifo") {
    VmpegFeed feed;
    feed.push(kClipAEs, sizeof(kClipAEs));
    REQUIRE(dts_entries(feed) == 12);
    feed.device().note_picture_discarded();
    CHECK(dts_entries(feed) == 11);
}

}  // TEST_SUITE

TEST_SUITE("vmpeg") {

TEST_CASE("picture start codes before a sequence header take no dts_fifo entry") {
    VmpegFeed feed;
    // A program stream joined mid-GOP: 2 pictures before the next SEQ, then I and B.
    feed.push(kClipAPs + kClipAPsCutOffset, sizeof(kClipAPs) - kClipAPsCutOffset);
    CHECK(dts_entries(feed) == 2);
}

TEST_CASE("after a sequence end pictures wait for the next sequence header") {
    VmpegFeed feed;
    feed.push(kClipAEs, kSecondGop);  // SEQ I P B B
    REQUIRE(dts_entries(feed) == 4);
    feed.push(kSequenceEnd, sizeof(kSequenceEnd));
    feed.push(kClipAEs + 20, kSecondGop - 20);  // I P B B, no header
    CHECK(dts_entries(feed) == 4);
    feed.push(kClipAEs, kSecondGop);
    CHECK(dts_entries(feed) == 8);
}

TEST_CASE("Decoder Off forgets the sequence header and Clear FIFO keeps it") {
    VmpegFeed feed;
    feed.push(kClipAEs, kSecondGop);
    SUBCASE("Clear FIFO") {
        write16(feed.device(), kFmvSyscmdOffset, 0x0100);
        feed.push(kClipAEs + 20, kSecondGop - 20);
        CHECK(dts_entries(feed) == 4);
    }
    SUBCASE("Decoder Off then On") {
        write16(feed.device(), kFmvSyscmdOffset, 0x2000);
        write16(feed.device(), kFmvSyscmdOffset, 0x1000);
        feed.push(kClipAEs + 20, kSecondGop - 20);
        CHECK(dts_entries(feed) == 0);
        feed.push(kClipAEs, kSecondGop);
        CHECK(dts_entries(feed) == 4);
    }
}

}  // TEST_SUITE

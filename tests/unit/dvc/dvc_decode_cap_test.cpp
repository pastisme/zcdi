#include "doctest.h"

#include <cstdint>
#include <vector>

#include "dvc/dvc_video.h"
#include "fixtures/dvc_clear_fifo_fixture.h"
#include "support/vmpeg_feed.h"

using zcdi::dvc::DvcVideo;

namespace {

struct Shown {
    uint32_t timecode;
    uint8_t video_status;  // temporal_reference << 2 | type: which picture
    std::vector<uint8_t> rgba;
    bool operator==(const Shown&) const = default;
};

// Clip A (12 pictures) three times over: more than the cap.
void feed_clip_a_three_times(DvcVideo& v, bool es) {
    const zcdi_test::Clip a = zcdi_test::clip_a(es);
    for (int i = 0; i < 3; ++i) v.feed(zcdi_test::demux_video(a.data, a.size));
}

// Bus's field order: decode every field, show on every `present_every`th.
std::vector<Shown> run(bool es, int present_every, size_t* max_depth) {
    DvcVideo v;
    feed_clip_a_three_times(v, es);
    std::vector<Shown> out;
    *max_depth = 0;
    for (int field = 0; field < 200; ++field) {
        v.decode_frame();
        if (v.queue_depth() > *max_depth) *max_depth = v.queue_depth();
        if (field % present_every == present_every - 1 && v.present_next_frame()) {
            out.push_back({v.presented_timecode(), v.presented_video_status(), zcdi_test::frame_bytes(v)});
        }
    }
    return out;
}

}  // namespace

TEST_SUITE("dvc") {

TEST_CASE("decoding pauses at kMaxDecodedPictures queued pictures") {
    for (bool es : {true, false}) {
        CAPTURE(es);
        DvcVideo v;
        feed_clip_a_three_times(v, es);
        for (int field = 0; field < 60; ++field) v.decode_frame();
        CHECK(v.queue_depth() == DvcVideo::kMaxDecodedPictures);
        CHECK_FALSE(v.decode_frame());
        // Showing one frees a slot, and decoding resumes.
        CHECK(v.present_next_frame());
        CHECK(v.decode_frame());
        CHECK(v.queue_depth() == DvcVideo::kMaxDecodedPictures);
    }
}

TEST_CASE("the decoded-picture limit doesn't change what is shown") {
    for (bool es : {true, false}) {
        CAPTURE(es);
        size_t depth_free = 0, depth_backlog = 0;
        const std::vector<Shown> free_running = run(es, 1, &depth_free);
        const std::vector<Shown> backlogged = run(es, 4, &depth_backlog);
        CHECK(depth_free <= 2);
        CHECK(depth_backlog == DvcVideo::kMaxDecodedPictures);
        // Most of the 36 pictures: the last stay buffered without an end code.
        REQUIRE(free_running.size() >= 30);
        REQUIRE(backlogged.size() == free_running.size());
        for (size_t i = 0; i < free_running.size(); ++i) {
            CAPTURE(i);
            CHECK(backlogged[i] == free_running[i]);
        }
    }
}

}  // TEST_SUITE

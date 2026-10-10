#include "doctest.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "dvc/dvc_audio.h"
#include "dvc/dvc_video.h"
#include "fixtures/dvc_clear_fifo_fixture.h"
#include "fixtures/dvc_mp2_fixture.h"
#include "support/state_roundtrip.h"
#include "support/vmpeg_feed.h"

using zcdi::dvc::DvcAudio;
using zcdi::dvc::DvcVideo;

namespace {

constexpr size_t kChunk = 200;  // bytes fed per field: pictures straddle feeds

// DvcVideo's input: the clip's elementary stream, as VMPEG demuxes it.
struct Feeder {
    std::vector<uint8_t> es;
    size_t pos = 0;

    Feeder(const uint8_t* data, size_t size) : es(zcdi_test::demux_video(data, size)) {}

    std::vector<uint8_t> next() {
        size_t n = std::min(kChunk, es.size() - pos);
        std::vector<uint8_t> out(es.begin() + std::ptrdiff_t(pos), es.begin() + std::ptrdiff_t(pos + n));
        pos += n;
        return out;
    }
};

// One field the way Bus drives it: feed, decode, present.
void field(DvcVideo& v, Feeder& f) {
    std::vector<uint8_t> chunk = f.next();
    if (!chunk.empty()) v.feed(chunk);
    v.decode_frame();
    v.present_next_frame();
}

void video_roundtrip(bool es, int save_field) {
    const zcdi_test::Clip clip = zcdi_test::clip_a(es);
    Feeder fa(clip.data, clip.size);
    DvcVideo a;
    for (int i = 0; i <= save_field; ++i) field(a, fa);

    const std::vector<uint8_t> state = zcdi_test::save_state(a);
    CHECK(zcdi_test::measure_state(a) == state.size());
    DvcVideo b;
    REQUIRE(zcdi_test::load_state(b, state));
    CHECK(zcdi_test::first_difference(zcdi_test::save_state(b), state) == -1);

    Feeder fb = fa;
    int shown = 0;
    for (int i = 0; i < 60; ++i) {
        CAPTURE(i);
        field(a, fa);
        field(b, fb);
        REQUIRE(b.has_frame() == a.has_frame());
        CHECK(b.presented_timecode() == a.presented_timecode());
        CHECK(zcdi_test::first_difference(zcdi_test::frame_bytes(b), zcdi_test::frame_bytes(a)) == -1);
        if (a.has_frame()) ++shown;
    }
    CHECK(shown > 0);
    CHECK(zcdi_test::first_difference(zcdi_test::save_state(b), zcdi_test::save_state(a)) == -1);
}

}  // namespace

TEST_SUITE("dvc") {

TEST_CASE("a restored DvcVideo decodes and presents the same pictures") {
    for (bool es : {true, false}) {
        for (int save_field : {0, 2, 5, 9, 14}) {
            CAPTURE(es);
            CAPTURE(save_field);
            video_roundtrip(es, save_field);
        }
    }
}

TEST_CASE("a truncated DvcVideo state is rejected") {
    Feeder f(zcdi_test::kClipAPs, sizeof(zcdi_test::kClipAPs));
    DvcVideo a;
    for (int i = 0; i < 6; ++i) field(a, f);
    std::vector<uint8_t> state = zcdi_test::save_state(a);
    for (size_t cut : {size_t(8), state.size() / 2, state.size() - 1}) {
        CAPTURE(cut);
        std::vector<uint8_t> part(state.begin(), state.begin() + cut);
        DvcVideo b;
        CHECK(!zcdi_test::load_state(b, part));
    }
}

TEST_CASE("a restored DvcAudio decodes the same samples") {
    const std::vector<uint8_t> tone(zcdi_test::kDvcMp2FixtureBytes,
                                    zcdi_test::kDvcMp2FixtureBytes + zcdi_test::kDvcMp2FixtureSize);
    for (int save_after : {3, 40, 45}) {
        CAPTURE(save_after);
        DvcAudio a;
        for (int i = 0; i < save_after; ++i) {
            a.feed(tone);
            a.decode_all();
        }
        const std::vector<uint8_t> state = zcdi_test::save_state(a);
        DvcAudio b;
        REQUIRE(zcdi_test::load_state(b, state));
        CHECK(zcdi_test::first_difference(zcdi_test::save_state(b), state) == -1);
        size_t samples = 0;
        for (int i = 0; i < 45; ++i) {
            CAPTURE(i);
            a.feed(tone);
            b.feed(tone);
            const std::vector<int16_t> sa = a.decode_all();
            CHECK(b.decode_all() == sa);
            CHECK(b.last_frame_header() == a.last_frame_header());
            samples += sa.size();
        }
        CHECK(samples > 0);
    }
}

}  // TEST_SUITE

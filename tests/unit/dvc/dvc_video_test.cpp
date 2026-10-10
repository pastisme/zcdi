#include "doctest.h"

#include <algorithm>
#include <vector>

#include "dvc/dvc_video.h"
#include "fixtures/dvc_mpeg1_fixture.h"
#include "fixtures/dvc_clear_fifo_fixture.h"
#include "support/vmpeg_feed.h"

using namespace zcdi::dvc;
using namespace zcdi_test;

namespace {
void feed_all(DvcVideo& v) {
    v.feed(std::vector<uint8_t>(kDvcMpeg1FixtureBytes, kDvcMpeg1FixtureBytes + kDvcMpeg1FixtureSize));
}
void feed_stall_safe_prefix(DvcVideo& v) {
    v.feed(std::vector<uint8_t>(kDvcMpeg1FixtureBytes, kDvcMpeg1FixtureBytes + kDvcMpeg1StallSafePrefixByteCount));
}
void feed_remainder_after_stall_prefix(DvcVideo& v) {
    v.feed(std::vector<uint8_t>(kDvcMpeg1FixtureBytes + kDvcMpeg1StallSafePrefixByteCount,
                                kDvcMpeg1FixtureBytes + kDvcMpeg1FixtureSize));
}
}  // namespace

TEST_SUITE_BEGIN("dvc");

TEST_CASE("first frame presents when asked") {
    DvcVideo v;
    feed_all(v);

    CHECK(v.decode_frame() == true);
    CHECK(v.has_frame() == false);  // decoded, not yet shown
    CHECK(v.present_next_frame() == true);
    CHECK(v.has_frame() == true);
    CHECK(v.width() == kDvcMpeg1Width);
    CHECK(v.height() == kDvcMpeg1Height);
}

TEST_CASE("queued pictures show in order with no drops") {
    DvcVideo v;
    feed_all(v);

    int decoded_count = 0;
    while (v.decode_frame()) ++decoded_count;
    CHECK(decoded_count == kDvcMpeg1FrameCount);
    CHECK(v.queue_depth() == size_t(kDvcMpeg1FrameCount));

    // One GOP per picture, time codes 00:00:00:k.
    std::vector<uint32_t> timecodes;
    while (v.present_next_frame()) timecodes.push_back(v.presented_timecode());
    CHECK(timecodes.size() == size_t(kDvcMpeg1FrameCount));
    for (size_t i = 1; i < timecodes.size(); ++i) CHECK(timecodes[i] > timecodes[i - 1]);
    CHECK(v.queue_depth() == 0u);
}

TEST_CASE("stall then resume presents immediately") {
    DvcVideo v;
    feed_stall_safe_prefix(v);

    CHECK(v.decode_frame() == true);
    CHECK(v.present_next_frame() == true);

    constexpr int kStallFields = 100;
    for (int i = 0; i < kStallFields; ++i) {
        CHECK(v.decode_frame() == false);
        CHECK(v.present_next_frame() == false);  // queue empty
    }

    feed_remainder_after_stall_prefix(v);
    CHECK(v.decode_frame() == true);
    CHECK(v.present_next_frame() == true);
}

TEST_CASE("unfed instance is inert") {
    DvcVideo v;
    for (int field = 0; field < 20; ++field) {
        CHECK(v.decode_frame() == false);
        CHECK(v.present_next_frame() == false);
    }
    CHECK(v.has_frame() == false);
    CHECK(v.queue_depth() == 0u);
}

// ---- FMV "Clear FIFO" (DvcVideo::clear_fifo) --------------------------------
// vmpeg.sv SYSCMD 0x0100: drops bitstream and queued pictures, keeps the shown frame.
// Both shapes go through VmpegDevice's demuxer, as in Bus.
namespace {
size_t cut_offset(bool es) { return es ? kClipAEsCutOffset : kClipAPsCutOffset; }

struct Shown { uint64_t hash; int w, h; bool operator==(const Shown& o) const { return hash == o.hash && w == o.w && h == o.h; } };

uint64_t frame_hash(const DvcVideo& v) {
    uint64_t h = 1469598103934665603ull;
    for (uint8_t b : frame_bytes(v)) { h ^= b; h *= 1099511628211ull; }
    return h;
}

// One "field" per chunk, in Bus's order: feed -> decode_frame -> present.
void play(DvcVideo& v, VmpegFeed& feed, const uint8_t* data, size_t size, size_t chunk, std::vector<Shown>& shown,
          int drain_fields = 40) {
    for (size_t pos = 0; pos < size; pos += chunk) {
        size_t n = std::min(chunk, size - pos);
        feed.feed(v, data + pos, n);
        v.decode_frame();
        if (v.present_next_frame()) shown.push_back({frame_hash(v), v.width(), v.height()});
    }
    for (int i = 0; i < drain_fields; ++i) {
        v.decode_frame();
        if (v.present_next_frame()) shown.push_back({frame_hash(v), v.width(), v.height()});
    }
}

std::vector<Shown> fresh(const Clip& c, size_t chunk) {
    DvcVideo v;
    VmpegFeed feed;
    std::vector<Shown> shown;
    play(v, feed, c.data, c.size, chunk, shown);
    return shown;
}

// Clip A cut off mid-picture, Clear FIFO, then `next` from its start.
std::vector<Shown> cut_clear_then(bool es, const Clip& next, size_t chunk, bool do_clear = true) {
    DvcVideo v;
    VmpegFeed feed;
    std::vector<Shown> before, after;
    play(v, feed, clip_a(es).data, cut_offset(es), chunk, before);
    if (do_clear) feed.clear_fifo(v);
    play(v, feed, next.data, next.size, chunk, after);
    return after;
}

// Last 4 bytes: 0xB7 sequence end in the ES, 0xB9 program end in the PS.
void end_of_stream_flushes_all_pictures_and_signals(bool es, bool pad) {
    constexpr int kClipAPictures = 12;
    const Clip a = clip_a(es);
    std::vector<uint8_t> stream(a.data, a.data + a.size);
    if (es) {
        stream.insert(stream.end(), {0x00, 0x00, 0x01, 0xb7});
    } else {
        // Video PES (no PTS) with the sequence end, optional padding, program end.
        stream.insert(stream.end(), {0x00, 0x00, 0x01, 0xe0, 0x00, 0x05, 0x0f,
                                     0x00, 0x00, 0x01, 0xb7});
        if (pad) {
            stream.insert(stream.end(), {0x00, 0x00, 0x01, 0xbe, 0x00, 0x10});
            stream.insert(stream.end(), 16, 0xff);
        }
        stream.insert(stream.end(), {0x00, 0x00, 0x01, 0xb9});
    }
    DvcVideo v;
    VmpegFeed feed;
    int shown = 0, seq_end = 0, last_picture = 0;
    bool last_picture_on_final = false;
    for (size_t pos = 0; pos < stream.size() + 64 * 256; pos += 256) {
        if (pos < stream.size()) {
            size_t n = std::min<size_t>(256, stream.size() - pos);
            feed.feed(v, stream.data() + pos, n);
            if (pos + n == stream.size()) feed.finish(v);
        }
        v.decode_frame();
        if (v.present_next_frame()) ++shown;
        seq_end += v.consume_sequence_end();
        if (v.consume_last_picture_shown()) {
            ++last_picture;
            last_picture_on_final = (shown == kClipAPictures);
        }
    }
    CHECK(shown == kClipAPictures);
    CHECK(seq_end == 1);
    CHECK(feed.program_ends() == (es ? 0 : 1));
    CHECK(last_picture == 1);
    CHECK(last_picture_on_final);
    // Without the end codes nothing marks the end: no EOD.
    DvcVideo w;
    VmpegFeed wfeed;
    std::vector<Shown> ignored;
    play(w, wfeed, a.data, a.size, 256, ignored);
    CHECK(!w.consume_last_picture_shown());
}
}  // namespace

// Each clip test runs on an ES and on a PS: different demux paths.

// Changed resolution: the cut-off clip-A picture must be discarded and clip B
// must decode exactly as a fresh decoder decodes it (header seen, new size).
TEST_CASE("clear fifo discards partial picture and reparses new size") {
    for (bool es : {true, false}) {
        CAPTURE(es);
        std::vector<Shown> got = cut_clear_then(es, clip_b(es), 256);
        std::vector<Shown> want = fresh(clip_b(es), 256);
        CHECK(!want.empty());
        CHECK(got == want);
        for (const Shown& s : got) {
            CHECK(s.w == kClipBWidth);
            CHECK(s.h == kClipBHeight);
        }
    }
}

// Same resolution: header matches, decoding resumes as a fresh decoder would.
TEST_CASE("clear fifo resumes on same size header") {
    for (bool es : {true, false}) {
        CAPTURE(es);
        std::vector<Shown> got = cut_clear_then(es, clip_a(es), 256);
        std::vector<Shown> want = fresh(clip_a(es), 256);
        CHECK(!want.empty());
        CHECK(got == want);
    }
}

// Tiny chunks: the post-clear sequence header arrives split across feeds.
TEST_CASE("clear fifo handles incremental header") {
    for (bool es : {true, false}) {
        for (size_t chunk : {1u, 7u, 13u}) {
            CAPTURE(es);
            CAPTURE(chunk);
            std::vector<Shown> got = cut_clear_then(es, clip_b(es), chunk);
            std::vector<Shown> want = fresh(clip_b(es), chunk);
            CHECK(!want.empty());
            CHECK(got == want);
        }
    }
}

// Negative control: proves the tests above exercise the real failure.
TEST_CASE("without clear fifo cut picture corrupts next clip") {
    for (bool es : {true, false}) {
        CAPTURE(es);
        std::vector<Shown> got = cut_clear_then(es, clip_b(es), 256, /*do_clear=*/false);
        CHECK(!(got == fresh(clip_b(es), 256)));
    }
}

// Before the container shape is known, Clear FIFO is a no-op.
TEST_CASE("clear fifo while mode undetermined is noop") {
    for (bool es : {true, false}) {
        CAPTURE(es);
        std::vector<uint8_t> junk(300, 0x5a);
        const Clip a = clip_a(es);
        std::vector<Shown> with_clear, without_clear;
        {
            DvcVideo v;
            VmpegFeed feed;
            feed.feed(v, junk.data(), junk.size());
            feed.clear_fifo(v);
            play(v, feed, a.data, a.size, 256, with_clear);
        }
        {
            DvcVideo v;
            VmpegFeed feed;
            feed.feed(v, junk.data(), junk.size());
            play(v, feed, a.data, a.size, 256, without_clear);
        }
        CHECK(!with_clear.empty());
        CHECK(with_clear == without_clear);
    }
}

// Resuming on clip A's second GOP (SEQ at 976, then I, then open-GOP Bs): the Bs
// take the I as forward reference, so what was decoded before the clear can't show.
TEST_CASE("after clear fifo, open-GOP B pictures don't depend on earlier pictures") {
    constexpr size_t kSecondGop = 976;
    auto resume_after = [](size_t history) {
        DvcVideo v;
        VmpegFeed feed;
        std::vector<Shown> before, after;
        play(v, feed, kClipAEs, history, 256, before);
        REQUIRE(!before.empty());  // an I was shown, so the Bs aren't held back
        feed.clear_fifo(v);
        play(v, feed, kClipAEs + kSecondGop, sizeof(kClipAEs) - kSecondGop, 256, after);
        return after;
    };
    const std::vector<Shown> short_history = resume_after(kSecondGop);
    const std::vector<Shown> long_history = resume_after(2100);
    CHECK(short_history.size() >= 4);
    CHECK(short_history == long_history);
}

// sw/mpeg_video/main.c push_frame(): GOP on the first I shown after temporal
// reference 0, SEQ on the first such I after a sequence header.
TEST_CASE("GOP and SEQ flags mark the first I picture shown in each GOP") {
    DvcVideo v;
    v.feed(std::vector<uint8_t>(kClipAEs, kClipAEs + sizeof(kClipAEs)));
    std::vector<uint8_t> flagged;
    while (v.decode_frame()) {
    }
    while (v.present_next_frame()) {
        CHECK(v.presented_starts_gop() == v.presented_starts_sequence());
        if (v.presented_starts_gop()) flagged.push_back(v.presented_video_status());
    }
    // I tr0 of GOP 1, I tr2 of GOP 2 (its open-GOP Bs come first).
    CHECK(flagged == std::vector<uint8_t>{0 << 2 | 1, 2 << 2 | 1});
}

TEST_CASE("clear fifo drops queue keeps presented frame") {
    DvcVideo v;
    std::vector<uint8_t> all(kClipAEs, kClipAEs + sizeof(kClipAEs));
    v.feed(all);
    for (int i = 0; i < 20 && !v.has_frame(); ++i) { v.decode_frame(); v.present_next_frame(); }
    CHECK(v.has_frame());
    for (int i = 0; i < 4; ++i) v.decode_frame();  // decode ahead without presenting
    CHECK(v.queue_depth() > 0);
    uint64_t on_screen = frame_hash(v);
    v.clear_fifo();
    CHECK(v.queue_depth() == 0);
    CHECK(v.has_frame());
    CHECK(frame_hash(v) == on_screen);
    CHECK(v.decode_frame() == false);  // bitstream gone too
    CHECK(v.present_next_frame() == false);
    CHECK(frame_hash(v) == on_screen);
}

// ---- End of stream -----------------------------------------------------------
TEST_CASE("end of stream flushes all pictures and signals") {
    for (bool es : {true, false}) {
        CAPTURE(es);
        end_of_stream_flushes_all_pictures_and_signals(es, /*pad=*/true);
    }
    {
        INFO("PS, closing packet not followed by 16 bytes");
        end_of_stream_flushes_all_pictures_and_signals(false, /*pad=*/false);
    }
}

TEST_CASE("program stream video follows the stream number") {
    std::vector<uint8_t> renumbered(kClipAPs, kClipAPs + sizeof(kClipAPs));
    int packets = 0;
    for (size_t i = 0; i + 3 < renumbered.size(); ++i) {
        if (renumbered[i] == 0 && renumbered[i + 1] == 0 && renumbered[i + 2] == 1 && renumbered[i + 3] == 0xe0) {
            renumbered[i + 3] = 0xe1;
            ++packets;
        }
    }
    REQUIRE(packets > 0);
    auto shown = [&](int stream) {
        DvcVideo v;
        VmpegFeed feed;
        feed.set_video_stream(stream);
        std::vector<Shown> out;
        play(v, feed, renumbered.data(), renumbered.size(), 256, out);
        return out.size();
    };
    CHECK(shown(0) == 0);
    CHECK(shown(1) > 0);
}

// seconds << 22 | pictures << 16 | hours << 6 | minutes; GOP k is 00:00:00:k.
// The decoder time code follows decoding (mpeg_video.sv event_frame_decoded),
// one reference picture ahead of the picture output for display.
TEST_CASE("pictures carry their GOP time code") {
    DvcVideo v;
    feed_all(v);
    bool decoder_ahead = false;
    for (uint32_t k = 0; k < uint32_t(kDvcMpeg1FrameCount); ++k) {
        REQUIRE(v.decode_frame());
        CHECK(v.last_decoded_timecode() >= k << 16);
        decoder_ahead |= v.last_decoded_timecode() > k << 16;
        REQUIRE(v.present_next_frame());
        CHECK(v.presented_timecode() == k << 16);
    }
    CHECK(decoder_ahead);
}

// mpeg_video.sv: Decoder Off resets the decoder and reset_persistent_storage
// drops has_sequence_header; Clear FIFO keeps it.
TEST_CASE("decoder off forgets the sequence header, clear fifo keeps it") {
    std::vector<uint8_t> all(kClipAEs, kClipAEs + sizeof(kClipAEs));
    const uint8_t kGop[] = {0, 0, 1, 0xb8};
    const auto gop = std::search(all.begin(), all.end(), std::begin(kGop), std::end(kGop));
    REQUIRE(gop != all.end());
    // The first GOP, up to the clip's next sequence header.
    const uint8_t kSeq[] = {0, 0, 1, 0xb3};
    const auto next_seq = std::search(gop, all.end(), std::begin(kSeq), std::end(kSeq));
    const std::vector<uint8_t> without_sequence(gop, next_seq);
    auto pictures_after = [&](bool decoder_off) {
        DvcVideo v;
        v.feed(all);
        for (int i = 0; i < 20 && !v.has_frame(); ++i) { v.decode_frame(); v.present_next_frame(); }
        REQUIRE(v.has_frame());
        v.decode_frame();
        if (decoder_off) {
            v.decoder_off();
            CHECK_FALSE(v.has_frame());
            CHECK(v.queue_depth() == 0);
        } else {
            v.clear_fifo();
        }
        v.feed(without_sequence);
        for (int i = 0; i < 20; ++i) v.decode_frame();
        return v.queue_depth();
    };
    CHECK(pictures_after(/*decoder_off=*/false) > 0);
    CHECK(pictures_after(/*decoder_off=*/true) == 0);
}

TEST_SUITE_END();

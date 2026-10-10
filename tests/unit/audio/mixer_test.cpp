#include "doctest.h"

#include <vector>

#include "audio/mixer.h"

namespace {

constexpr int kFramesPerRender = 100;

std::vector<int16_t> burst(int frames, int16_t value) {
    return std::vector<int16_t>(static_cast<size_t>(frames) * 2, value);
}

}  // namespace

TEST_SUITE("audio") {

TEST_CASE("bursts from both sources overlap") {
    zcdi::Mixer mixer(44100);
    mixer.push_samples(burst(300, 100));
    mixer.push_dvc_samples(burst(300, 50));
    // Start cushion: a queue that just started waits two renders.
    for (int i = 0; i < 2; ++i) {
        const int16_t* out = mixer.render(kFramesPerRender);
        CHECK(out[0] == 0);
    }
    for (int i = 0; i < 3; ++i) {
        const int16_t* out = mixer.render(kFramesPerRender);
        CHECK(out[0] == 150);
        CHECK(out[2 * kFramesPerRender - 1] == 150);
    }
    const int16_t* out = mixer.render(kFramesPerRender);
    CHECK(out[0] == 0);
    CHECK(out[2 * kFramesPerRender - 1] == 0);
}

TEST_CASE("a late second source does not delay the first") {
    zcdi::Mixer mixer(44100);
    mixer.push_samples(burst(1000, 100));
    mixer.render(kFramesPerRender);
    mixer.render(kFramesPerRender);
    const int16_t* out = mixer.render(kFramesPerRender);
    CHECK(out[0] == 100);
    mixer.push_dvc_samples(burst(500, 50));
    // CDIC keeps playing while the DVC queue waits out its own cushion.
    out = mixer.render(kFramesPerRender);
    CHECK(out[0] == 100);
    out = mixer.render(kFramesPerRender);
    CHECK(out[0] == 100);
    out = mixer.render(kFramesPerRender);
    CHECK(out[0] == 150);
}

TEST_CASE("the sum is clamped") {
    zcdi::Mixer mixer(44100);
    mixer.push_samples(burst(10, 30000));
    mixer.push_dvc_samples(burst(10, 30000));
    mixer.render(10);
    mixer.render(10);
    const int16_t* out = mixer.render(10);
    CHECK(out[0] == 32767);
}

TEST_CASE("sustained overproduction is capped") {
    constexpr int kRate = 44100;
    constexpr int kPerRender = kRate / 50;
    zcdi::Mixer mixer(kRate);
    for (int i = 0; i < 500; ++i) {
        mixer.push_dvc_samples(burst(2 * kPerRender, static_cast<int16_t>(i)));
        mixer.render(kPerRender);
    }
    const int16_t* out = mixer.render(kPerRender);
    CHECK(out[0] > 400);  // recent bursts, not ones from seconds ago
    // At most a second of backlog left to play out.
    int backlog_frames = kPerRender;
    while (mixer.render(kPerRender)[0] != 0 && backlog_frames <= 2 * kRate) backlog_frames += kPerRender;
    CHECK(backlog_frames <= kRate);
}

}  // TEST_SUITE

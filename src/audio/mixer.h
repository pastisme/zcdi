#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace zcdi {
// Mixes the machine's two audio streams (Machine::drain_audio) for output.
// All samples are interleaved stereo S16.
class Mixer {
public:
    explicit Mixer(int sample_rate);

    // CDIC audio (AudioStreams::cdic).
    void push_samples(const std::vector<int16_t>& interleaved_stereo);

    // DVC audio (AudioStreams::dvc); queued apart and summed in render().
    void push_dvc_samples(const std::vector<int16_t>& interleaved_stereo);

    // `frames` stereo frames, padded with silence on underrun. Valid until
    // the next call.
    const int16_t* render(int frames);

    template <class T>
    void state_io(T& archive);

private:
    // A (re)started queue waits kStartDelayRenders renders, so a burst
    // doesn't underrun at its boundary.
    struct Source {
        std::vector<int16_t> queue;
        int start_delay = 0;
        bool running = false;
        // Appends, dropping the oldest samples past `cap`.
        void push(const std::vector<int16_t>& samples, std::size_t cap);
        std::size_t take(std::size_t needed);
    };
    static constexpr int kStartDelayRenders = 2;
    // Backlog past which the oldest samples are dropped.
    static constexpr double kMaxQueuedSeconds = 1.0;
    std::size_t queue_cap() const { return static_cast<std::size_t>(kMaxQueuedSeconds * sample_rate_) * 2; }

    int sample_rate_ = 0;
    std::vector<int16_t> buf_;
    Source cdic_;
    Source dvc_;
};
} // namespace zcdi

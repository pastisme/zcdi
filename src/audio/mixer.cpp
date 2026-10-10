#include "audio/mixer.h"

#include "core/state_archive.h"
#include "core/util.h"

#include <algorithm>
#include <cstddef>

namespace zcdi {

Mixer::Mixer(int sample_rate) : sample_rate_(sample_rate) {
    cdic_.queue.reserve(queue_cap());
    dvc_.queue.reserve(queue_cap());
}

template <class T>
void Mixer::state_io(T& archive) {
    archive.begin_section(state_tag("MIXR"));
    archive.io_struct(cdic_);
    archive.io_struct(dvc_);
    archive.end_section();
}
ZCDI_STATE_IO_INSTANTIATE(Mixer);

void Mixer::Source::push(const std::vector<int16_t>& samples, std::size_t cap) {
    queue.insert(queue.end(), samples.begin(), samples.end());
    if (queue.size() > cap) queue.erase(queue.begin(), queue.end() - cap);
}

void Mixer::push_samples(const std::vector<int16_t>& interleaved_stereo) {
    cdic_.push(interleaved_stereo, queue_cap());
}

void Mixer::push_dvc_samples(const std::vector<int16_t>& interleaved_stereo) {
    dvc_.push(interleaved_stereo, queue_cap());
}

std::size_t Mixer::Source::take(std::size_t needed) {
    if (queue.empty()) {
        running = false;
        return 0;
    }
    if (!running) {
        running = true;
        start_delay = kStartDelayRenders;
    }
    if (start_delay > 0) {
        --start_delay;
        return 0;
    }
    return std::min(needed, queue.size());
}

const int16_t* Mixer::render(int frames) {
    std::size_t needed = static_cast<std::size_t>(frames) * 2;
    buf_.assign(needed, 0);

    std::size_t from_cdic = cdic_.take(needed);
    std::size_t from_dvc = dvc_.take(needed);
    for (std::size_t i = 0; i < needed; ++i) {
        int v = (i < from_cdic ? cdic_.queue[i] : 0) + (i < from_dvc ? dvc_.queue[i] : 0);
        buf_[i] = clamp16(v);
    }
    cdic_.queue.erase(cdic_.queue.begin(), cdic_.queue.begin() + from_cdic);
    dvc_.queue.erase(dvc_.queue.begin(), dvc_.queue.begin() + from_dvc);

    return buf_.data();
}

} // namespace zcdi

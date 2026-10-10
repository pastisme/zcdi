#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace zcdi {
namespace dvc {

class DvcAudio {
public:
    DvcAudio();
    ~DvcAudio();

    DvcAudio(const DvcAudio&) = delete;
    DvcAudio& operator=(const DvcAudio&) = delete;

    // MPEG audio elementary-stream bytes (VMPEG demuxes program streams).
    void feed(const std::vector<uint8_t>& data);

    // Back to the just-constructed state.
    void reset();

    // Every complete frame buffered, as interleaved stereo S16 at the stream's rate (not resampled).
    std::vector<int16_t> decode_all();

    // 0xE00000 | the 21 bits after the sync word, as at FMA 0xE03014/16; 0 if none.
    uint32_t last_frame_header() const;

    template <class T>
    void state_io(T& archive);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dvc
} // namespace zcdi

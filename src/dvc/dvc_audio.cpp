#include "dvc/dvc_audio.h"

#include <cstddef>

#define MINIMP3_IMPLEMENTATION
#include "minimp3/minimp3.h"

#include "core/state_archive.h"

namespace zcdi {
namespace dvc {

namespace {

// ISO 11172-3 2.4.2.3, kbit/s.
constexpr int kBitrateLayerI[15] = {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448};
constexpr int kBitrateLayerII[15] = {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384};
constexpr int kSampleRate[3] = {44100, 48000, 32000};

struct FrameHeader {
    int layer = 0; // 1 or 2
    int bitrate_index = 0;
    int samplerate_index = 0;
    size_t size = 0; // whole frame, header included
    uint32_t reg = 0;
};

// MPEG-1 Layer I or II, no free format: what mpeg_audio.sv decodes.
bool parse_header(const uint8_t* p, FrameHeader& h) {
    if (p[0] != 0xff || (p[1] & 0xf8) != 0xf8) return false; // sync, MPEG-1
    const int layer_bits = (p[1] >> 1) & 3;
    if (layer_bits != 3 && layer_bits != 2) return false;
    h.layer = layer_bits == 3 ? 1 : 2;
    h.bitrate_index = p[2] >> 4;
    h.samplerate_index = (p[2] >> 2) & 3;
    if (h.bitrate_index == 0 || h.bitrate_index == 15 || h.samplerate_index == 3) return false;
    const int padding = (p[2] >> 1) & 1;
    const int rate = kSampleRate[h.samplerate_index];
    h.size = h.layer == 1 ? size_t((12000 * kBitrateLayerI[h.bitrate_index] / rate + padding) * 4)
                          : size_t(144000 * kBitrateLayerII[h.bitrate_index] / rate + padding);
    h.reg = 0xe00000u | uint32_t(p[1] & 0x1f) << 16 | uint32_t(p[2]) << 8 | p[3];
    return true;
}

// The layer, bitrate and sample rate bits of FrameHeader::reg.
constexpr uint32_t kFormatBits = 3u << 17 | 0xfu << 12 | 3u << 10;

} // namespace

// A zeroed `decoder` is what mp3dec_init() leaves.
struct DvcAudio::Impl {
    mp3dec_t decoder{};
    std::vector<uint8_t> input;
    // FrameHeader::reg of the last frame, 0 before the first. A header found
    // by searching must match its format (missed sync).
    uint32_t last_header = 0;
    bool synced = false; // input starts where the last frame ended
};

DvcAudio::DvcAudio() : impl_(std::make_unique<Impl>()) {}

DvcAudio::~DvcAudio() = default;

void DvcAudio::reset() { impl_ = std::make_unique<Impl>(); }

void DvcAudio::feed(const std::vector<uint8_t>& data) {
    impl_->input.insert(impl_->input.end(), data.begin(), data.end());
}

uint32_t DvcAudio::last_frame_header() const { return impl_->last_header; }

std::vector<int16_t> DvcAudio::decode_all() {
    Impl& s = *impl_;
    std::vector<int16_t> out;
    mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    size_t pos = 0;
    while (s.input.size() - pos >= 4) {
        FrameHeader h;
        bool found = parse_header(&s.input[pos], h);
        if (found && !s.synced && s.last_header != 0) found = ((h.reg ^ s.last_header) & kFormatBits) == 0;
        if (!found) {
            s.synced = false;
            ++pos;
            continue;
        }
        if (s.input.size() - pos < h.size) break; // wait for the rest of the frame
        // Exactly one frame, so minimp3 decodes it without looking for the next header.
        mp3dec_frame_info_t info{};
        const int samples = mp3dec_decode_frame(&s.decoder, &s.input[pos], int(h.size), pcm, &info);
        pos += h.size;
        s.synced = true;
        s.last_header = h.reg;
        if (samples <= 0) continue;
        const size_t base = out.size();
        out.resize(base + size_t(samples) * 2);
        for (int i = 0; i < samples; ++i) {
            const int16_t l = pcm[info.channels == 2 ? 2 * i : i];
            const int16_t r = pcm[info.channels == 2 ? 2 * i + 1 : i];
            out[base + 2 * size_t(i)] = l;
            out[base + 2 * size_t(i) + 1] = r;
        }
    }
    s.input.erase(s.input.begin(), s.input.begin() + std::ptrdiff_t(pos));
    return out;
}

template <class T>
void DvcAudio::state_io(T& archive) {
    archive.begin_section(state_tag("DVCA"));
    if constexpr (T::kLoading) impl_ = std::make_unique<Impl>();
    archive.io_struct(*impl_);
    archive.end_section();
}
ZCDI_STATE_IO_INSTANTIATE(DvcAudio);

} // namespace dvc
} // namespace zcdi

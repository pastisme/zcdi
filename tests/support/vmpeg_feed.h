#pragma once
// Feeds a stream through VmpegDevice's demuxer word by word, as the channel-1
// DMA does, and hands the elementary stream to DvcVideo the way Bus does.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "dvc/dvc_video.h"
#include "vmpeg/vmpeg.h"
#include "support/vmpeg_regs.h"

namespace zcdi_test {

class VmpegFeed {
public:
    // Audio: route the DMA to the FMA (FMA_CMD start DMA). Video: Decoder On,
    // without which the FMV demuxer is held in reset.
    explicit VmpegFeed(bool audio = false) {
        if (audio) write16(device_, kFmaCmdOffset, 0x8000);
        else write16(device_, kFmvSyscmdOffset, 0x1000);
    }

    // The demuxed elementary-stream bytes; an odd last byte waits for the next call.
    std::vector<uint8_t> push(const uint8_t* data, size_t size, bool audio = false) {
        for (size_t i = 0; i < size; ++i) {
            if (!has_pending_) {
                pending_ = data[i];
                has_pending_ = true;
            } else {
                device_.feed_dma_word(uint16_t(pending_ << 8 | data[i]));
                has_pending_ = false;
            }
        }
        return audio ? device_.drain_fma_es_bytes() : device_.drain_fmv_es_bytes();
    }

    // Sends a waiting odd byte, padded with 0xff.
    std::vector<uint8_t> flush(bool audio = false) {
        static const uint8_t kPad = 0xff;
        return has_pending_ ? push(&kPad, 1, audio) : std::vector<uint8_t>{};
    }

    // One field the way Bus feeds DvcVideo.
    void feed(zcdi::dvc::DvcVideo& video, const uint8_t* data, size_t size) {
        video.feed(push(data, size));
        if (device_.consume_fmv_program_end()) {
            video.note_program_end();
            ++program_ends_;
        }
    }

    void finish(zcdi::dvc::DvcVideo& video) {
        video.feed(flush());
        if (device_.consume_fmv_program_end()) {
            video.note_program_end();
            ++program_ends_;
        }
    }

    // FMV SYSCMD Clear FIFO, then the decoder's, as Bus does.
    void clear_fifo(zcdi::dvc::DvcVideo& video) {
        has_pending_ = false;
        write16(device_, kFmvSyscmdOffset, 0x0100);
        if (device_.consume_clear_fifo()) video.clear_fifo();
    }

    void set_video_stream(int number) { write16(device_, kFmvStreamNumOffset, uint16_t(number)); }

    int program_ends() const { return program_ends_; }
    zcdi::mamecdi::VmpegDevice& device() { return device_; }

private:
    zcdi::mamecdi::VmpegDevice device_;
    bool has_pending_ = false;
    uint8_t pending_ = 0;
    int program_ends_ = 0;
};

// The whole stream's elementary stream, as VMPEG demuxes it.
inline std::vector<uint8_t> demux_video(const uint8_t* data, size_t size) {
    VmpegFeed feed;
    std::vector<uint8_t> es = feed.push(data, size);
    std::vector<uint8_t> tail = feed.flush();
    es.insert(es.end(), tail.begin(), tail.end());
    return es;
}

// The shown frame's RGBA bytes, empty when nothing is shown.
inline std::vector<uint8_t> frame_bytes(const zcdi::dvc::DvcVideo& v) {
    if (!v.has_frame()) return {};
    const uint8_t* p = v.frame_rgba();
    return std::vector<uint8_t>(p, p + size_t(v.width()) * size_t(v.height()) * 4);
}

}  // namespace zcdi_test

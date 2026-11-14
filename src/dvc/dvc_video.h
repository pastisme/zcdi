#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace zcdi {
namespace dvc {

struct DvcVideoSaveState {
    bool has_frame_ = false;
    bool sequence_end_pending_ = false;
    bool end_of_data_ = false; // sequence/program end seen, no picture since
    bool last_picture_pending_ = false;
    bool program_end_waiting_ = false; // 0xB9 demuxed, applies once the input is used up

    // Converted to RGBA when presented, so the queue (and states) hold 4:2:0.
    struct PendingPicture {
        std::vector<uint8_t> yuv; // Y rows, then Cb, then Cr, unpadded
        int width = 0;
        int height = 0;
        bool starts_gop = false;
        bool starts_sequence = false;
        uint32_t timecode = 0;
        uint8_t video_status = 0;
        int picture_rate_code = 0;
    };
    std::deque<PendingPicture> queue_;
    // The last one presented; still there after decoder_off().
    PendingPicture presented_;

    // Latched as each picture finishes decoding, in decode order.
    uint32_t last_decoded_timecode_ = 0;
    bool picture_decoded_pending_ = false;
    int decoder_held_pictures_ = 0;
    int last_decoded_picture_rate_code_ = 0;
    int discarded_pictures_ = 0; // decoded, never queued (pushed before the first I)

    // Decoder side: what is known about each picture until it is pushed to the output FIFO.
    struct PictureInfo {
        uint32_t timecode = 0;
        uint16_t temporal_ref = 0; // 10 bits
        uint8_t coding_type = 0;
    };
    std::array<PictureInfo, 3> picture_info_; // per libmpeg2 frame buffer
    uint32_t gop_timecode_ = 0;
    bool sequence_header_latched_ = false;
    bool gop_intra_shown_ = false;
    bool intra_shown_ = false; // since the stream started
    int pictures_since_restart_ = 0;
    // A B picture right after the first I uses that I as forward reference.
    bool forward_fixup_pending_ = true;
    // A sequence header pushes the held reference picture out; libmpeg2 still
    // offers it later, then it's skipped. -1: none.
    int pushed_reference_buffer_ = -1;

    // The decoder handles one picture at a time: decode it, then push it to the output FIFO.
    struct DecoderStep {
        bool active = false;
        uint32_t done_at = 0; // DCLK
        bool decoded = false; // a picture, not only a push (sequence header/end)
        bool decoded_reported = false;
        uint32_t timecode = 0;
        int picture_rate_code = 0;
        bool push = false;
        bool discard = false; // pushed before the first I
        bool push_blocked = false;
        bool restart = false; // the next sequence starts afresh
        bool end_of_data = false; // 0xB7
        PendingPicture picture;
    };
    DecoderStep step_;
    uint32_t decoder_free_at_ = 0; // DCLK the last push completed
    bool decoder_idle_ = true; // nothing decoded since the input was cleared
    // When each part of the input arrived (DCLK), by stream offset.
    struct InputMark {
        uint64_t offset = 0;
        uint32_t time = 0;
    };
    std::vector<InputMark> input_marks_;
    uint64_t input_offset_ = 0; // stream offset of the first unparsed byte
    uint64_t picture_start_offset_ = 0;

    bool state_valid() const;
};

class DvcVideo : private DvcVideoSaveState {
public:
    // Decoded pictures waiting for display; a push waits while more than 2 are queued.
    static constexpr size_t kOutputFifoPictures = 3;
    // Decode time in DCLK (45 kHz) ticks: per picture, plus per KiB.
    static constexpr uint32_t kDecodeBaseTicks = 1000;
    static constexpr uint32_t kDecodeTicksPerKiB = 87;

    // Arrival time (DCLK) of the data from byte `offset` of a feed() on.
    struct ArrivalMark {
        uint32_t offset = 0;
        uint32_t time = 0;
    };

    DvcVideo();
    ~DvcVideo();

    DvcVideo(const DvcVideo&) = delete;
    DvcVideo& operator=(const DvcVideo&) = delete;

    // Video elementary-stream bytes (VMPEG demuxes program streams).
    void feed(const std::vector<uint8_t>& data, const std::vector<ArrivalMark>& arrivals = {});

    // 0xB9 demuxed: the end once the input is decoded.
    void note_program_end() { program_end_waiting_ = true; }

    // Keeps the sequence header and the picture on screen.
    void clear_fifo();

    // Decoder Off: as clear_fifo(), and also forgets the sequence header and
    // drops the picture on screen.
    void decoder_off();

    // Runs the decoder up to `now` (DCLK); true if a picture was queued. Without `now`,
    // decoding takes no time and stops at the first queued picture.
    bool decode_frame(std::optional<uint32_t> now = std::nullopt);

    // After present_next_frame(): a push waiting for room in the output FIFO completes.
    void resume_push(std::optional<uint32_t> now = std::nullopt);

    bool present_next_frame();

    // The presented picture, not the last decoded one.
    bool has_frame() const { return has_frame_; }
    int width() const { return presented_.width; }
    int height() const { return presented_.height; }
    // RGBA8888, width()*height()*4 bytes, valid only while has_frame().
    const uint8_t* frame_rgba() const { return rgba_.data(); }

    // The first I picture of a GOP or of a sequence.
    bool presented_starts_gop() const { return presented_.starts_gop; }
    bool presented_starts_sequence() const { return presented_.starts_sequence; }

    // One-shot: 0xB7 decoded.
    bool consume_sequence_end() { return std::exchange(sequence_end_pending_, false); }

    // One-shot: the last picture starts display, only at stream end (not on underrun).
    bool consume_last_picture_shown() { return std::exchange(last_picture_pending_, false); }

    // One-shot: a picture finished decoding since the last call (the last_decoded_* values).
    bool consume_picture_decoded() { return std::exchange(picture_decoded_pending_, false); }

    // Pictures decoded but not queued since the last call.
    int consume_discarded_pictures() { return std::exchange(discarded_pictures_, 0); }

    // Time codes already in the FMV register layout.
    uint32_t last_decoded_timecode() const { return last_decoded_timecode_; }
    int last_decoded_picture_rate_code() const { return last_decoded_picture_rate_code_; }
    uint32_t presented_timecode() const { return presented_.timecode; }
    int presented_picture_rate_code() const { return presented_.picture_rate_code; }
    bool has_next_picture() const { return !queue_.empty(); }
    int next_width() const { return queue_.front().width; }
    int next_height() const { return queue_.front().height; }
    int next_picture_rate_code() const { return queue_.front().picture_rate_code; }
    // SYS_VSR
    uint8_t presented_video_status() const { return presented_.video_status; }

    // Decoded but not yet pushed, held reference included.
    int decoder_held_pictures() const { return decoder_held_pictures_; }
    size_t queue_depth() const { return queue_.size(); }
    // Fed but not yet parsed.
    size_t input_bytes() const;

    template <class T>
    void state_io(T& archive);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    // presented_ as RGBA; rebuilt on load, not saved.
    std::vector<uint8_t> rgba_;

    bool make_pending(const void* fbuf, PendingPicture& pending);
    void begin_step(uint32_t done_at);
    void begin_push(const void* fbuf);
    bool finish_step(std::optional<uint32_t> now);
    bool parse_step(std::optional<uint32_t> now);
    uint32_t arrival(uint64_t offset) const;
    void convert_presented();
};

} // namespace dvc
} // namespace zcdi

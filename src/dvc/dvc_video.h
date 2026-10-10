#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <memory>
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
    };
    std::deque<PendingPicture> queue_;
    // The last one presented; still there after decoder_off().
    PendingPicture presented_;

    // Latched as each picture finishes decoding, in decode order (mpeg_video.sv event_frame_decoded).
    uint32_t last_decoded_timecode_ = 0;
    bool picture_decoded_pending_ = false;
    int decoder_held_pictures_ = 0;
    int last_decoded_width_ = 0;
    int last_decoded_height_ = 0;
    int last_decoded_picture_rate_code_ = 0;

    // Decoder side, as CDi_MiSTer's sw/mpeg_video/main.c push_frame() and pl_mpeg.h track it.
    struct PictureInfo {
        uint32_t timecode = 0;
        uint16_t temporal_ref = 0; // 10 bits
        uint8_t coding_type = 0;
    };
    std::array<PictureInfo, 3> picture_info_; // per libmpeg2 frame buffer
    uint32_t gop_timecode_ = 0;
    bool sequence_header_latched_ = false; // seq_hdr_latched
    bool gop_intra_shown_ = false; // first_intra_frame_of_gop_occured
    bool intra_shown_ = false; // intra_frame_occured_during_stream
    int pictures_since_restart_ = 0;
    // A B picture right after the first I uses that I as forward reference.
    bool forward_fixup_pending_ = true;
};

class DvcVideo : private DvcVideoSaveState {
public:
    // Decode pauses at this many queued pictures (sw/mpeg_video/main.c > 25).
    static constexpr size_t kMaxDecodedPictures = 26;

    DvcVideo();
    ~DvcVideo();

    DvcVideo(const DvcVideo&) = delete;
    DvcVideo& operator=(const DvcVideo&) = delete;

    // Video elementary-stream bytes (VMPEG demuxes program streams).
    void feed(const std::vector<uint8_t>& data);

    // 0xB9 demuxed (mpeg_demuxer.sv event_program_end): the end once the input is decoded.
    void note_program_end() { program_end_waiting_ = true; }

    // Keeps the sequence header and the picture on screen.
    void clear_fifo();

    // Decoder Off: as clear_fifo(), and also forgets the sequence header and
    // drops the picture on screen (mpeg_video.sv reset_persistent_storage).
    void decoder_off();

    // Decodes and queues at most one picture.
    bool decode_frame();

    bool present_next_frame();

    // The presented picture, not the last decoded one.
    bool has_frame() const { return has_frame_; }
    int width() const { return presented_.width; }
    int height() const { return presented_.height; }
    // RGBA8888, width()*height()*4 bytes, valid only while has_frame().
    const uint8_t* frame_rgba() const { return rgba_.data(); }

    // mpeg_video.sv event_first_intra_frame_of_gop/_of_seq.
    bool presented_starts_gop() const { return presented_.starts_gop; }
    bool presented_starts_sequence() const { return presented_.starts_sequence; }

    // One-shot: 0xB7 decoded (mpeg_video.sv event_sequence_end).
    bool consume_sequence_end() { return std::exchange(sequence_end_pending_, false); }

    // One-shot: event_last_picture_starts_display, only at stream end (not on underrun).
    bool consume_last_picture_shown() { return std::exchange(last_picture_pending_, false); }

    // One-shot: a picture finished decoding since the last call (the last_decoded_* values).
    bool consume_picture_decoded() { return std::exchange(picture_decoded_pending_, false); }

    // Time codes already in the FMV register layout.
    uint32_t last_decoded_timecode() const { return last_decoded_timecode_; }
    int last_decoded_width() const { return last_decoded_width_; }
    int last_decoded_height() const { return last_decoded_height_; }
    int last_decoded_picture_rate_code() const { return last_decoded_picture_rate_code_; }
    uint32_t presented_timecode() const { return presented_.timecode; }
    // SYS_VSR
    uint8_t presented_video_status() const { return presented_.video_status; }

    // 0 or 1: a reference picture decoded but not yet output (RTL: pictures_in_mpeg_decoder).
    int decoder_held_pictures() const { return decoder_held_pictures_; }
    size_t queue_depth() const { return queue_.size(); }
    // Fed but not yet parsed (mpeg_video.sv fifo_level).
    size_t input_bytes() const;

    template <class T>
    void state_io(T& archive);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    // presented_ as RGBA; rebuilt on load, not saved.
    std::vector<uint8_t> rgba_;

    bool output_picture(const void* fbuf);
    void convert_presented();
    bool loaded() const;
};

} // namespace dvc
} // namespace zcdi

#include "dvc/dvc.h"

#include <cmath>
#include <utility>

#include "core/state_archive.h"
#include "core/util.h"

namespace zcdi {
namespace dvc {

template <class T>
void Dvc::state_io(T& archive) {
    archive.begin_section(state_tag("DVC "));
    archive.io_struct(static_cast<FmaSaveState&>(*this));
    archive.io_struct(static_cast<FmvSaveState&>(*this));
    archive.end_section();
    video_.state_io(archive);
    audio_.state_io(archive);
}
ZCDI_STATE_IO_INSTANTIATE(Dvc);

bool Dvc::tick_field(double refresh_rate_hz) {
    // Decoder Off also drops the shown picture so it can't flash up at the next Show.
    const bool picture_dropped = std::exchange(decoder_off_pending_, false);
    if (picture_dropped) video_.decoder_off();
    // Before feeding, so a cut-off picture isn't glued onto the next clip.
    if (std::exchange(clear_fifo_pending_, false)) {
        video_.clear_fifo();
    }
    // Demuxed by VMPEG from the channel-1 DMA and XFER writes.
    std::vector<DvcVideo::ArrivalMark> arrivals;
    for (const auto& a : std::exchange(fmv_es_arrivals_, {}))
        arrivals.push_back({a.offset, a.time});
    video_.feed(std::exchange(fmv_es_bytes_, {}), arrivals);
    if (std::exchange(fmv_program_end_pending_, false)) video_.note_program_end();
    const uint32_t now = fma_dclk_;
    video_.decode_frame(now);
    for (int n = video_.consume_discarded_pictures(); n > 0; --n) {
        note_picture_discarded();
    }
    if (video_.consume_picture_decoded()) {
        set_video_decoder_timecode(video_.last_decoded_timecode());
        set_video_frame_decoded(video_.last_decoded_picture_rate_code());
    }
    fmv_decoder_input_bytes_ = uint32_t(video_.input_bytes());

    // The frame player decides presentation.
    const uint32_t field_clocks = uint32_t(std::lround(30000000.0 / refresh_rate_hz));
    field_samples_ = int32_t(std::lround(44100.0 / refresh_rate_hz));
    // One vsync edge per field: RegsUpd is taken there, before the picture is selected.
    notify_vsync();
    const bool show = frame_player_field(field_clocks, video_.queue_depth());
    // PIC, GOP and SEQ belong to the display process.
    if (show && video_.present_next_frame()) {
        note_picture_displayed();
        // The latch freed a slot: a waiting push_frame() goes ahead before line 11.
        video_.resume_push(now);
        check_buffer_underflow(video_.queue_depth(), video_.decoder_held_pictures());
        set_video_frame_ready(video_.width(), video_.height());
        set_video_display_timecode(video_.presented_timecode());
        set_video_display_status(video_.presented_video_status());
        set_video_display_picture_rate(video_.presented_picture_rate_code());
        if (video_.presented_starts_gop()) set_video_gop_start();
        if (video_.presented_starts_sequence()) set_video_sequence_start();
    }
    if (video_.has_next_picture()) {
        set_next_picture(video_.next_width(), video_.next_height(), video_.next_picture_rate_code());
    }
    // No picture of its own, so checked every field.
    if (video_.consume_sequence_end()) set_video_sequence_end();
    if (video_.consume_last_picture_shown()) set_video_last_picture();
    return picture_dropped;
}

std::vector<int16_t> Dvc::drain_audio() {
    // Demuxed by VMPEG from the channel-1 DMA; all of it arrived after any FMA stop.
    std::vector<uint8_t> bytes = std::exchange(fma_es_bytes_, {});
    if (std::exchange(audio_reset_pending_, false)) {
        audio_.reset();
    }

    audio_.feed(bytes);
    // PS audio waits until the FMA playback timer reaches its first PTS.
    const bool hold = audio_start_pending();
    std::vector<int16_t> pcm = hold ? std::vector<int16_t>{} : audio_.decode_all();
    if (!pcm.empty()) {
        if (!audio_dsp_enabled()) set_audio_decoding_started();
        set_audio_frame_decoded(audio_.last_frame_header());
    }
    // FMA underflow, once a DMA has fed the FMA.
    if (cartridge_present_ && fma_dma_seen_) audio_field(int32_t(pcm.size() / 2), field_samples_);
    // DAC gain matrix, Q0.8 (256 = unity). The gains power up at 0 and
    // apply as each is written: muted until the driver programs them.
    const int l2l = mpeg_volume_l2l_, r2r = mpeg_volume_r2r_;
    const int l2r = mpeg_volume_l2r_, r2l = mpeg_volume_r2l_;
    std::vector<int16_t> out(pcm.size() & ~std::size_t(1), 0);
    for (std::size_t i = 0; i + 1 < pcm.size(); i += 2) {
        int l = pcm[i], r = pcm[i + 1];
        out[i] = clamp16((l * l2l + r * r2l) >> 8);
        out[i + 1] = clamp16((l * l2r + r * r2r) >> 8);
    }
    return out;
}

} // namespace dvc
} // namespace zcdi

#pragma once
// Dvc's internal steps and state, for tests of one part at a time. Reaching
// them through registers, DMA and tick_field() would take a whole program
// stream; read state through Dvc::fmv_state() and fma_state() instead.
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "dvc/dvc.h"
#include "support/access_private.h"

ACCESS_PRIVATE_FUN(zcdi::dvc::Dvc, bool(uint32_t, size_t), frame_player_field)
ACCESS_PRIVATE_FUN(zcdi::dvc::Dvc, void(), notify_vsync)
ACCESS_PRIVATE_FUN(zcdi::dvc::Dvc, bool(), video_display_enabled)
ACCESS_PRIVATE_FUN(zcdi::dvc::Dvc, void(), note_picture_displayed)
ACCESS_PRIVATE_FUN(zcdi::dvc::Dvc, void(), note_picture_discarded)
ACCESS_PRIVATE_FUN(zcdi::dvc::Dvc, bool() const, audio_dsp_enabled)
ACCESS_PRIVATE_FUN(zcdi::dvc::Dvc, void(size_t, int), check_buffer_underflow)
ACCESS_PRIVATE_FUN(zcdi::dvc::Dvc, void(int, int), set_video_frame_ready)
ACCESS_PRIVATE_FUN(zcdi::dvc::Dvc, void(int), set_video_frame_decoded)
ACCESS_PRIVATE_FUN(zcdi::dvc::Dvc, void(int, int, int), set_next_picture)
ACCESS_PRIVATE_FUN(zcdi::dvc::Dvc, void(), set_video_gop_start)
ACCESS_PRIVATE_FUN(zcdi::dvc::Dvc, void(), set_video_sequence_start)
ACCESS_PRIVATE_FUN(zcdi::dvc::Dvc, void(), set_video_last_picture)
ACCESS_PRIVATE_FUN(zcdi::dvc::Dvc, void(uint32_t), set_video_decoder_timecode)
ACCESS_PRIVATE_FUN(zcdi::dvc::Dvc, void(uint32_t), set_video_display_timecode)
ACCESS_PRIVATE_FUN(zcdi::dvc::Dvc, void(int), set_video_display_picture_rate)
ACCESS_PRIVATE_FUN(zcdi::dvc::Dvc, void(), set_audio_decoding_started)
ACCESS_PRIVATE_FUN(zcdi::dvc::Dvc, void(uint32_t), set_audio_frame_decoded)
ACCESS_PRIVATE_FUN(zcdi::dvc::Dvc, void(int32_t, int32_t), audio_field)

namespace zcdi_test {

using zcdi::dvc::Dvc;

// Dvc's saved state, writable.
inline zcdi::dvc::FmvSaveState& fmv(Dvc& d) { return saved_state<zcdi::dvc::FmvSaveState>(d); }
inline zcdi::dvc::FmaSaveState& fma(Dvc& d) { return saved_state<zcdi::dvc::FmaSaveState>(d); }

// As if the Play delay had elapsed (or Stop/Pause had taken effect).
inline void set_playback_active(Dvc& d, bool active) { fmv(d).fmv_playback_active_ = active; }
inline void set_fmv_dclk(Dvc& d, uint32_t dclk) { fmv(d).fmv_dclk_ = dclk; }
// One DTS (90 kHz, 0 = none) per picture not yet shown.
inline std::vector<int64_t>& dts_fifo(Dvc& d) { return fmv(d).fmv_dts_fifo_; }
// The last pack SCR the FMV demuxer read (90 kHz).
inline void set_demux_scr(Dvc& d, int64_t scr) {
    fmv(d).fmv_demux_.scr = scr;
    fmv(d).fmv_demux_scr_valid_ = true;
}
// Past the wait for the first picture's DTS.
inline void set_dts_start_satisfied(Dvc& d) { fmv(d).fmv_dts_start_satisfied_ = true; }
inline void set_decoder_input_bytes(Dvc& d, size_t bytes) { fmv(d).fmv_decoder_input_bytes_ = uint32_t(bytes); }

// What tick_field() hands the decoders: the demuxed streams and pending commands.
inline std::vector<uint8_t> drain_fmv_es_bytes(Dvc& d) { return std::exchange(fmv(d).fmv_es_bytes_, {}); }
inline std::vector<uint8_t> drain_fma_es_bytes(Dvc& d) { return std::exchange(fma(d).fma_es_bytes_, {}); }
inline bool consume_fmv_program_end(Dvc& d) { return std::exchange(fmv(d).fmv_program_end_pending_, false); }
inline bool consume_clear_fifo(Dvc& d) { return std::exchange(fmv(d).clear_fifo_pending_, false); }
inline bool consume_decoder_off(Dvc& d) { return std::exchange(fmv(d).decoder_off_pending_, false); }

// Unlike reading the register, doesn't clear it.
inline uint16_t fmv_isr(const Dvc& d) { return d.fmv_state().fmv_interrupt_status_register_; }

inline bool frame_player_field(Dvc& d, uint32_t field_clocks, size_t pictures_ready) {
    return call_private::frame_player_field(d, field_clocks, pictures_ready);
}
inline void notify_vsync(Dvc& d) { call_private::notify_vsync(d); }
inline bool video_display_enabled(Dvc& d) { return call_private::video_display_enabled(d); }
inline void note_picture_displayed(Dvc& d) { call_private::note_picture_displayed(d); }
inline void note_picture_discarded(Dvc& d) { call_private::note_picture_discarded(d); }
inline bool audio_dsp_enabled(Dvc& d) { return call_private::audio_dsp_enabled(d); }
inline void check_buffer_underflow(Dvc& d, size_t output_pictures, int decoder_pictures) {
    call_private::check_buffer_underflow(d, output_pictures, decoder_pictures);
}
inline void set_video_frame_ready(Dvc& d, int width, int height) {
    call_private::set_video_frame_ready(d, width, height);
}
inline void set_video_frame_decoded(Dvc& d, int rate_code) { call_private::set_video_frame_decoded(d, rate_code); }
inline void set_next_picture(Dvc& d, int width, int height, int rate_code) {
    call_private::set_next_picture(d, width, height, rate_code);
}
inline void set_video_gop_start(Dvc& d) { call_private::set_video_gop_start(d); }
inline void set_video_sequence_start(Dvc& d) { call_private::set_video_sequence_start(d); }
inline void set_video_last_picture(Dvc& d) { call_private::set_video_last_picture(d); }
inline void set_video_decoder_timecode(Dvc& d, uint32_t tc) { call_private::set_video_decoder_timecode(d, tc); }
inline void set_video_display_timecode(Dvc& d, uint32_t tc) { call_private::set_video_display_timecode(d, tc); }
inline void set_video_display_picture_rate(Dvc& d, int rate_code) {
    call_private::set_video_display_picture_rate(d, rate_code);
}
inline void set_audio_decoding_started(Dvc& d) { call_private::set_audio_decoding_started(d); }
inline void set_audio_frame_decoded(Dvc& d, uint32_t header) { call_private::set_audio_frame_decoded(d, header); }
inline void audio_field(Dvc& d, int32_t decoded, int32_t field_samples) {
    call_private::audio_field(d, decoded, field_samples);
}

}  // namespace zcdi_test

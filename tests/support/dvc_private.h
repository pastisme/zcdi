#pragma once
// Dvc's internal steps and state, for tests of one part at a time. Reaching
// them through registers, DMA and tick_field() would take a whole program
// stream; read state through Dvc::fmv_state() and fma_state() instead.
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "dvc/dvc.h"
#include "support/private_access.h"

ZCDI_PRIVATE_MEMBER(DvcPlaybackActive, zcdi::dvc::FmvSaveState, fmv_playback_active_, bool);
ZCDI_PRIVATE_MEMBER(DvcFmvDclk, zcdi::dvc::FmvSaveState, fmv_dclk_, uint32_t);
ZCDI_PRIVATE_MEMBER(DvcDtsFifo, zcdi::dvc::FmvSaveState, fmv_dts_fifo_, std::vector<int64_t>);
ZCDI_PRIVATE_MEMBER(DvcFmvDemux, zcdi::dvc::FmvSaveState, fmv_demux_, zcdi::dvc::PsDemux);
ZCDI_PRIVATE_MEMBER(DvcScrValid, zcdi::dvc::FmvSaveState, fmv_demux_scr_valid_, bool);
ZCDI_PRIVATE_MEMBER(DvcStartSatisfied, zcdi::dvc::FmvSaveState, fmv_dts_start_satisfied_, bool);
ZCDI_PRIVATE_MEMBER(DvcFmvEsBytes, zcdi::dvc::FmvSaveState, fmv_es_bytes_, std::vector<uint8_t>);
ZCDI_PRIVATE_MEMBER(DvcFmaEsBytes, zcdi::dvc::FmaSaveState, fma_es_bytes_, std::vector<uint8_t>);
ZCDI_PRIVATE_MEMBER(DvcProgramEndPending, zcdi::dvc::FmvSaveState, fmv_program_end_pending_, bool);
ZCDI_PRIVATE_MEMBER(DvcClearFifoPending, zcdi::dvc::FmvSaveState, clear_fifo_pending_, bool);
ZCDI_PRIVATE_MEMBER(DvcDecoderOffPending, zcdi::dvc::FmvSaveState, decoder_off_pending_, bool);
ZCDI_PRIVATE_MEMBER(DvcDecoderInputBytes, zcdi::dvc::FmvSaveState, fmv_decoder_input_bytes_, uint32_t);

ZCDI_PRIVATE_MEMBER(DvcFramePlayerField, zcdi::dvc::Dvc, frame_player_field, bool(uint32_t, size_t));
ZCDI_PRIVATE_MEMBER(DvcNotifyVsync, zcdi::dvc::Dvc, notify_vsync, void());
ZCDI_PRIVATE_MEMBER(DvcVideoDisplayEnabled, zcdi::dvc::Dvc, video_display_enabled, bool());
ZCDI_PRIVATE_MEMBER(DvcNotePictureDisplayed, zcdi::dvc::Dvc, note_picture_displayed, void());
ZCDI_PRIVATE_MEMBER(DvcNotePictureDiscarded, zcdi::dvc::Dvc, note_picture_discarded, void());
ZCDI_PRIVATE_MEMBER(DvcAudioDspEnabled, zcdi::dvc::Dvc, audio_dsp_enabled, bool() const);
ZCDI_PRIVATE_MEMBER(DvcCheckBufferUnderflow, zcdi::dvc::Dvc, check_buffer_underflow, void(size_t, int));
ZCDI_PRIVATE_MEMBER(DvcSetVideoFrameReady, zcdi::dvc::Dvc, set_video_frame_ready, void(int, int));
ZCDI_PRIVATE_MEMBER(DvcSetVideoFrameDecoded, zcdi::dvc::Dvc, set_video_frame_decoded, void(int));
ZCDI_PRIVATE_MEMBER(DvcSetNextPicture, zcdi::dvc::Dvc, set_next_picture, void(int, int, int));
ZCDI_PRIVATE_MEMBER(DvcSetVideoGopStart, zcdi::dvc::Dvc, set_video_gop_start, void());
ZCDI_PRIVATE_MEMBER(DvcSetVideoSequenceStart, zcdi::dvc::Dvc, set_video_sequence_start, void());
ZCDI_PRIVATE_MEMBER(DvcSetVideoLastPicture, zcdi::dvc::Dvc, set_video_last_picture, void());
ZCDI_PRIVATE_MEMBER(DvcSetVideoDecoderTimecode, zcdi::dvc::Dvc, set_video_decoder_timecode, void(uint32_t));
ZCDI_PRIVATE_MEMBER(DvcSetVideoDisplayTimecode, zcdi::dvc::Dvc, set_video_display_timecode, void(uint32_t));
ZCDI_PRIVATE_MEMBER(DvcSetVideoDisplayPictureRate, zcdi::dvc::Dvc, set_video_display_picture_rate, void(int));
ZCDI_PRIVATE_MEMBER(DvcSetAudioDecodingStarted, zcdi::dvc::Dvc, set_audio_decoding_started, void());
ZCDI_PRIVATE_MEMBER(DvcSetAudioFrameDecoded, zcdi::dvc::Dvc, set_audio_frame_decoded, void(uint32_t));
ZCDI_PRIVATE_MEMBER(DvcAudioField, zcdi::dvc::Dvc, audio_field, void(int32_t, int32_t));

namespace zcdi_test {

using zcdi::dvc::Dvc;

// As if the Play delay had elapsed (or Stop/Pause had taken effect).
inline void set_playback_active(Dvc& d, bool active) { priv<DvcPlaybackActive>(d) = active; }
inline void set_fmv_dclk(Dvc& d, uint32_t dclk) { priv<DvcFmvDclk>(d) = dclk; }
// One DTS (90 kHz, 0 = none) per picture not yet shown.
inline std::vector<int64_t>& dts_fifo(Dvc& d) { return priv<DvcDtsFifo>(d); }
// The last pack SCR the FMV demuxer read (90 kHz).
inline void set_demux_scr(Dvc& d, int64_t scr) {
    priv<DvcFmvDemux>(d).scr = scr;
    priv<DvcScrValid>(d) = true;
}
// Past the wait for the first picture's DTS.
inline void set_dts_start_satisfied(Dvc& d) { priv<DvcStartSatisfied>(d) = true; }
inline void set_decoder_input_bytes(Dvc& d, size_t bytes) { priv<DvcDecoderInputBytes>(d) = uint32_t(bytes); }

// What tick_field() hands the decoders: the demuxed streams and pending commands.
inline std::vector<uint8_t> drain_fmv_es_bytes(Dvc& d) { return std::exchange(priv<DvcFmvEsBytes>(d), {}); }
inline std::vector<uint8_t> drain_fma_es_bytes(Dvc& d) { return std::exchange(priv<DvcFmaEsBytes>(d), {}); }
inline bool consume_fmv_program_end(Dvc& d) { return std::exchange(priv<DvcProgramEndPending>(d), false); }
inline bool consume_clear_fifo(Dvc& d) { return std::exchange(priv<DvcClearFifoPending>(d), false); }
inline bool consume_decoder_off(Dvc& d) { return std::exchange(priv<DvcDecoderOffPending>(d), false); }

// Unlike reading the register, doesn't clear it.
inline uint16_t fmv_isr(const Dvc& d) { return d.fmv_state().fmv_interrupt_status_register_; }

inline bool frame_player_field(Dvc& d, uint32_t field_clocks, size_t pictures_ready) {
    return priv<DvcFramePlayerField>(d, field_clocks, pictures_ready);
}
inline void notify_vsync(Dvc& d) { priv<DvcNotifyVsync>(d); }
inline bool video_display_enabled(Dvc& d) { return priv<DvcVideoDisplayEnabled>(d); }
inline void note_picture_displayed(Dvc& d) { priv<DvcNotePictureDisplayed>(d); }
inline void note_picture_discarded(Dvc& d) { priv<DvcNotePictureDiscarded>(d); }
inline bool audio_dsp_enabled(Dvc& d) { return priv<DvcAudioDspEnabled>(d); }
inline void check_buffer_underflow(Dvc& d, size_t output_pictures, int decoder_pictures) {
    priv<DvcCheckBufferUnderflow>(d, output_pictures, decoder_pictures);
}
inline void set_video_frame_ready(Dvc& d, int width, int height) { priv<DvcSetVideoFrameReady>(d, width, height); }
inline void set_video_frame_decoded(Dvc& d, int rate_code) { priv<DvcSetVideoFrameDecoded>(d, rate_code); }
inline void set_next_picture(Dvc& d, int width, int height, int rate_code) {
    priv<DvcSetNextPicture>(d, width, height, rate_code);
}
inline void set_video_gop_start(Dvc& d) { priv<DvcSetVideoGopStart>(d); }
inline void set_video_sequence_start(Dvc& d) { priv<DvcSetVideoSequenceStart>(d); }
inline void set_video_last_picture(Dvc& d) { priv<DvcSetVideoLastPicture>(d); }
inline void set_video_decoder_timecode(Dvc& d, uint32_t tc) { priv<DvcSetVideoDecoderTimecode>(d, tc); }
inline void set_video_display_timecode(Dvc& d, uint32_t tc) { priv<DvcSetVideoDisplayTimecode>(d, tc); }
inline void set_video_display_picture_rate(Dvc& d, int rate_code) { priv<DvcSetVideoDisplayPictureRate>(d, rate_code); }
inline void set_audio_decoding_started(Dvc& d) { priv<DvcSetAudioDecodingStarted>(d); }
inline void set_audio_frame_decoded(Dvc& d, uint32_t header) { priv<DvcSetAudioFrameDecoded>(d, header); }
inline void audio_field(Dvc& d, int32_t decoded, int32_t field_samples) { priv<DvcAudioField>(d, decoded, field_samples); }

}  // namespace zcdi_test

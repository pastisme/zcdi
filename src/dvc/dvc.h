#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "chips/slave/slave.h" // VideoStandard
#include "dvc/dvc_audio.h"
#include "dvc/dvc_video.h"
#include "dvc/ps_demux.h"
#include "dvc/vmpeg.h"

namespace zcdi {
namespace dvc {

// One set of FMV window registers as RegsUpd latches them (display_offset_y holds SCRPOS X).
struct FmvWindow {
    uint16_t display_offset_y = 0, display_offset_x = 0;
    uint16_t window_offset_y = 0, window_offset_x = 0;
    uint16_t width = 0, height = 0;
};

// A RegsUpd taken at vsync, on its way to the latched window.
struct FmvWindowUpdate {
    bool late = false; // written after the window started scanning out
    // A non-scroll update waiting for a picture.
    FmvWindow next;
    bool next_pending = false;
    bool next_late = false;
    // A late update, held for the picture after.
    FmvWindow deferred;
    bool deferred_pending = false;
    bool last_picture_held = false; // EOD shown, no picture follows
};

// What each channel's input turned out to be.
enum DemuxMode : uint8_t {
    kDemuxModeUnknown = 0,
    kDemuxModeProgram = 1,
    kDemuxModeElementary = 2,
};

// The FMA (MPEG audio) block and its input.
struct FmaSaveState {
    // FMA, 0xe03000-0xe03024
    uint16_t fma_command_register_ = 0;
    uint8_t fma_status_register_ = 0;
    uint8_t fma_stream_number_ = 0;
    bool fma_stream_change_pending_ = false; // raises CSU with the next audio frame
    uint16_t fma_interrupt_status_register_ = 0;
    uint32_t fma_mpeg_audio_header_ = 0;
    uint16_t fma_interrupt_enable_register_ = 0;
    uint16_t fma_interrupt_vector_register_ = 0;
    uint32_t fma_dclk_ = 0; // ~45 kHz free-running counter
    uint16_t fma_dclkl_latch_ = 0;
    uint8_t fma_dspa_ = 0;
    uint8_t fma_dsp_mode_ = 0;
    uint8_t fma_dsp_target_ = 0;
    uint8_t fma_dsp_write_index_ = 0;
    uint8_t mpeg_volume_r2r_ = 0;
    uint8_t mpeg_volume_l2r_ = 0;
    uint8_t mpeg_volume_r2l_ = 0;
    uint8_t mpeg_volume_l2l_ = 0;
    // Decoded audio not yet played, in 44.1 kHz sample frames (see audio_field).
    int32_t fma_unplayed_samples_ = 0;
    uint8_t fma_demux_mode_ = kDemuxModeUnknown;
    PsDemux fma_demux_;
    bool fma_ps_seen_ = false;
    bool fma_start_valid_ = false;
    uint32_t fma_start_dclk_ = 0;
    static constexpr size_t kMaxFmaPsSniffBytes = 64 * 2324; // ~64 Form-2 sectors
    std::vector<uint8_t> fma_sniff_bytes_; // input kept until PS or ES is decided
    // Demuxed elementary stream for the audio decoder.
    std::vector<uint8_t> fma_es_bytes_;
    bool audio_reset_pending_ = false;
    // Underflow is only reported once a DMA has fed the FMA.
    bool fma_dma_seen_ = false;
};

// The FMV (MPEG video) block, its window and the host interface.
struct FmvSaveState {
    // FMV, 0xe04000-0xe040e6
    uint32_t fmv_dclk_ = 0;
    uint32_t fmv_dclk_start_video_ = 0;
    bool fmv_dclk_start_video_latched_ = false;
    uint32_t fmv_dclk_pause_video_ = 0;
    bool fmv_dclk_pause_video_latched_ = false;
    uint16_t fmv_system_command_register_ = 0;
    uint16_t fmv_video_command_register_ = 0;
    uint16_t fmv_system_control_register_ = 0;
    bool fmv_playback_active_ = false;
    bool fmv_show_on_next_video_frame_ = false;
    uint16_t fmv_interrupt_status_register_ = 0;
    uint16_t fmv_interrupt_enable_register_ = 0;
    uint16_t fmv_interrupt_vector_register_ = 0;
    uint16_t fmv_timer_compare_register_ = 56 - 1; // TCNT, reset value
    uint32_t timer_cnt_ = 0; // 21 bits
    uint16_t fmv_decoder_command_ = 0; // GEN_DEC_CMD, 0xe04088
    uint16_t fmv_video_data_input_command_register_ = 0; // GEN_VDI_CMD
    uint16_t image_width_ = 0, image_height_ = 0, image_rt_ = 0;
    bool fmv_decoder_active_ = false; // Play to Stop/Decoder Off
    uint16_t display_width_ = 0, display_height_ = 0; // last picture shown
    uint16_t decoder_frameperiod_90khz_ = 0;
    uint8_t display_frameperiod_rawhdr_ = 0; // Pic Rt of the picture shown
    uint8_t fmv_pictures_received_ = 0; // since Decoder On/Clear FIFO, 7 bits
    uint32_t fmv_decoder_input_bytes_ = 0; // handed to the decoder, not yet parsed
    bool fmv_dts_start_satisfied_ = false; // display may start: the head picture's DTS is reached, or it has none
    int64_t player_frame_cnt_ = 0;
    bool player_latch_pending_ = false;
    int player_desync_q_ = 0;
    uint8_t player_desync_hold_ = 0; // pictures the last desync still holds for
    int64_t player_predicted_dts_ = 0; // 90 kHz
    bool player_predicted_dts_valid_ = false;
    uint32_t player_frame_period_ = 1200000;
    uint8_t fmv_slow_motion_ = 0; // SYSCMD bits 2-0; period x (n + 1)
    bool player_step_latch_ = false;
    bool player_playback_active_delayed_ = false;
    bool ndat_armed_ = false;
    bool ndat_line_pending_ = false; // NDAT waits for line 11
    // PIC/GOP/SEQ/EOD/NDAT of the picture selected at vsync, raised when vertical blanking ends.
    uint16_t fmv_display_events_pending_ = 0;

    // FMV program-stream demuxer and the DTS of pictures not yet shown.
    uint8_t fmv_demux_mode_ = kDemuxModeUnknown;
    PsDemux fmv_demux_;
    uint32_t fmv_es_shift_ = 0xffffffff;
    // The decoder has a sequence header: pictures before one are discarded.
    bool fmv_es_has_sequence_header_ = false;
    int64_t fmv_demuxer_dts_ = 0; // 33-bit, 90 kHz
    bool fmv_demux_scr_valid_ = false;
    // dclk ticks until fmv_dclk_ takes the last pack's SCR; 0 = none pending.
    uint8_t fmv_scr_load_countdown_ = 0;
    int64_t fmv_next_picture_dts_ = 0;
    static constexpr size_t kDtsFifoDepth = 127; // 7-bit count
    std::vector<int64_t> fmv_dts_fifo_; // one entry per picture not yet shown
    uint8_t fmv_stream_number_ = 0;

    // Window registers as written, and the copies RegsUpd latches.
    uint16_t video_ctrl_y_offset_ = 0, video_ctrl_x_offset_ = 0;
    uint16_t video_ctrl_y_active_ = 0, video_ctrl_x_active_ = 0;
    uint16_t video_ctrl_y_display_ = 0, video_ctrl_x_display_ = 0;
    uint16_t video_ctrl_window_width_ = 0, video_ctrl_window_height_ = 0;
    uint16_t video_ctrl_decoder_offset_y_ = 0, video_ctrl_decoder_offset_x_ = 0;
    FmvWindow latched_window_; // what display_window() reports
    bool register_update_latch_ = false;
    bool register_update_scroll_ = false;
    FmvWindowUpdate window_update_;
    int fmv_line_ = 0; // line of the field being drawn
    int fmv_first_active_line_ = 26, fmv_active_lines_ = 280;

    // Host interface: DMA routing, XFER FIFOs, MPEG RAM enable, pending resets.
    bool vcd_pixel_clock_ = false; // write-only register, 0xe01xxx
    int mpeg_ram_enabled_cnt_ = 0; // register writes, saturating at vmpeg::kMpegRamEnableWrites

    // Routes DMA and FMV_XFER bytes: set by vmpeg::kFmaCmdStartDma, cleared by kSysCmdStartDma.
    bool dma_for_fma_ = false;
    bool clear_fifo_pending_ = false;
    bool decoder_off_pending_ = false;
    bool fmv_dsp_enable_ = false; // gates the FMV demuxer and the time code/rate registers
    uint32_t fmv_decoder_timecode_ = 0;
    uint32_t fmv_display_timecode_ = 0;
    uint8_t fmv_display_video_status_ = 0;
    bool dma_active_ = false;
    uint8_t fmv_xfer_pending_high_ = 0; // latched high byte of an in-progress 16-bit XFER write
    // Demuxed elementary stream for the video decoder.
    std::vector<uint8_t> fmv_es_bytes_;
    // fma_dclk_ when the bytes from fmv_es_bytes_[offset] on arrived.
    struct EsArrival {
        uint32_t offset = 0;
        uint32_t time = 0;
    };
    std::vector<EsArrival> fmv_es_arrivals_;
    bool fmv_program_end_pending_ = false;
    int dclk_cycle_accum_ = 0; // 3 dclk ticks per 1000 CPU cycles
};

// The Digital Video Cartridge: the VMPEG register file with its demuxers and
// frame player, and the MPEG video and audio decoders behind it.
class Dvc : private FmaSaveState, private FmvSaveState {
public:
    struct DisplayWindow {
        uint16_t scrpos_x = 0, scrpos_y = 0;
        uint16_t decwin_w = 0, decwin_h = 0;
        uint16_t decoff_x = 0, decoff_y = 0;
        bool show = false;
        bool vcd_pixel_clock = false; // 13.5 MHz instead of 15 MHz
    };

    Dvc();

    // Configuration, not saved.
    void set_cartridge_present(bool present) { cartridge_present_ = present; }
    void set_video_standard(VideoStandard standard) { video_standard_ = standard; }

    // Registers, offset = addr - 0xe00000.
    uint8_t read_byte(uint32_t offset);
    void write_byte(uint32_t offset, uint8_t value);
    // The decode RAM at 0xe80000 shows once the firmware has enabled it.
    bool mpeg_ram_enabled() const { return mpeg_ram_enabled_cnt_ >= vmpeg::kMpegRamEnableWrites; }

    // Channel 1 DMA: a register write sets dma_active(); the board then feeds
    // the words and calls complete_dma() with how many it fed.
    bool dma_active() const { return dma_active_; }
    void feed_dma_word(uint16_t word);
    void complete_dma(uint32_t words_fed);

    void tick_cycles(int cpu_cycles);
    // After each line; `active_lines` follow `first_active_line`.
    void end_of_line(int line, int first_active_line, int active_lines);
    // Once per field, after its lines. True if Decoder Off dropped the shown picture.
    bool tick_field(double refresh_rate_hz);

    // Decoded audio since the last drain, interleaved stereo S16, through the cartridge's gains.
    std::vector<int16_t> drain_audio();

    bool irq_asserted() const;
    int iack_vector() const;

    // The shown picture, RGBA8888; frame_rgba() is valid only while has_frame().
    bool has_frame() const { return video_.has_frame(); }
    const uint8_t* frame_rgba() const { return video_.frame_rgba(); }
    int width() const { return video_.width(); }
    int height() const { return video_.height(); }
    DisplayWindow display_window() const;

    template <class T>
    void state_io(T& archive);

    // For debuggers (MachineInspector).
    const FmaSaveState& fma_state() const { return *this; }
    const FmvSaveState& fmv_state() const { return *this; }
    const DvcVideo& video() const { return video_; }

private:
    // dvc_registers.cpp: registers, host interface, FMV input.
    uint16_t read_word(uint32_t word_index) const;
    // Side effects run only when `completing` (the low byte of a 16-bit write).
    void write_word(uint32_t word_index, uint16_t value, bool completing);
    void handle_fmv_syscmd_write(uint16_t value);
    void handle_fmv_vidcmd_write(uint16_t value);
    void handle_fmv_gen_dec_cmd_write(uint16_t value);
    void note_bus_write_access();
    uint16_t fmv_display_rate() const {
        return video_standard_ == VideoStandard::kPal ? vmpeg::kDisplayRatePal : vmpeg::kDisplayRateNtsc;
    }
    uint16_t fmv_pictures_in_fifo() const;
    uint16_t fmv_dts_register() const { return uint16_t(fmv_demuxer_dts_ >> 7) & 0x7fff; }
    // Input FIFO level over kFmvInputFifoFullBytes.
    bool fmv_input_fifo_full() const;
    void fmv_demux_byte(uint8_t b);
    void fmv_es_byte(uint8_t b);
    // Per ES byte, so inline: a new mark only when the clock has moved.
    void note_fmv_es_arrival() {
        if (fmv_es_arrivals_.empty() || fmv_es_arrivals_.back().time != fma_dclk_) {
            fmv_es_arrivals_.push_back({uint32_t(fmv_es_bytes_.size()), fma_dclk_});
        }
    }
    void reset_fmv_demux();
    // What Clear FIFO and Decoder Off both reset.
    void reset_fmv_pipeline();

    // frame_player.cpp: picture timing, display events and the window.
    // One call per field.
    void notify_vsync();
    // `field_clocks` in 30 MHz clocks. True: show the next picture.
    bool frame_player_field(uint32_t field_clocks, size_t pictures_ready);
    void update_player_desync(uint32_t dclk, bool synchronous);
    bool video_display_enabled();
    void note_picture_displayed();
    // A decoded picture that won't be shown.
    void note_picture_discarded();
    // NDAT once per play, before the last picture's PIC.
    void check_buffer_underflow(size_t output_pictures, int decoder_pictures);
    // PIC, raised when vertical blanking ends (end_of_line).
    void set_video_frame_ready(int width, int height);
    void set_video_frame_decoded(int picture_rate_code);
    // IMGSZ: the picture shown next, once playing (mv_trigger() NIS timing).
    void set_next_picture(int width, int height, int picture_rate_code);
    void set_video_gop_start();
    void set_video_sequence_start();
    void set_video_sequence_end();
    void set_video_program_end();
    void set_video_last_picture();
    void set_video_decoder_timecode(uint32_t timecode);
    void set_video_display_timecode(uint32_t timecode);
    void set_video_display_status(uint8_t status);
    void set_video_display_picture_rate(int picture_rate_code);
    FmvWindow live_window() const;
    // Takes pending window updates at a picture selected for display, or none.
    void apply_window_updates(bool picture_selected);
    bool in_vertical_blank() const;
    bool window_scan_started() const;

    // fma.cpp: MPEG audio registers, DSP volumes, playback timer, underflow.
    void handle_fma_cmd_write(uint16_t value);
    void handle_fma_dspd_write(uint8_t dspd);
    void fma_input_byte(uint8_t b);
    void fma_demux_byte(uint8_t b);
    void reset_fma_playback_timer();
    // FMA status "decoding": set by the playback timer or, for audio
    // without timestamps, the first decoded samples.
    bool audio_dsp_enabled() const { return (fma_status_register_ & vmpeg::kFmaStatusDecoding) != 0; }
    // PS audio waiting for the playback timer to reach its first PTS.
    bool audio_start_pending() const { return fma_ps_seen_ && !audio_dsp_enabled(); }
    void set_audio_decoding_started();
    void set_audio_frame_decoded(uint32_t header);
    void set_audio_underflow();
    void set_audio_program_end();
    // Once per field: `decoded` sample frames were decoded, `field_samples`
    // played. Raises underflow when the field ends with everything played.
    void audio_field(int32_t decoded, int32_t field_samples);

    DvcVideo video_;
    DvcAudio audio_;
    // Configuration, not saved.
    bool cartridge_present_ = false;
    VideoStandard video_standard_ = VideoStandard::kNtsc;
    int32_t field_samples_ = 882; // 44.1 kHz samples per field, set by tick_field
};

} // namespace dvc
} // namespace zcdi

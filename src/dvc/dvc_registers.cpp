#include "dvc/dvc.h"

#include <algorithm>

namespace zcdi {
namespace dvc {

using namespace vmpeg;
using namespace mpeg;

namespace {

// 45 kHz dclk ticks from SYSCMD to playback starting or pausing.
enum DclkDelay : uint32_t {
    kPauseDelayTicks = 100, // ~2.2 ms
    kPlayStartDelayTicks = 1000, // ~22 ms, Play/Continue
    kScrLoadTicks = 15, // pack SCR into fmv_dclk unless the CPU writes GEN_SYSCR first
};

constexpr uint32_t kTimerCntMask = (1u << 21) - 1;

} // namespace

Dvc::Dvc() { fmv_dts_fifo_.reserve(kDtsFifoDepth); }

uint16_t Dvc::read_word(uint32_t word_index) const {
    switch (word_index) {
        case kFmaCmd: return fma_command_register_;
        case kFmaStatus: return (uint16_t(0x02) << 8) | fma_status_register_;
        case kFmaFixed04: return 0x0007;
        case kFmaFixed06: return 0x0900;
        case kFmaStreamW:
        case kFmaStreamR: return fma_stream_number_ & 0xf;
        case kFmaIvec: return fma_interrupt_vector_register_;
        case kFmaFixed0e: return 0x0042;
        case kFmaDclkH: return uint16_t(fma_dclk_ >> 16);
        case kFmaDclkL: return fma_dclkl_latch_;
        case kFmaAudioHdrH: return uint16_t(fma_mpeg_audio_header_ >> 16);
        case kFmaAudioHdrL: return uint16_t(fma_mpeg_audio_header_);
        case kFmaRun: return audio_dsp_enabled() ? 1 : 0;
        case kFmaIsr: return fma_interrupt_status_register_;
        case kFmaIer: return fma_interrupt_enable_register_;
        case kFmaDspd: return 0x0004;

        case kFmvImageWidth: return image_width_;
        case kFmvImageHeight: return image_height_;
        case kFmvImageRt: return image_rt_;
        case kFmvDecoderTimecodeHi: return uint16_t(fmv_decoder_timecode_ >> 16);
        case kFmvDecoderTimecodeLo: return uint16_t(fmv_decoder_timecode_);
        case kFmvDisplayW: return display_width_ & 0x7ff;
        case kFmvDisplayH: return display_height_ & 0x1ff;
        case kFmvPicRt: return display_frameperiod_rawhdr_;
        case kFmvDisplayTimecodeHi: return uint16_t(fmv_display_timecode_ >> 16);
        case kFmvDisplayTimecodeLo: return uint16_t(fmv_display_timecode_);
        case kFmvTempref: return fmv_display_video_status_; // SYS_VSR, displayed picture
        case kFmvSysSts: return fmv_input_fifo_full() ? 0 : kSysStsFifoNotFull;
        case kFmvIer: return fmv_interrupt_enable_register_;
        case kFmvIsr: return fmv_interrupt_status_register_;
        case kFmvTcnt: return fmv_timer_compare_register_;
        case kFmvVoff: return video_ctrl_y_offset_;
        case kFmvHoff: return video_ctrl_x_offset_;
        case kFmvVpix: return video_ctrl_y_active_;
        case kFmvHpix: return video_ctrl_x_active_;
        case kFmvScrposY: return video_ctrl_y_display_;
        case kFmvScrposX: return video_ctrl_x_display_;
        case kFmvDecwinH: return video_ctrl_window_height_;
        case kFmvDecwinW: return video_ctrl_window_width_;
        case kFmvDecoffY: return video_ctrl_decoder_offset_y_;
        case kFmvDecoffX: return video_ctrl_decoder_offset_x_;
        case kFmvGenDecCmd: return fmv_decoder_command_;
        case kFmvGenVdiCmd: return fmv_video_data_input_command_register_;
        case kFmvGenSyscr: return uint16_t(fmv_dclk_ >> 6) & 0x7fff; // bit 21 reads 0: the driver's SCR sign
        case kFmvGenSyncDiff: return 0;
        case kFmvGenDecDelay: return 1;
        case kFmvDts: return fmv_dts_register();
        case kFmvPicsInFifo: return fmv_pictures_in_fifo();
        case kFmvPicRate: // twice the display rate until a picture is decoded
            return decoder_frameperiod_90khz_ != 0 ? decoder_frameperiod_90khz_ : uint16_t(fmv_display_rate() << 1);
        case kFmvDisplayRate: return fmv_display_rate();
        case kFmvFrameRate: return 1; // always allows window updates; writes ignored
        case kFmvSyscmd: return fmv_system_command_register_;
        case kFmvVidcmd: return fmv_video_command_register_;
        case kFmvStreamNum: return fmv_stream_number_ & 0xf;
        case kFmvIvec: return fmv_interrupt_vector_register_;
        case kFmvMmuBase: return 0;

        default: return 0;
    }
}

void Dvc::handle_fmv_syscmd_write(uint16_t value) {
    fmv_system_command_register_ = value;

    if (value & kSysCmdPlay) {
        fmv_dclk_start_video_ = fma_dclk_ + kPlayStartDelayTicks;
        fmv_dclk_start_video_latched_ = true;
        fmv_decoder_command_ |= kDecCmdRunning;
        fmv_slow_motion_ = uint8_t(value & kSysCmdSlowMotion);
        fmv_decoder_active_ = true;
    }
    if (value & kSysCmdPause) {
        fmv_playback_active_ = false;
        fmv_dclk_start_video_latched_ = false;
        fmv_dclk_pause_video_ = fma_dclk_ + kPauseDelayTicks;
        fmv_dclk_pause_video_latched_ = true;
    }
    if (value & kSysCmdContinue) {
        fmv_dclk_start_video_ = fma_dclk_ + kPlayStartDelayTicks;
        fmv_dclk_start_video_latched_ = true;
        fmv_slow_motion_ = uint8_t(value & kSysCmdSlowMotion);
    }
    if (value & kSysCmdStep) {
        fmv_dclk_start_video_latched_ = false;
        player_step_latch_ = true;
    }
    if (value & kSysCmdStop) {
        fmv_playback_active_ = false;
        fmv_dclk_start_video_latched_ = false;
        fmv_decoder_active_ = false;
    }
    if (value & kSysCmdClearFifo) {
        reset_fmv_pipeline();
        clear_fifo_pending_ = true;
        fmv_dsp_enable_ = true; // drops for one clock on hardware, then back on
    }
    if (value & kSysCmdDecoderOn) {
        fmv_dsp_enable_ = true;
    }
    if (value & kSysCmdDecoderOff) {
        // The input FIFO, demuxer and frame queue are held in reset.
        reset_fmv_pipeline();
        fmv_dsp_enable_ = false;
        // Also drops the picture held for display.
        decoder_off_pending_ = true;
        fmv_es_has_sequence_header_ = false; // as DvcVideo::decoder_off()
        fmv_decoder_active_ = false;
        fmv_decoder_command_ &= ~kDecCmdRunning;
        image_width_ = 0;
        image_height_ = 0;
        image_rt_ = 0;
    }
    if (value & kSysCmdStartDma) {
        dma_for_fma_ = false;
        dma_active_ = true;
    }
}

void Dvc::reset_fmv_pipeline() {
    fmv_playback_active_ = false;
    fmv_dclk_start_video_latched_ = false;
    fmv_es_bytes_.clear();
    fmv_es_arrivals_.clear();
    fmv_program_end_pending_ = false;
    reset_fmv_demux();
    fmv_dts_fifo_.clear();
    fmv_pictures_received_ = 0;
    fmv_decoder_input_bytes_ = 0;
    player_desync_q_ = 0;
    player_desync_hold_ = 0;
    player_predicted_dts_ = 0;
    player_predicted_dts_valid_ = false;
    player_step_latch_ = false;
    player_latch_pending_ = false;
    player_playback_active_delayed_ = false;
    fmv_decoder_timecode_ = 0;
    fmv_display_timecode_ = 0;
    fmv_display_video_status_ = 0;
    decoder_frameperiod_90khz_ = 0;
    display_frameperiod_rawhdr_ = 0;
}

void Dvc::handle_fmv_vidcmd_write(uint16_t value) {
    fmv_video_command_register_ = value;

    if (value & kVidCmdRegsUpd) {
        // A scroll update in vertical blanking first applies a pending picture update.
        if ((value & kVidCmdScroll) && in_vertical_blank() && window_update_.next_pending) {
            latched_window_ = window_update_.next;
            window_update_.deferred_pending = false;
            window_update_.next_pending = false;
        }
        register_update_latch_ = true;
        register_update_scroll_ = (value & kVidCmdScroll) != 0;
        window_update_.late = fmv_playback_active_ && window_scan_started();
    }
    if (value & kVidCmdHide) fmv_show_on_next_video_frame_ = false;
    if (value & kVidCmdShow) fmv_show_on_next_video_frame_ = true;
    if (value & kVidCmdShowOnChange) fmv_show_on_next_video_frame_ = true;
}

void Dvc::handle_fmv_gen_dec_cmd_write(uint16_t value) {
    // A 0x22xx command outside pause steps one picture; how real hardware decides this is not known.
    fmv_decoder_command_ = value;
    if ((value >> 8) == 0x22 && !(fmv_system_command_register_ & kSysCmdPause)) player_step_latch_ = true;
}

void Dvc::note_bus_write_access() {
    if (mpeg_ram_enabled_cnt_ < kMpegRamEnableWrites) ++mpeg_ram_enabled_cnt_;
}

void Dvc::write_word(uint32_t word_index, uint16_t value, bool completing) {
    if (completing) note_bus_write_access();

    switch (word_index) {
        case kFmaCmd:
            fma_command_register_ = value;
            if (completing) handle_fma_cmd_write(value);
            break;
        case kFmaStreamW:
            fma_stream_number_ = uint8_t(value & 0xf);
            fma_stream_change_pending_ = true;
            break;
        case kFmaIvec: fma_interrupt_vector_register_ = value; break;
        case kFmaIer: fma_interrupt_enable_register_ = value; break;
        case kFmaDspa: fma_dspa_ = uint8_t(value & 0xff); break;
        case kFmaDspd:
            if (completing) handle_fma_dspd_write(uint8_t(value & 0xff));
            break;

        case kFmvIer: fmv_interrupt_enable_register_ = value; break;
        case kFmvIsr: break; // cleared by reading
        case kFmvXfer: {
            // High byte first. Bytes for the FMA are dropped: no driver sends any.
            if (!completing) {
                fmv_xfer_pending_high_ = uint8_t(value >> 8);
            } else if (!dma_for_fma_) {
                fmv_demux_byte(fmv_xfer_pending_high_);
                fmv_demux_byte(uint8_t(value));
            }
            break;
        }
        case kFmvTcnt: fmv_timer_compare_register_ = value; break;
        case kFmvTrld:
            if (completing) timer_cnt_ = 0;
            break;
        case kFmvSyscmd:
            if (completing)
                handle_fmv_syscmd_write(value);
            else
                fmv_system_command_register_ = value;
            break;
        case kFmvVidcmd:
            if (completing)
                handle_fmv_vidcmd_write(value);
            else
                fmv_video_command_register_ = value;
            break;
        case kFmvSysScr: fmv_system_control_register_ = value; break;
        case kFmvIvec: fmv_interrupt_vector_register_ = value; break;
        case kFmvVoff: video_ctrl_y_offset_ = value; break;
        case kFmvHoff: video_ctrl_x_offset_ = value; break;
        case kFmvVpix: video_ctrl_y_active_ = value; break;
        case kFmvHpix: video_ctrl_x_active_ = value; break;
        case kFmvScrposY: video_ctrl_y_display_ = value; break;
        case kFmvScrposX: video_ctrl_x_display_ = value; break;
        case kFmvDecwinH: video_ctrl_window_height_ = value; break;
        case kFmvDecwinW: video_ctrl_window_width_ = value; break;
        case kFmvDecoffY: video_ctrl_decoder_offset_y_ = value; break;
        case kFmvDecoffX: video_ctrl_decoder_offset_x_ = value; break;
        case kFmvGenDecCmd:
            if (completing)
                handle_fmv_gen_dec_cmd_write(value);
            else
                fmv_decoder_command_ = value;
            break;
        case kFmvGenVdiCmd: fmv_video_data_input_command_register_ = value; break;
        case kFmvGenSyscr:
            fmv_dclk_ = (fmv_dclk_ & ~uint32_t(0x7fff << 6)) | (uint32_t(value & 0x7fff) << 6);
            fmv_scr_load_countdown_ = 0; // the driver's value wins
            break;
        case kFmvImageWidth: image_width_ = value; break;
        case kFmvImageHeight: image_height_ = value; break;
        case kFmvImageRt: image_rt_ = value; break;
        case kFmvStreamNum: fmv_stream_number_ = uint8_t(value & 0xf); break;

        default: break;
    }

    if ((word_index & ~uint32_t(0xff)) == 0x0800) {
        // VCD register, 0xe01xxx.
        vcd_pixel_clock_ = (value & 1) != 0;
    }
}

uint8_t Dvc::read_byte(uint32_t offset) {
    // Only address bits 15..1 are decoded: the registers repeat every 64 KB.
    offset &= 0xffffu;
    uint32_t word_index = offset >> 1;
    uint16_t w = read_word(word_index);
    uint8_t out = (offset & 1) ? uint8_t(w) : uint8_t(w >> 8);
    // Clear-on-read ISRs, on the low byte, after `w` was read.
    bool completing = (offset & 1) != 0;
    if (completing) {
        if (word_index == kFmaIsr) fma_interrupt_status_register_ = 0;
        if (word_index == kFmvIsr) fmv_interrupt_status_register_ = 0;
        // Reading DCLK high latches the low word.
        if (word_index == kFmaDclkH) fma_dclkl_latch_ = uint16_t(fma_dclk_);
    }
    return out;
}

void Dvc::write_byte(uint32_t offset, uint8_t value) {
    offset &= 0xffffu;
    uint32_t word_index = offset >> 1;
    uint16_t current = read_word(word_index);
    bool completing = (offset & 1) != 0;
    uint16_t merged =
        completing ? uint16_t((current & 0xff00) | value) : uint16_t((current & 0x00ff) | (uint16_t(value) << 8));
    write_word(word_index, merged, completing);
}

void Dvc::tick_cycles(int cpu_cycles) {
    // 3 dclk ticks per 1000 CPU cycles: exactly 45 kHz at 15 MHz.
    dclk_cycle_accum_ += cpu_cycles * 3;
    while (dclk_cycle_accum_ >= 1000) {
        dclk_cycle_accum_ -= 1000;

        fma_dclk_ += 1;
        fmv_dclk_ += 1;
        // The demuxer sets the FMV DCLK too (45 kHz, so SCR / 2).
        if (fmv_scr_load_countdown_ > 0 && --fmv_scr_load_countdown_ == 0) fmv_dclk_ = uint32_t(fmv_demux_.scr >> 1);

        // Hardware compares for equality; a start time already past fires at once here.
        if (fma_start_valid_ && int32_t(fma_dclk_ - fma_start_dclk_) >= 0) {
            fma_start_valid_ = false;
            if (!audio_dsp_enabled()) set_audio_decoding_started();
        }

        if (fmv_dclk_start_video_latched_ && fmv_dclk_start_video_ == fma_dclk_) {
            fmv_dclk_start_video_latched_ = false;
            fmv_playback_active_ = true;
        }
        if (fmv_dclk_pause_video_latched_ && fmv_dclk_pause_video_ == fma_dclk_) {
            fmv_dclk_pause_video_latched_ = false;
            fmv_interrupt_status_register_ |= kIntPai;
        }

        // TIM: period (TCNT + 1) * 8 ticks, 448 at TCNT 55.
        if (timer_cnt_ >= (uint32_t(fmv_timer_compare_register_) << 3) + 7) {
            fmv_interrupt_status_register_ |= kIntTim;
            fma_interrupt_status_register_ |= kFmaIntTim;
            timer_cnt_ = 0;
        } else {
            timer_cnt_ = (timer_cnt_ + 1) & kTimerCntMask;
        }
    }
}

bool Dvc::irq_asserted() const {
    bool fmv_intreq = (fmv_interrupt_status_register_ & fmv_interrupt_enable_register_) != 0;
    bool fma_intreq = (fma_interrupt_status_register_ & fma_interrupt_enable_register_) != 0;
    return fmv_intreq || fma_intreq;
}

int Dvc::iack_vector() const {
    bool fma_intreq = (fma_interrupt_status_register_ & fma_interrupt_enable_register_) != 0;
    if (fma_intreq) return fma_interrupt_vector_register_ & 0xff;
    return (fmv_interrupt_vector_register_ >> 3) & 0xff;
}

bool Dvc::fmv_input_fifo_full() const {
    // Demuxed bytes still here count too: hardware writes them straight into the FIFO.
    return fmv_decoder_input_bytes_ + fmv_es_bytes_.size() > kFmvInputFifoFullBytes;
}

uint16_t Dvc::fmv_pictures_in_fifo() const {
    // Until Play, every picture received, as if nothing was decoded yet.
    if (!fmv_decoder_active_) return fmv_pictures_received_;
    const int count = int(fmv_dts_fifo_.size());
    return uint16_t(std::min(count > 1 ? count - 1 : count, 0x7f));
}

void Dvc::reset_fmv_demux() {
    fmv_demux_.reset();
    fmv_es_shift_ = 0xffffffff;
    fmv_next_picture_dts_ = 0;
    fmv_demux_scr_valid_ = false;
    fmv_demuxer_dts_ = 0;
}

void Dvc::fmv_es_byte(uint8_t b) {
    note_fmv_es_arrival();
    fmv_es_bytes_.push_back(b);
    fmv_es_shift_ = (fmv_es_shift_ << 8) | b;
    if ((fmv_es_shift_ & kStartCodePrefixMask) != kStartCodePrefix) return;
    switch (uint8_t(fmv_es_shift_)) {
        case kSequenceHeader: fmv_es_has_sequence_header_ = true; break;
        case kSequenceEnd: fmv_es_has_sequence_header_ = false; break; // the decoder waits for the next
        case kPictureStart:
            fmv_pictures_received_ = uint8_t((fmv_pictures_received_ + 1) & 0x7f);
            // Without a header the decoder skips it: its DTS goes with it.
            if (fmv_es_has_sequence_header_ && fmv_dts_fifo_.size() < kDtsFifoDepth) {
                fmv_dts_fifo_.push_back(fmv_next_picture_dts_);
            }
            fmv_next_picture_dts_ = 0;
            break;
        default: break;
    }
}

void Dvc::fmv_demux_byte(uint8_t b) {
    if (!fmv_dsp_enable_) return; // the demuxer is held in reset
    if (fmv_demux_mode_ == kDemuxModeElementary) {
        fmv_es_byte(b);
        return;
    }
    PsDemux& d = fmv_demux_;
    if (d.state == PsDemux::kScan) {
        if (!d.scan(b)) return;
        if (fmv_demux_mode_ == kDemuxModeUnknown) {
            if (b == kSequenceHeader) { // first: an elementary stream
                fmv_demux_mode_ = kDemuxModeElementary;
                fmv_es_shift_ = kStartCodePrefix | kSequenceHeader;
                fmv_es_has_sequence_header_ = true;
                note_fmv_es_arrival();
                fmv_es_bytes_.insert(fmv_es_bytes_.end(), {0x00, 0x00, 0x01, kSequenceHeader});
                return;
            }
            if (b != kPackStart) return;
            fmv_demux_mode_ = kDemuxModeProgram;
        }
        if (b == kProgramEnd) {
            set_video_program_end();
            fmv_program_end_pending_ = true;
        }
        d.begin(b);
        return;
    }
    const uint8_t events = d.step(b, uint8_t(kVideoStream + (fmv_stream_number_ & 0xf)));
    if (events & PsDemux::kScr) {
        fmv_demux_scr_valid_ = true;
        fmv_scr_load_countdown_ = kScrLoadTicks;
    }
    if (events & PsDemux::kLastTimestamp) {
        // The last 5 bytes: DTS if present, else PTS.
        fmv_demuxer_dts_ = d.timestamp();
        fmv_next_picture_dts_ = fmv_demuxer_dts_;
        fmv_video_data_input_command_register_ |= kVdiTimestampUpdated;
    }
    if (events & PsDemux::kPayloadByte) fmv_es_byte(b);
}

void Dvc::feed_dma_word(uint16_t word) {
    // High byte first.
    if (dma_for_fma_) {
        fma_input_byte(uint8_t(word >> 8));
        fma_input_byte(uint8_t(word));
    } else {
        fmv_demux_byte(uint8_t(word >> 8));
        fmv_demux_byte(uint8_t(word));
    }
}

void Dvc::complete_dma(uint32_t words_fed) {
    if (words_fed > 0 && dma_for_fma_) fma_dma_seen_ = true;
    // kSysCmdStartDma stays set.
    dma_active_ = false;
    dma_for_fma_ = false;
    fma_command_register_ &= ~kFmaCmdStartDma;
}

} // namespace dvc
} // namespace zcdi

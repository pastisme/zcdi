#include "vmpeg/vmpeg.h"

#include <algorithm>

#include "core/state_archive.h"

namespace zcdi {
namespace mamecdi {

using namespace vmpeg;

void PsDemux::reset() {
    state = kScan;
    shift = 0xffffffff;
    remaining = 0;
    ts_bytes = 0;
}

bool PsDemux::scan(uint8_t b) {
    shift = (shift << 8) | b;
    return (shift & kStartCodePrefixMask) == kStartCodePrefix;
}

void PsDemux::begin(uint8_t code) {
    shift = 0xffffffff;
    if (code == kPackStart) { // 5-byte SCR + 3-byte mux rate
        remaining = 8;
        state = kPack;
    } else if (code >= kSystemHeader) { // and PES packets
        stream = code;
        state = kLengthHigh;
    }
}

uint8_t PsDemux::step(uint8_t b, uint8_t wanted) {
    switch (state) {
        case kLengthHigh:
            remaining = uint16_t(b << 8);
            state = kLengthLow;
            return kNone;
        case kLengthLow:
            remaining |= b;
            state = remaining == 0 ? kScan : stream == wanted ? kPesHeader : kSkip;
            return kNone;
        default: break;
    }

    // Inside a packet: every byte counts against its length.
    const uint8_t current = state;
    if (--remaining == 0) state = kScan;
    const bool more = state != kScan;
    switch (current) {
        case kPack: {
            const int index = 7 - int(remaining); // remaining is 7 after the first byte
            if (index >= 5) return kNone; // mux rate
            ts_accum = (ts_accum << 8) | b;
            if (index != 4) return kNone;
            scr = timestamp();
            return kScr;
        }
        case kPesHeader:
            if (b == kPesStuffing) return kNone;
            if ((b & kPesStdBufferMask) == kPesStdBuffer) {
                if (more) state = kPesStd;
            } else if ((b & kPesPtsMask) == kPesPts) { // the PTS comes first
                has_dts = (b & kPesHasDts) != 0;
                ts_accum = b;
                ts_bytes = has_dts ? 9 : 4;
                if (more) state = kPesTimestamp;
            } else if (more) { // 0x0f: no timestamp
                state = kPayload;
            }
            return kNone;
        case kPesStd:
            if (more) state = kPesHeader;
            return kNone;
        case kPesTimestamp: {
            ts_accum = (ts_accum << 8) | b;
            --ts_bytes;
            uint8_t events = ts_bytes == (has_dts ? 5 : 0) ? kPts : kNone;
            if (ts_bytes == 0) {
                events |= kLastTimestamp;
                if (more) state = kPayload;
            }
            return events;
        }
        case kPayload: return kPayloadByte;
        default: // kSkip
            return kNone;
    }
}

int64_t PsDemux::timestamp() const {
    const uint64_t t = ts_accum & 0xffffffffffull;
    return int64_t(((t >> 33) & 0x7) << 30 | ((t >> 17) & 0x7fff) << 15 | ((t >> 1) & 0x7fff));
}

namespace {

// 45 kHz dclk ticks from SYSCMD to playback starting or pausing.
enum DclkDelay : uint32_t {
    kPauseDelayTicks = 100, // ~2.2 ms
    kPlayStartDelayTicks = 1000, // ~22 ms, Play/Continue
    kScrLoadTicks = 15, // pack SCR into fmv_dclk unless the CPU writes GEN_SYSCR first
};

// Pack SCR past the head picture's DTS by more than this (90 kHz): play at the shortest period.
constexpr int64_t kOutOfSyncThreshold = 15000;
// Pictures without a DTS keep the last desync for this many pictures.
constexpr uint8_t kDesyncHoldPictures = 3;

// dclk[20:6] - DTS[21:7], 15-bit signed.
int dts_desync(uint32_t dclk, int64_t dts) {
    int d = int((dclk >> 6) & 0x7fff) - int((uint64_t(dts) >> 7) & 0x7fff);
    d &= 0x7fff;
    return d >= 0x4000 ? d - 0x8000 : d;
}

constexpr uint32_t kTimerCntMask = (1u << 21) - 1;

// 10^(-db/20) * 256 * 9/16; >60 (incl. mute bit 7) is 0.
uint8_t db_to_linear(uint8_t db) {
    static constexpr uint8_t kTable[61] = {
        144, 128, 114, 102, 91, 81, 72, 64, 57, 51, 46, 41, 36, 32, 29, 26, 23, 20, 18, 16, 14,
        13,  11,  10,  9,   8,  7,  6,  6,  5,  5,  4,  4,  3,  3,  3,  2,  2,  2,  2,  1,  1,
        1,   1,   1,   1,   1,  1,  1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    };
    return db < 61 ? kTable[db] : 0;
}
} // namespace

VmpegDevice::VmpegDevice() { fmv_dts_fifo_.reserve(kDtsFifoDepth); }

template <class T>
void VmpegDevice::state_io(T& archive) {
    archive.begin_section(state_tag("VMPG"));
    archive.io_struct(static_cast<VmpegSaveState&>(*this));
    archive.end_section();
}
ZCDI_STATE_IO_INSTANTIATE(VmpegDevice);

uint16_t VmpegDevice::read_word(uint32_t word_index) const {
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

void VmpegDevice::handle_fmv_syscmd_write(uint16_t value) {
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

void VmpegDevice::reset_fmv_pipeline() {
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

void VmpegDevice::handle_fmv_vidcmd_write(uint16_t value) {
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

void VmpegDevice::handle_fmv_gen_dec_cmd_write(uint16_t value) {
    // A 0x22xx command outside pause steps one picture; how real hardware decides this is not known.
    fmv_decoder_command_ = value;
    if ((value >> 8) == 0x22 && !(fmv_system_command_register_ & kSysCmdPause)) player_step_latch_ = true;
}

void VmpegDevice::note_bus_write_access() {
    if (mpeg_ram_enabled_cnt_ < kMpegRamEnableWrites) ++mpeg_ram_enabled_cnt_;
}

void VmpegDevice::write_word(uint32_t word_index, uint16_t value, bool completing) {
    if (completing) note_bus_write_access();

    switch (word_index) {
        case kFmaCmd: {
            fma_command_register_ = value;
            if (!completing) break;
            if (value & kFmaCmdStartDma) {
                // Clears underflow (Green Book 8.2.4.3.3).
                fma_status_register_ &= ~kFmaStatusUnderflow;
                dma_for_fma_ = true;
                dma_active_ = true;
            }
            if (value & kFmaCmdStop) {
                fma_status_register_ = 0; // also ends decoding
                fma_es_bytes_.clear(); // input FIFO reset
                fma_sniff_bytes_.clear();
                fma_mpeg_audio_header_ = 0; // reset with it
                audio_reset_pending_ = true;
                fma_unplayed_samples_ = 0;
                reset_fma_playback_timer();
                fma_demux_mode_ = kDemuxModeUnknown;
                fma_demux_.reset();
                fma_ps_seen_ = false;
            }
            break;
        }
        case kFmaStreamW:
            fma_stream_number_ = uint8_t(value & 0xf);
            fma_stream_change_pending_ = true;
            break;
        case kFmaIvec: fma_interrupt_vector_register_ = value; break;
        case kFmaIer: fma_interrupt_enable_register_ = value; break;
        case kFmaDspa: fma_dspa_ = uint8_t(value & 0xff); break;
        case kFmaDspd: {
            if (!completing) break;
            uint8_t dspd = uint8_t(value & 0xff);
            const bool targeting_volume = (fma_dsp_mode_ == kDspModeVolume && fma_dsp_target_ == kDspTargetVolume);
            switch (fma_dspa_) {
                case 0: fma_dsp_mode_ = dspd; break;
                case 1:
                    fma_dsp_target_ = dspd;
                    // Strictly only when the target already held it; the same for well-formed sequences.
                    if (fma_dsp_mode_ == kDspModeVolume && dspd == kDspTargetVolume) fma_dsp_write_index_ = 0;
                    break;
                default: break;
            }
            if (targeting_volume && fma_dspa_ == kDspaVolume) {
                uint8_t linear = db_to_linear(dspd);
                switch (fma_dsp_write_index_) {
                    case 0: mpeg_volume_r2r_ = linear; break;
                    case 1: mpeg_volume_l2r_ = linear; break;
                    case 2: mpeg_volume_r2l_ = linear; break;
                    case 3: mpeg_volume_l2l_ = linear; break;
                    default: break;
                }
                fma_dsp_write_index_ = uint8_t((fma_dsp_write_index_ + 1) & 0x3);
            }
            break;
        }

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

uint8_t VmpegDevice::read_byte(uint32_t offset) {
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

void VmpegDevice::write_byte(uint32_t offset, uint8_t value) {
    offset &= 0xffffu;
    uint32_t word_index = offset >> 1;
    uint16_t current = read_word(word_index);
    bool completing = (offset & 1) != 0;
    uint16_t merged =
        completing ? uint16_t((current & 0xff00) | value) : uint16_t((current & 0x00ff) | (uint16_t(value) << 8));
    write_word(word_index, merged, completing);
}

void VmpegDevice::tick_cycles(int cpu_cycles) {
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

void VmpegDevice::notify_vsync() {
    // Each call is one vsync edge.
    fmv_interrupt_status_register_ |= kIntVsync;
    fmv_line_ = 0;
    ndat_line_pending_ = false;
    if (!register_update_latch_) return;

    // Every RegsUpd is taken at vsync, with VCUP and DCL.
    register_update_latch_ = false;
    fmv_interrupt_status_register_ |= kIntVcup | kIntDcl;
    if (register_update_scroll_) {
        latched_window_ = live_window();
        window_update_.deferred_pending = false;
        window_update_.next_pending = false;
        window_update_.next_late = false;
    } else { // waits for the next picture
        window_update_.next = live_window();
        window_update_.next_pending = true;
        window_update_.next_late = window_update_.late;
    }
}

void VmpegDevice::apply_window_updates(bool picture_selected) {
    // After the last picture, updates go on at vsync: no picture will take them.
    const bool held = !picture_selected && window_update_.last_picture_held;
    if (picture_selected || !fmv_decoder_active_) window_update_.last_picture_held = false;
    if (!picture_selected && !held) return;
    if (window_update_.deferred_pending) {
        latched_window_ = window_update_.deferred;
        window_update_.deferred_pending = false;
    }
    if (window_update_.next_pending) {
        // Written after the window started scanning out: keep it for the picture after.
        if (picture_selected && window_update_.next_late) {
            window_update_.deferred = window_update_.next;
            window_update_.deferred_pending = true;
        } else {
            latched_window_ = window_update_.next;
        }
        window_update_.next_pending = false;
    }
}

bool VmpegDevice::in_vertical_blank() const {
    return fmv_line_ < fmv_first_active_line_ || fmv_line_ >= fmv_first_active_line_ + fmv_active_lines_;
}

// The window's first line has been drawn this field.
bool VmpegDevice::window_scan_started() const {
    return fmv_show_on_next_video_frame_ && !in_vertical_blank() &&
           fmv_line_ >= fmv_first_active_line_ + int(latched_window_.display_offset_x);
}

void VmpegDevice::notify_line(int line, int first_active_line, int active_lines) {
    fmv_line_ = line + 1;
    fmv_first_active_line_ = first_active_line;
    fmv_active_lines_ = active_lines;
    if (ndat_line_pending_ && fmv_line_ == 11) {
        ndat_line_pending_ = false;
        fmv_interrupt_status_register_ |= kIntNdat;
    }
    // The picture starts display with the first active line.
    if (line + 1 == first_active_line && fmv_display_events_pending_ != 0) {
        fmv_interrupt_status_register_ |= fmv_display_events_pending_;
        fmv_display_events_pending_ = 0;
    }
}

bool VmpegDevice::irq_asserted() const {
    bool fmv_intreq = (fmv_interrupt_status_register_ & fmv_interrupt_enable_register_) != 0;
    bool fma_intreq = (fma_interrupt_status_register_ & fma_interrupt_enable_register_) != 0;
    return fmv_intreq || fma_intreq;
}

int VmpegDevice::iack_vector() const {
    bool fma_intreq = (fma_interrupt_status_register_ & fma_interrupt_enable_register_) != 0;
    if (fma_intreq) return fma_interrupt_vector_register_ & 0xff;
    return (fmv_interrupt_vector_register_ >> 3) & 0xff;
}

// SCRPOS X and Y are latched swapped, 9 bits each.
FmvWindow VmpegDevice::live_window() const {
    return {
        .display_offset_y = uint16_t(video_ctrl_x_display_ & 0x1ff),
        .display_offset_x = uint16_t(video_ctrl_y_display_ & 0x1ff),
        .window_offset_y = uint16_t(video_ctrl_decoder_offset_y_ & 0x1ff),
        .window_offset_x = uint16_t(video_ctrl_decoder_offset_x_ & 0x1ff),
        .width = uint16_t(video_ctrl_window_width_ & 0x1ff),
        .height = uint16_t(video_ctrl_window_height_ & 0x1ff),
    };
}

void VmpegDevice::set_next_picture(int width, int height, int picture_rate_code) {
    if (!fmv_decoder_active_) return;
    image_width_ = uint16_t(width);
    image_height_ = uint16_t(height);
    image_rt_ = uint16_t(picture_rate_code & 0xff);
}

void VmpegDevice::set_video_frame_ready(int width, int height) {
    fmv_display_events_pending_ |= kIntPic;
    display_width_ = uint16_t(width);
    display_height_ = uint16_t(height);
}

void VmpegDevice::set_video_frame_decoded(int picture_rate_code) {
    // 90000 / picture rate, indexed by the sequence header's picture_rate code.
    static constexpr uint16_t kPeriod90khz[16] = {
        100, 3753, 3750, 3600, 3003, 3000, 1800, 1501, 1500, 100, 100, 100, 100, 100, 100, 100,
    };
    // The same period in 30 MHz clocks.
    static constexpr uint32_t kPeriod30mhz[16] = {
        10000,  1251251, 1250000, 1200000, 1001001, 1000000, 600000, 500500,
        500000, 10000,   10000,   10000,   10000,   10000,   10000,  10000,
    };
    player_frame_period_ = kPeriod30mhz[picture_rate_code & 0xf];
    if (!fmv_dsp_enable_) return;
    decoder_frameperiod_90khz_ = kPeriod90khz[picture_rate_code & 0xf];
}

void VmpegDevice::check_buffer_underflow(size_t output_pictures, int decoder_pictures) {
    // The DTS FIFO holds input + decoder + output, so the input count includes the picture being decoded.
    const int input_pictures = int(fmv_dts_fifo_.size()) - int(output_pictures) - decoder_pictures;
    if (fmv_playback_active_ && ndat_armed_ && output_pictures <= 1 && decoder_pictures < 2 && input_pictures <= 1) {
        ndat_armed_ = false;
        ndat_line_pending_ = true;
    }
}

void VmpegDevice::set_video_gop_start() { fmv_display_events_pending_ |= kIntGop; }

void VmpegDevice::set_video_sequence_start() { fmv_display_events_pending_ |= kIntSeq; }

void VmpegDevice::set_video_sequence_end() { fmv_interrupt_status_register_ |= kIntEsi; }

void VmpegDevice::set_video_program_end() { fmv_interrupt_status_register_ |= kIntEii; }

void VmpegDevice::set_video_last_picture() {
    fmv_display_events_pending_ |= kIntEod;
    window_update_.last_picture_held = true;
}

void VmpegDevice::set_audio_program_end() {
    fma_interrupt_status_register_ |= kFmaIntIsoEnd;
    fma_status_register_ |= kFmaStatusFinished;
}

void VmpegDevice::set_audio_decoding_started() {
    fma_status_register_ |= kFmaStatusDecoding;
    fma_interrupt_status_register_ |= kFmaIntDecoding;
}

void VmpegDevice::set_audio_underflow() {
    fma_status_register_ |= kFmaStatusUnderflow;
    fma_interrupt_status_register_ |= kFmaIntUnf;
    fma_status_register_ &= ~kFmaStatusDecoding;
    reset_fma_playback_timer();
}

// UNF at the first field ending with the decoded audio played out.
void VmpegDevice::audio_field(int32_t decoded, int32_t field_samples) {
    const bool played_out = fma_unplayed_samples_ <= field_samples;
    fma_unplayed_samples_ = std::max(0, fma_unplayed_samples_ - field_samples) + decoded;
    if (audio_dsp_enabled() && decoded == 0 && played_out) {
        fma_unplayed_samples_ = 0;
        set_audio_underflow();
    }
}

void VmpegDevice::reset_fma_playback_timer() { fma_start_valid_ = false; }

void VmpegDevice::fma_input_byte(uint8_t b) {
    fma_demux_byte(b);
    if (fma_demux_mode_ == kDemuxModeElementary) {
        fma_es_bytes_.push_back(b);
    } else if (fma_demux_mode_ == kDemuxModeUnknown) {
        // No PACK within the first sectors: a bare audio elementary stream.
        fma_sniff_bytes_.push_back(b);
        if (fma_sniff_bytes_.size() >= kMaxFmaPsSniffBytes) {
            fma_demux_mode_ = kDemuxModeElementary;
            fma_es_bytes_.insert(fma_es_bytes_.end(), fma_sniff_bytes_.begin(), fma_sniff_bytes_.end());
            fma_sniff_bytes_.clear();
        }
    }
}

void VmpegDevice::fma_demux_byte(uint8_t b) {
    PsDemux& d = fma_demux_;
    if (d.state == PsDemux::kScan) {
        if (!d.scan(b)) return;
        if (b == kPackStart) {
            fma_ps_seen_ = true;
            if (fma_demux_mode_ == kDemuxModeUnknown) {
                fma_demux_mode_ = kDemuxModeProgram;
                fma_sniff_bytes_.clear();
            }
        } else if (b == kProgramEnd) {
            if (fma_demux_mode_ == kDemuxModeProgram) set_audio_program_end();
        }
        d.begin(b);
        return;
    }
    const uint8_t events = d.step(b, uint8_t(kAudioStream + (fma_stream_number_ & 0xf)));
    if ((events & PsDemux::kPts) && !fma_start_valid_) {
        fma_start_dclk_ = fma_dclk_ + uint32_t(d.timestamp() >> 1) - uint32_t(d.scr >> 1);
        fma_start_valid_ = true;
    }
    if ((events & PsDemux::kPayloadByte) && fma_demux_mode_ == kDemuxModeProgram) fma_es_bytes_.push_back(b);
}

void VmpegDevice::set_audio_frame_decoded(uint32_t header) {
    fma_status_register_ |= kFmaStatusFrame;
    fma_interrupt_status_register_ |= kFmaIntFrame;
    // CSU follows the pending flag, so an unread one clears.
    fma_interrupt_status_register_ = (fma_interrupt_status_register_ & ~unsigned(kFmaIntCsu)) |
                                     (fma_stream_change_pending_ ? unsigned(kFmaIntCsu) : 0u);
    fma_stream_change_pending_ = false;
    if (header != 0) fma_mpeg_audio_header_ = header;
}

void VmpegDevice::set_video_decoder_timecode(uint32_t timecode) {
    if (fmv_dsp_enable_) fmv_decoder_timecode_ = timecode;
}

void VmpegDevice::set_video_display_timecode(uint32_t timecode) {
    if (fmv_dsp_enable_) fmv_display_timecode_ = timecode;
}

void VmpegDevice::set_video_display_status(uint8_t status) {
    if (fmv_dsp_enable_) fmv_display_video_status_ = status;
}

void VmpegDevice::set_video_display_picture_rate(int picture_rate_code) {
    if (fmv_dsp_enable_) display_frameperiod_rawhdr_ = uint8_t(picture_rate_code & 0xff);
}

bool VmpegDevice::fmv_input_fifo_full() const {
    // Demuxed bytes still here count too: hardware writes them straight into the FIFO.
    return fmv_decoder_input_bytes_ + fmv_es_bytes_.size() > kFmvInputFifoFullBytes;
}

uint16_t VmpegDevice::fmv_pictures_in_fifo() const {
    // Until Play, every picture received, as if nothing was decoded yet.
    if (!fmv_decoder_active_) return fmv_pictures_received_;
    const int count = int(fmv_dts_fifo_.size());
    return uint16_t(std::min(count > 1 ? count - 1 : count, 0x7f));
}

bool VmpegDevice::video_display_enabled() {
    if (!fmv_playback_active_) {
        fmv_dts_start_satisfied_ = false;
        return false;
    }
    if (!fmv_dts_start_satisfied_) {
        bool head_has_dts = !fmv_dts_fifo_.empty() && fmv_dts_fifo_.front() != 0;
        if (!head_has_dts || dts_desync(fmv_dclk_, fmv_dts_fifo_.front()) > -30) fmv_dts_start_satisfied_ = true;
    }
    bool synchronous = (fmv_system_control_register_ & kSysScrSynchronous) != 0;
    return !synchronous || fmv_dts_start_satisfied_;
}

void VmpegDevice::note_picture_displayed() {
    if (fmv_dts_fifo_.empty()) return;
    // Predicted DTS: the last DTS plus a picture period per picture after it.
    const int64_t head = fmv_dts_fifo_.front();
    if (head != 0 && decoder_frameperiod_90khz_ != 0) {
        player_predicted_dts_ = head + decoder_frameperiod_90khz_;
        player_predicted_dts_valid_ = true;
    } else if (player_predicted_dts_valid_ && decoder_frameperiod_90khz_ != 0) {
        player_predicted_dts_ += decoder_frameperiod_90khz_;
    } else {
        player_predicted_dts_valid_ = false;
    }
    fmv_dts_fifo_.erase(fmv_dts_fifo_.begin());
}

// Its DTS leaves the DTS FIFO like a shown picture's.
void VmpegDevice::note_picture_discarded() { note_picture_displayed(); }

VmpegDevice::FramePlayerStep VmpegDevice::frame_player_field(uint32_t field_clocks, size_t pictures_ready) {
    FramePlayerStep step;
    const bool counting = video_display_enabled(); // also keeps the start latch
    // Step: armed at one vsync, shown at the next, playing or not.
    auto arm_step = [&] {
        if (player_step_latch_ && pictures_ready > size_t(step.show)) {
            player_step_latch_ = false;
            player_latch_pending_ = true;
        }
    };
    // Playback starting arms NDAT; nothing disarms it but NDAT.
    if (fmv_playback_active_ && !player_playback_active_delayed_) ndat_armed_ = true;
    if (!fmv_playback_active_) {
        player_frame_cnt_ = 0;
        step.show = player_latch_pending_ && pictures_ready > 0;
        player_latch_pending_ = false;
        apply_window_updates(step.show);
        arm_step();
        return step;
    }
    const bool synchronous = (fmv_system_control_register_ & kSysScrSynchronous) != 0;
    const bool head_has_dts = !fmv_dts_fifo_.empty() && fmv_dts_fifo_.front() != 0;
    // Far out of sync: only forces the shortest period.
    const bool out_of_sync =
        head_has_dts && fmv_demux_scr_valid_ && fmv_demux_.scr - fmv_dts_fifo_.front() > kOutOfSyncThreshold;
    if (counting) {
        player_frame_cnt_ += field_clocks;
        for (;;) {
            // Slow motion multiplies the period.
            const int64_t period = int64_t(player_frame_period_) * (fmv_slow_motion_ + 1);
            // Speed control stays within 1/8 of the period, as measured on a VMPEG.
            const int64_t shortest = period - period / 8, longest = period + period / 8;
            int64_t top = period - 1 - int64_t(player_desync_q_) * 2048;
            if (top < shortest || out_of_sync) top = shortest;
            if (top > longest) top = longest;
            if (top < 1) top = 1;
            if (player_frame_cnt_ < top) break;
            player_frame_cnt_ -= top + 1;
            if (period <= 1200) continue; // no picture latched at periods this short
            player_latch_pending_ = true;
            player_playback_active_delayed_ = true;
            if (pictures_ready == 0) continue; // desync is measured only with a picture waiting
            // dclk at the wrap, player_frame_cnt_ 30 MHz clocks before the field end.
            const uint32_t dclk_at_wrap = fmv_dclk_ - uint32_t(player_frame_cnt_ * 3 / 2000);
            update_player_desync(dclk_at_wrap, synchronous);
        }
    }
    if (player_latch_pending_) {
        player_latch_pending_ = false;
        step.show = pictures_ready > 0;
    }
    apply_window_updates(step.show);
    arm_step();
    return step;
}

// Desync at a picture period wrap, against the head's DTS
// or, without one, the predicted DTS; a measurement holds for 3 more pictures.
void VmpegDevice::update_player_desync(uint32_t dclk, bool synchronous) {
    const bool head_has_dts = !fmv_dts_fifo_.empty() && fmv_dts_fifo_.front() != 0;
    const bool use_predicted = !fmv_dts_fifo_.empty() && !head_has_dts && player_predicted_dts_valid_;
    const bool syncing = synchronous && fmv_slow_motion_ == 0; // no sync in slow motion
    if (syncing && (head_has_dts || use_predicted)) {
        player_desync_q_ = dts_desync(dclk, head_has_dts ? fmv_dts_fifo_.front() : player_predicted_dts_);
        player_desync_hold_ = kDesyncHoldPictures;
    } else if (syncing && player_desync_hold_ > 0) {
        --player_desync_hold_;
    } else {
        player_desync_q_ = 0;
    }
}

void VmpegDevice::reset_fmv_demux() {
    fmv_demux_.reset();
    fmv_es_shift_ = 0xffffffff;
    fmv_next_picture_dts_ = 0;
    fmv_demux_scr_valid_ = false;
    fmv_demuxer_dts_ = 0;
}

void VmpegDevice::fmv_es_byte(uint8_t b) {
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

void VmpegDevice::fmv_demux_byte(uint8_t b) {
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

VmpegDevice::DisplayWindow VmpegDevice::display_window() const {
    // display_offset_y holds X.
    const FmvWindow& w = latched_window_;
    return {
        .scrpos_x = w.display_offset_y,
        .scrpos_y = w.display_offset_x,
        .decwin_w = w.width,
        .decwin_h = w.height,
        .decoff_x = w.window_offset_x,
        .decoff_y = w.window_offset_y,
        .show = fmv_show_on_next_video_frame_,
        .vcd_pixel_clock = vcd_pixel_clock_,
    };
}

std::vector<uint8_t> VmpegDevice::drain_fmv_es_bytes() { return std::exchange(fmv_es_bytes_, {}); }

std::vector<uint8_t> VmpegDevice::drain_fma_es_bytes() { return std::exchange(fma_es_bytes_, {}); }

void VmpegDevice::feed_dma_word(uint16_t word) {
    // High byte first.
    if (dma_for_fma_) {
        fma_input_byte(uint8_t(word >> 8));
        fma_input_byte(uint8_t(word));
    } else {
        fmv_demux_byte(uint8_t(word >> 8));
        fmv_demux_byte(uint8_t(word));
    }
}

void VmpegDevice::complete_dma() {
    // kSysCmdStartDma stays set.
    dma_active_ = false;
    dma_for_fma_ = false;
    fma_command_register_ &= ~kFmaCmdStartDma;
}

} // namespace mamecdi
} // namespace zcdi

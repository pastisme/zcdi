#include "vmpeg/vmpeg.h"

#include <algorithm>

#include "core/state_archive.h"

namespace zcdi {
namespace mamecdi {

void PsDemux::reset() {
    state = kScan;
    shift = 0xffffffff;
    remaining = 0;
    ts_bytes = 0;
}

bool PsDemux::scan(uint8_t b) {
    shift = (shift << 8) | b;
    return (shift & 0xffffff00) == 0x00000100;
}

void PsDemux::begin(uint8_t code) {
    shift = 0xffffffff;
    if (code == 0xba) { // MPEG-1 pack header: 5-byte SCR + 3-byte mux rate
        remaining = 8;
        state = kPack;
    } else if (code >= 0xbb) { // system header and PES packets: 16-bit length
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
            if (b == 0xff) return kNone; // stuffing
            if ((b & 0xc0) == 0x40) { // P-STD buffer size, 2 bytes
                if (more) state = kPesStd;
            } else if ((b & 0xe0) == 0x20) { // 0010 PTS or 0011 PTS + DTS: the PTS comes first
                has_dts = (b & 0x10) != 0;
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
// Word indices, vmpeg.sv `address[15:1]`.
constexpr uint32_t kFmaCmd = 0x1800; // 0xe03000
constexpr uint32_t kFmaStatus = 0x1801; // 0xe03002
constexpr uint32_t kFmaStreamW = 0x1804; // 0xe03008 (write: wanted stream)
constexpr uint32_t kFmaStreamR = 0x1805; // 0xe0300a (read: current stream, same reg here)
constexpr uint32_t kFmaIvec = 0x1806; // 0xe0300c
constexpr uint32_t kFmaDclkH = 0x1808; // 0xe03010
constexpr uint32_t kFmaDclkL = 0x1809; // 0xe03012
constexpr uint32_t kFmaAudioHdrH = 0x180a; // 0xe03014
constexpr uint32_t kFmaAudioHdrL = 0x180b; // 0xe03016
constexpr uint32_t kFmaRun = 0x180c; // 0xe03018
constexpr uint32_t kFmaIsr = 0x180d; // 0xe0301a
constexpr uint32_t kFmaIer = 0x180e; // 0xe0301c
constexpr uint32_t kFmaDspa = 0x1811; // 0xe03022
constexpr uint32_t kFmaDspd = 0x1812; // 0xe03024

constexpr uint32_t kFmvImageWidth = 0x2001; // 0xe04002
constexpr uint32_t kFmvImageHeight = 0x2002; // 0xe04004
constexpr uint32_t kFmvImageRt = 0x2003; // 0xe04006
constexpr uint32_t kFmvTimecodeHiA = 0x2004; // 0xe04008
constexpr uint32_t kFmvTimecodeLoA = 0x2005; // 0xe0400c
constexpr uint32_t kFmvDisplayW = 0x2029; // 0xe04052
constexpr uint32_t kFmvDisplayH = 0x202a; // 0xe04054
constexpr uint32_t kFmvPicRt = 0x202b; // 0xe04056
constexpr uint32_t kFmvTimecodeHiB = 0x202c; // 0xe04058
constexpr uint32_t kFmvTimecodeLoB = 0x202d; // 0xe0405a
constexpr uint32_t kFmvTempref = 0x202e; // 0xe0405c
constexpr uint32_t kFmvSysSts = 0x202f; // 0xe0405e
constexpr uint32_t kFmvIer = 0x2030; // 0xe04060
constexpr uint32_t kFmvIsr = 0x2031; // 0xe04062
constexpr uint32_t kFmvTcnt = 0x2032; // 0xe04064
constexpr uint32_t kFmvTrld = 0x2057; // 0xe040ae (write-only reload)
constexpr uint32_t kFmvVoff = 0x2036; // 0xe0406c
constexpr uint32_t kFmvHoff = 0x2037; // 0xe0406e
constexpr uint32_t kFmvVpix = 0x2038; // 0xe04070
constexpr uint32_t kFmvHpix = 0x2039; // 0xe04072
constexpr uint32_t kFmvScrposY = 0x203a; // 0xe04074
constexpr uint32_t kFmvScrposX = 0x203b; // 0xe04076
constexpr uint32_t kFmvDecwinH = 0x203c; // 0xe04078
constexpr uint32_t kFmvDecwinW = 0x203d; // 0xe0407a
constexpr uint32_t kFmvDecoffY = 0x203e; // 0xe0407c
constexpr uint32_t kFmvDecoffX = 0x203f; // 0xe0407e
constexpr uint32_t kFmvGenDecCmd = 0x2044; // 0xe04088
constexpr uint32_t kFmvGenVdiCmd = 0x2046; // 0xe0408c
constexpr uint32_t kFmvGenSyscr = 0x204c; // 0xe04098
constexpr uint32_t kFmvGenSyncDiff = 0x204e; // 0xe0409c
constexpr uint32_t kFmvGenDecDelay = 0x204f; // 0xe0409e
constexpr uint32_t kFmvDts = 0x2050; // 0xe040a0
constexpr uint32_t kFmvPicsInFifo = 0x2052; // 0xe040a4
constexpr uint32_t kFmvPicRate = 0x2054; // 0xe040a8
constexpr uint32_t kFmvDisplayRate = 0x2055; // 0xe040aa
constexpr uint32_t kFmvFrameRate = 0x2056; // 0xe040ac
constexpr uint32_t kFmvSyscmd = 0x2060; // 0xe040c0
constexpr uint32_t kFmvVidcmd = 0x2061; // 0xe040c2
constexpr uint32_t kFmvStreamNum = 0x2062; // 0xe040c4
constexpr uint32_t kFmvSysScr = 0x2063; // 0xe040c6
constexpr uint32_t kFmvXfer = 0x206f; // 0xe040de
constexpr uint32_t kFmvIvec = 0x206e; // 0xe040dc
constexpr uint32_t kFmvMmuBase = 0x2073; // 0xe040e6

// vmpeg.sv:410-411
constexpr uint16_t kDisplayRatePal = 0x0708;
constexpr uint16_t kDisplayRateNtsc = 0x05dc;

// 45 kHz dclk ticks, ~22 ms (vmpeg.sv SYSCMD Play/Continue).
constexpr uint32_t kPlayStartDelayTicks = 1000;

// mpeg_video.sv totally_out_of_sync_need_frameskip, 90 kHz.
constexpr int64_t kFrameSkipThreshold = 15000;
constexpr int kMaxSkipsPerBlanking = 20;

// mpeg_video.sv display_dts_desync: dclk[20:6] - DTS[21:7], 15-bit signed.
int dts_desync(uint32_t dclk, int64_t dts) {
    int d = int((dclk >> 6) & 0x7fff) - int((uint64_t(dts) >> 7) & 0x7fff);
    d &= 0x7fff;
    return d >= 0x4000 ? d - 0x8000 : d;
}
constexpr uint32_t kPauseDelayTicks = 100; // ~2.2 ms, vmpeg.sv:894-895

// vmpeg.sv:531/698
constexpr uint32_t kTimerCntMask = (1u << 21) - 1;

// dsp_registers.sv db_to_linear: 10^(-db/20) * 256 * 9/16; >60 (incl. mute bit 7) is 0.
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
        case 0x1802: return 0x0007; // vmpeg.sv:458
        case 0x1803: return 0x0900; // vmpeg.sv:459
        case kFmaStreamW:
        case kFmaStreamR: return fma_stream_number_ & 0xf;
        case kFmaIvec: return fma_interrupt_vector_register_;
        case 0x1807: return 0x0042; // vmpeg.sv:463
        case kFmaDclkH: return uint16_t(fma_dclk_ >> 16);
        case kFmaDclkL: return fma_dclkl_latch_;
        case kFmaAudioHdrH: return uint16_t(fma_mpeg_audio_header_ >> 16); // vmpeg.sv:517
        case kFmaAudioHdrL: return uint16_t(fma_mpeg_audio_header_);
        case kFmaRun: return audio_dsp_enabled() ? 1 : 0;
        case kFmaIsr: return fma_interrupt_status_register_;
        case kFmaIer: return fma_interrupt_enable_register_;
        case kFmaDspd: return 0x0004; // vmpeg.sv:471

        case kFmvImageWidth: return image_width_;
        case kFmvImageHeight: return image_height_;
        case kFmvImageRt: return image_rt_;
        case kFmvTimecodeHiA: return uint16_t(fmv_display_timecode_ >> 16);
        case kFmvTimecodeLoA: return uint16_t(fmv_display_timecode_);
        case kFmvTimecodeHiB: return uint16_t(fmv_decoder_timecode_ >> 16);
        case kFmvTimecodeLoB: return uint16_t(fmv_decoder_timecode_);
        case kFmvDisplayW: return display_width_ & 0x7ff; // vmpeg.sv:529
        case kFmvDisplayH: return display_height_ & 0x1ff;
        case kFmvPicRt: return decoder_frameperiod_rawhdr_;
        case kFmvTempref: return fmv_display_video_status_; // SYS_VSR, displayed picture
        case kFmvSysSts: return fmv_input_fifo_full() ? 0 : 0x2000; // vmpeg.sv:523
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
        case kFmvGenSyscr: return uint16_t(fmv_dclk_ >> 6) & 0xffff;
        case kFmvGenSyncDiff: return 0; // vmpeg.sv:501
        case kFmvGenDecDelay: return 1; // older RTL revisions: 0xfe96
        case kFmvDts: return fmv_dts_register();
        case kFmvPicsInFifo: return fmv_pictures_in_fifo();
        case kFmvPicRate: return decoder_frameperiod_90khz_;
        case kFmvDisplayRate: return video_standard_ == VideoStandard::kPal ? kDisplayRatePal : kDisplayRateNtsc;
        case kFmvFrameRate: return fmv_frame_rate_;
        case kFmvSyscmd: return fmv_system_command_register_;
        case kFmvVidcmd: return fmv_video_command_register_;
        case kFmvStreamNum: return fmv_stream_number_ & 0xf;
        case kFmvIvec: return fmv_interrupt_vector_register_;
        case kFmvMmuBase: return 0; // vmpeg.sv:513

        default: return 0;
    }
}

void VmpegDevice::handle_fmv_syscmd_write(uint16_t value) {
    // vmpeg.sv:836-949
    fmv_system_command_register_ = value;

    if (value & 0x0008) { // Play
        fmv_dclk_start_video_ = fma_dclk_ + kPlayStartDelayTicks;
        fmv_dclk_start_video_latched_ = true;
        fmv_decoder_command_ |= 0x0042;
        fmv_slow_motion_ = uint8_t(value & 7);
    }
    if (value & 0x0010) { // Pause
        fmv_playback_active_ = false;
        fmv_dclk_start_video_latched_ = false;
        fmv_dclk_pause_video_ = fma_dclk_ + kPauseDelayTicks;
        fmv_dclk_pause_video_latched_ = true;
    }
    if (value & 0x0020) { // Continue
        fmv_dclk_start_video_ = fma_dclk_ + kPlayStartDelayTicks;
        fmv_dclk_start_video_latched_ = true;
        fmv_slow_motion_ = uint8_t(value & 7);
    }
    if (value & 0x0040) { // Step
        fmv_dclk_start_video_latched_ = false;
        player_step_latch_ = true;
    }
    if (value & 0x0080) { // Stop
        fmv_playback_active_ = false;
        fmv_dclk_start_video_latched_ = false;
    }
    if (value & 0x0100) { // Clear FIFO
        reset_fmv_pipeline();
        clear_fifo_pending_ = true;
        // fmv_dsp_enable drops for a cycle, then restart_fmv_dsp_enable.
        fmv_dsp_enable_ = true;
    }
    if (value & 0x1000) { // Decoder on
        fmv_dsp_enable_ = true;
    }
    if (value & 0x2000) { // Decoder off
        // The input FIFO, demuxer and frame queue are held in reset.
        reset_fmv_pipeline();
        fmv_dsp_enable_ = false;
        // fmv_reset_persistent_storage drops the latched picture (frameplayer.sv:44).
        decoder_off_pending_ = true;
        image_width_ = 0;
        image_height_ = 0;
        image_rt_ = 0;
    }
    if (value & 0x8000) { // Start DMA, to video (vmpeg.sv:944-946)
        dma_for_fma_ = false;
        dma_active_ = true;
    }
}

void VmpegDevice::reset_fmv_pipeline() {
    fmv_playback_active_ = false;
    fmv_dclk_start_video_latched_ = false;
    fmv_es_bytes_.clear();
    fmv_program_end_pending_ = false;
    reset_fmv_demux();
    fmv_dts_fifo_.clear();
    fmv_decoder_held_pictures_ = 0;
    fmv_decoder_input_bytes_ = 0;
    player_desync_q_ = 0;
    player_step_latch_ = false;
    player_latch_pending_ = false;
    fmv_decoder_timecode_ = 0;
    fmv_display_timecode_ = 0;
    fmv_display_video_status_ = 0;
    decoder_frameperiod_90khz_ = 0;
    decoder_frameperiod_rawhdr_ = 0;
}

void VmpegDevice::handle_fmv_vidcmd_write(uint16_t value) {
    // vmpeg.sv:950-981
    fmv_video_command_register_ = value;

    if (value & 0x0008) { // RegsUpd
        register_update_latch_ = true;
        register_update_scroll_ = (value & 0x0004) != 0;
    }
    if (value & 0x0100) fmv_show_on_next_video_frame_ = false; // Hide
    if (value & 0x0200) fmv_show_on_next_video_frame_ = true; // Show
    if (value & 0x0400) fmv_show_on_next_video_frame_ = true; // Show on next picture change
}

void VmpegDevice::handle_fmv_gen_dec_cmd_write(uint16_t value) {
    // vmpeg.sv:1034-1042, ported as-is (the RTL marks it "very unsure about this").
    fmv_decoder_command_ = value;
    if ((value >> 8) == 0x22 && !(fmv_system_command_register_ & 0x0010)) player_step_latch_ = true;
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
            if (value & 0x8000) {
                // Start DMA, to audio; clears underflow (Green Book 8.2.4.3.3, vmpeg.sv:772-775).
                fma_status_register_ &= ~uint8_t(0x08);
                dma_for_fma_ = true; // vmpeg.sv:769-770
                dma_active_ = true;
            }
            if (value & 0x0001) {
                // Stop
                fma_status_register_ = 0; // also ends decoding
                fma_es_bytes_.clear(); // input FIFO reset, vmpeg.sv:852
                fma_sniff_bytes_.clear();
                fma_mpeg_audio_header_ = 0; // reset with it, mpeg_audio.sv:63-66
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
            // dsp_registers.sv: mode 0x80, target 0x93, then four dspa=7 gains.
            if (!completing) break;
            uint8_t dspd = uint8_t(value & 0xff);
            const bool targeting_volume = (fma_dsp_mode_ == 0x80 && fma_dsp_target_ == 0x93);
            switch (fma_dspa_) {
                case 0: fma_dsp_mode_ = dspd; break;
                case 1:
                    fma_dsp_target_ = dspd;
                    // The RTL resets only if target already held 0x93; same for well-formed sequences.
                    if (fma_dsp_mode_ == 0x80 && dspd == 0x93) fma_dsp_write_index_ = 0;
                    break;
                default: break;
            }
            if (targeting_volume && fma_dspa_ == 7) {
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
        case kFmvIsr: break; // vmpeg.sv:824-826
        case kFmvXfer: {
            // High byte first (vmpeg.sv:55). Bytes for the FMA are dropped: no driver sends any.
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
        case kFmvGenSyscr: fmv_dclk_ = (fmv_dclk_ & ~uint32_t(0xffff << 6)) | (uint32_t(value) << 6); break;
        case kFmvFrameRate: fmv_frame_rate_ = value; break;
        case kFmvImageWidth: image_width_ = value; break;
        case kFmvImageHeight: image_height_ = value; break;
        case kFmvImageRt: image_rt_ = value; break;
        case kFmvStreamNum: fmv_stream_number_ = uint8_t(value & 0xf); break;

        default: break;
    }

    if ((word_index & ~uint32_t(0xff)) == 0x0800) {
        // VCD register, 0xe01xxx (vmpeg.sv:750-754).
        vcd_pixel_clock_ = (value & 1) != 0;
    }
}

uint8_t VmpegDevice::read_byte(uint32_t offset) {
    // Only address bits 15..1 are decoded: the registers repeat every 64 KB.
    offset &= 0xffffu;
    uint32_t word_index = offset >> 1;
    uint16_t w = read_word(word_index);
    uint8_t out = (offset & 1) ? uint8_t(w) : uint8_t(w >> 8);
    // Clear-on-read ISRs (vmpeg.sv:726-735), on the low byte, after `w` was read.
    bool completing = (offset & 1) != 0;
    if (completing) {
        if (word_index == kFmaIsr) fma_interrupt_status_register_ = 0;
        if (word_index == kFmvIsr) fmv_interrupt_status_register_ = 0;
        // vmpeg.sv:809-810: reading DCLK high latches the low word.
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

        // mpeg_playback_timer.sv compares for equality; a past start time fires at once.
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
    // Each call is one vsync edge (vmpeg.sv:608).
    fmv_interrupt_status_register_ |= kIntVsync;

    // vmpeg.sv:678: scroll-mode RegsUpd.
    if (register_update_latch_ && register_update_scroll_) commit_register_update();
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

void VmpegDevice::commit_register_update() {
    // vmpeg.sv:678-694. VCUP always comes with DCL.
    fmv_interrupt_status_register_ |= (kIntVcup | kIntDcl);
    register_update_latch_ = false;

    // X and Y swapped as in the RTL (vmpeg.sv:160-165).
    latched_display_offset_y_ = video_ctrl_x_display_ & 0x1ff;
    latched_display_offset_x_ = video_ctrl_y_display_ & 0x1ff;
    latched_window_offset_y_ = video_ctrl_decoder_offset_y_ & 0x1ff;
    latched_window_offset_x_ = video_ctrl_decoder_offset_x_ & 0x1ff;
    latched_window_width_ = video_ctrl_window_width_ & 0x1ff;
    latched_window_height_ = video_ctrl_window_height_ & 0x1ff;
}

void VmpegDevice::set_video_frame_ready(int width, int height) {
    fmv_interrupt_status_register_ |= kIntPic;
    display_width_ = uint16_t(width);
    display_height_ = uint16_t(height);
}

void VmpegDevice::set_video_frame_decoded(int width, int height, int picture_rate_code) {
    // vmpeg.sv:653-657, mpeg_video.sv:894-896. 90000 / rate (CDi_MiSTer sw/mpeg_video/pl_mpeg.h).
    static constexpr uint16_t kPeriod90khz[16] = {
        100, 3753, 3750, 3600, 3003, 3000, 1800, 1501, 1500, 100, 100, 100, 100, 100, 100, 100,
    };
    // PLM_VIDEO_PICTURE_RATE_30MHZ (CDi_MiSTer sw/mpeg_video/pl_mpeg.h).
    static constexpr uint32_t kPeriod30mhz[16] = {
        10000,  1251251, 1250000, 1200000, 1001001, 1000000, 600000, 500500,
        500000, 10000,   10000,   10000,   10000,   10000,   10000,  10000,
    };
    player_frame_period_ = kPeriod30mhz[picture_rate_code & 0xf];
    image_width_ = uint16_t(width);
    image_height_ = uint16_t(height);
    image_rt_ = uint16_t(picture_rate_code & 0xff);
    if (!fmv_dsp_enable_) return;
    decoder_frameperiod_rawhdr_ = uint8_t(picture_rate_code & 0xff);
    decoder_frameperiod_90khz_ = kPeriod90khz[picture_rate_code & 0xf];
}

void VmpegDevice::set_video_buffer_underflow() { fmv_interrupt_status_register_ |= kIntNdat; }

void VmpegDevice::set_video_gop_start() { fmv_interrupt_status_register_ |= kIntGop; }

void VmpegDevice::set_video_sequence_start() { fmv_interrupt_status_register_ |= kIntSeq; }

void VmpegDevice::set_video_sequence_end() { fmv_interrupt_status_register_ |= kIntEsi; }

void VmpegDevice::set_video_program_end() { fmv_interrupt_status_register_ |= kIntEii; }

void VmpegDevice::set_video_last_picture() { fmv_interrupt_status_register_ |= kIntEod; }

void VmpegDevice::set_audio_program_end() {
    fma_interrupt_status_register_ |= kFmaIntIsoEnd;
    fma_status_register_ |= 0x01; // operation finished
}

void VmpegDevice::set_audio_decoding_started() {
    fma_status_register_ |= kFmaStatusDecoding;
    fma_interrupt_status_register_ |= kFmaIntDecoding;
}

void VmpegDevice::set_audio_underflow() {
    fma_status_register_ |= 0x08;
    fma_interrupt_status_register_ |= kFmaIntUnf;
    fma_status_register_ &= ~kFmaStatusDecoding; // vmpeg.sv:743-745, with fma_dsp_enable
    reset_fma_playback_timer(); // vmpeg.sv fma_play_start .reset
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
        if (b == 0xba) {
            fma_ps_seen_ = true;
            if (fma_demux_mode_ == kDemuxModeUnknown) {
                fma_demux_mode_ = kDemuxModeProgram;
                fma_sniff_bytes_.clear();
            }
        } else if (b == 0xb9) { // program end, mpeg_demuxer.sv:231
            if (fma_demux_mode_ == kDemuxModeProgram) set_audio_program_end();
        }
        d.begin(b);
        return;
    }
    const uint8_t events = d.step(b, uint8_t(0xc0 + (fma_stream_number_ & 0xf)));
    if ((events & PsDemux::kPts) && !fma_start_valid_) {
        fma_start_dclk_ = fma_dclk_ + uint32_t(d.timestamp() >> 1) - uint32_t(d.scr >> 1);
        fma_start_valid_ = true;
    }
    if ((events & PsDemux::kPayloadByte) && fma_demux_mode_ == kDemuxModeProgram) fma_es_bytes_.push_back(b);
}

void VmpegDevice::set_audio_frame_decoded(uint32_t header) {
    fma_status_register_ |= 0x04;
    fma_interrupt_status_register_ |= kFmaIntFrame;
    // CSU follows the pending flag, so an unread one clears (vmpeg.sv:719-720).
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

bool VmpegDevice::fmv_input_fifo_full() const {
    // Demuxed bytes still here count too: the RTL writes them straight into the FIFO.
    return fmv_decoder_input_bytes_ + fmv_es_bytes_.size() > 28000;
}

uint16_t VmpegDevice::fmv_pictures_in_fifo() const {
    // mpeg_video.sv pictures_in_fifo: decoder-held pictures don't count during playback.
    int count = int(fmv_dts_fifo_.size());
    if (fmv_playback_active_) count -= fmv_decoder_held_pictures_;
    return uint16_t(std::clamp(count, 0, 0x7f));
}

bool VmpegDevice::video_display_enabled() {
    // mpeg_video.sv:936-944
    if (!fmv_playback_active_) {
        fmv_dts_start_satisfied_ = false;
        return false;
    }
    if (!fmv_dts_start_satisfied_) {
        bool head_has_dts = !fmv_dts_fifo_.empty() && fmv_dts_fifo_.front() != 0;
        if (!head_has_dts || dts_desync(fmv_dclk_, fmv_dts_fifo_.front()) > -30) fmv_dts_start_satisfied_ = true;
    }
    bool synchronous = (fmv_system_control_register_ & 0x0004) != 0;
    return !synchronous || fmv_dts_start_satisfied_;
}

void VmpegDevice::note_picture_displayed() {
    if (!fmv_dts_fifo_.empty()) fmv_dts_fifo_.erase(fmv_dts_fifo_.begin());
}

VmpegDevice::FramePlayerStep VmpegDevice::frame_player_field(uint32_t field_clocks, size_t pictures_ready) {
    FramePlayerStep step;
    const bool counting = video_display_enabled(); // also keeps the start latch
    // Step: armed at one vsync, shown at the next, playing or not (mpeg_video.sv:962-965).
    auto arm_step = [&] {
        if (player_step_latch_ && pictures_ready > size_t(step.show)) {
            player_step_latch_ = false;
            player_latch_pending_ = true;
        }
    };
    if (!fmv_playback_active_) {
        player_frame_cnt_ = 0;
        if (player_latch_pending_) potential_picture_starts_display();
        step.show = player_latch_pending_ && pictures_ready > 0;
        player_latch_pending_ = false;
        arm_step();
        return step;
    }
    const bool synchronous = (fmv_system_control_register_ & 0x0004) != 0;
    // totally_out_of_sync_need_frameskip for the picture `i` places down
    // the dts_fifo (each skipped picture's DTS leaves it).
    auto out_of_sync = [&](size_t i) {
        return synchronous && fmv_demux_scr_valid_ && i < fmv_dts_fifo_.size() && fmv_dts_fifo_[i] != 0 &&
               fmv_demux_.scr - fmv_dts_fifo_[i] > kFrameSkipThreshold;
    };
    // One skip per line of vertical blanking at most.
    while (size_t(step.skipped) < pictures_ready && step.skipped < kMaxSkipsPerBlanking &&
           out_of_sync(size_t(step.skipped))) {
        ++step.skipped;
    }
    const size_t ready = pictures_ready - size_t(step.skipped);
    const size_t next = size_t(step.skipped);
    if (counting) {
        player_frame_cnt_ += field_clocks;
        for (;;) {
            // sw/mpeg_video/main.c multiplies the period by the slow-motion factor.
            int64_t period = int64_t(player_frame_period_) * (fmv_slow_motion_ + 1);
            int64_t top = period - 1 - int64_t(player_desync_q_) * 2048;
            if (top < 1) top = 1;
            if (player_frame_cnt_ < top) break;
            player_frame_cnt_ -= top + 1;
            if (period <= 1200) continue; // mpeg_video.sv only latches above this
            player_latch_pending_ = true;
            if (ready == 0) continue; // frame_period_tick needs a picture waiting
            // dclk at the wrap, player_frame_cnt_ 30 MHz clocks before the field end.
            bool has_dts = next < fmv_dts_fifo_.size() && fmv_dts_fifo_[next] != 0;
            uint32_t dclk_at_wrap = fmv_dclk_ - uint32_t(player_frame_cnt_ * 3 / 2000);
            player_desync_q_ =
                has_dts && synchronous && !out_of_sync(next) ? dts_desync(dclk_at_wrap, fmv_dts_fifo_[next]) : 0;
        }
    }
    if (player_latch_pending_) {
        player_latch_pending_ = false;
        potential_picture_starts_display();
        step.show = ready > 0;
    }
    arm_step();
    return step;
}

// vmpeg.sv:679, event_potential_picture_starts_display: also with no picture
// ready (mpeg_video.sv:918-921).
void VmpegDevice::potential_picture_starts_display() {
    if (register_update_latch_ && !register_update_scroll_) commit_register_update();
}

void VmpegDevice::reset_fmv_demux() {
    fmv_demux_.reset();
    fmv_es_shift_ = 0xffffffff;
    fmv_next_picture_dts_ = 0;
    fmv_demux_scr_valid_ = false;
    fmv_demuxer_dts_ = 0; // mpeg_demuxer.sv:62
}

void VmpegDevice::fmv_es_byte(uint8_t b) {
    fmv_es_bytes_.push_back(b);
    fmv_es_shift_ = (fmv_es_shift_ << 8) | b;
    if (fmv_es_shift_ != 0x00000100) return; // picture start code
    if (fmv_dts_fifo_.size() < kDtsFifoDepth) fmv_dts_fifo_.push_back(fmv_next_picture_dts_);
    fmv_next_picture_dts_ = 0;
}

void VmpegDevice::fmv_demux_byte(uint8_t b) {
    if (!fmv_dsp_enable_) return; // the demuxer is held in reset (vmpeg.sv:244)
    if (fmv_demux_mode_ == kDemuxModeElementary) {
        fmv_es_byte(b);
        return;
    }
    PsDemux& d = fmv_demux_;
    if (d.state == PsDemux::kScan) {
        if (!d.scan(b)) return;
        if (fmv_demux_mode_ == kDemuxModeUnknown) {
            if (b == 0xb3) { // sequence header first: elementary stream
                fmv_demux_mode_ = kDemuxModeElementary;
                fmv_es_shift_ = 0x000001b3;
                fmv_es_bytes_.insert(fmv_es_bytes_.end(), {0x00, 0x00, 0x01, 0xb3});
                return;
            }
            if (b != 0xba) return;
            fmv_demux_mode_ = kDemuxModeProgram;
        }
        if (b == 0xb9) { // program end, mpeg_demuxer.sv:231 and vmpeg.sv:696
            set_video_program_end();
            fmv_program_end_pending_ = true;
        }
        d.begin(b);
        return;
    }
    const uint8_t events = d.step(b, uint8_t(0xe0 + (fmv_stream_number_ & 0xf)));
    if (events & PsDemux::kScr) fmv_demux_scr_valid_ = true;
    if (events & PsDemux::kLastTimestamp) {
        // The last 5 bytes: DTS if present, else PTS (mpeg_demuxer.sv).
        fmv_demuxer_dts_ = d.timestamp();
        fmv_next_picture_dts_ = fmv_demuxer_dts_;
        fmv_video_data_input_command_register_ |= 0x4000; // vmpeg.sv:660
    }
    if (events & PsDemux::kPayloadByte) fmv_es_byte(b);
}

VmpegDevice::DisplayWindow VmpegDevice::display_window() const {
    // latched_display_offset_y holds X (vmpeg.sv:160-165).
    return {
        .scrpos_x = latched_display_offset_y_,
        .scrpos_y = latched_display_offset_x_,
        .decwin_w = latched_window_width_,
        .decwin_h = latched_window_height_,
        .decoff_x = latched_window_offset_x_,
        .decoff_y = latched_window_offset_y_,
        .show = fmv_show_on_next_video_frame_,
        .vcd_pixel_clock = vcd_pixel_clock_,
    };
}

std::vector<uint8_t> VmpegDevice::drain_fmv_es_bytes() { return std::exchange(fmv_es_bytes_, {}); }

std::vector<uint8_t> VmpegDevice::drain_fma_es_bytes() { return std::exchange(fma_es_bytes_, {}); }

void VmpegDevice::feed_dma_word(uint16_t word) {
    // High byte first (vmpeg.sv:50-56).
    if (dma_for_fma_) {
        fma_input_byte(uint8_t(word >> 8));
        fma_input_byte(uint8_t(word));
    } else {
        fmv_demux_byte(uint8_t(word >> 8));
        fmv_demux_byte(uint8_t(word));
    }
}

void VmpegDevice::complete_dma() {
    // vmpeg.sv:711-714: FMV_SYSCMD bit 15 stays set.
    dma_active_ = false;
    dma_for_fma_ = false;
    fma_command_register_ &= ~uint16_t(0x8000);
}

} // namespace mamecdi
} // namespace zcdi

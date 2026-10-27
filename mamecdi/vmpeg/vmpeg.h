#pragma once
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "slave/slave.h" // VideoStandard

namespace zcdi {
namespace mamecdi {

// VMPEG (DVC cartridge) register values and the MPEG-1 system stream syntax it parses.
namespace vmpeg {

// Register word indices, vmpeg.sv `address[15:1]` (byte address 0xe00000 + 2 * index).
enum Register : uint32_t {
    kFmaCmd = 0x1800, // 0xe03000
    kFmaStatus = 0x1801, // 0xe03002
    kFmaStreamW = 0x1804, // 0xe03008 (write: wanted stream)
    kFmaStreamR = 0x1805, // 0xe0300a (read: current stream, same reg here)
    kFmaIvec = 0x1806, // 0xe0300c
    kFmaDclkH = 0x1808, // 0xe03010
    kFmaDclkL = 0x1809, // 0xe03012
    kFmaAudioHdrH = 0x180a, // 0xe03014
    kFmaAudioHdrL = 0x180b, // 0xe03016
    kFmaRun = 0x180c, // 0xe03018
    kFmaIsr = 0x180d, // 0xe0301a
    kFmaIer = 0x180e, // 0xe0301c
    kFmaDspa = 0x1811, // 0xe03022
    kFmaDspd = 0x1812, // 0xe03024

    kFmvImageWidth = 0x2001, // 0xe04002
    kFmvImageHeight = 0x2002, // 0xe04004
    kFmvImageRt = 0x2003, // 0xe04006
    kFmvTimecodeHiA = 0x2004, // 0xe04008
    kFmvTimecodeLoA = 0x2005, // 0xe0400a (vmpeg.sv comment says 0xe0400c)
    kFmvDisplayW = 0x2029, // 0xe04052
    kFmvDisplayH = 0x202a, // 0xe04054
    kFmvPicRt = 0x202b, // 0xe04056
    kFmvTimecodeHiB = 0x202c, // 0xe04058
    kFmvTimecodeLoB = 0x202d, // 0xe0405a
    kFmvTempref = 0x202e, // 0xe0405c
    kFmvSysSts = 0x202f, // 0xe0405e
    kFmvIer = 0x2030, // 0xe04060
    kFmvIsr = 0x2031, // 0xe04062
    kFmvTcnt = 0x2032, // 0xe04064
    kFmvTrld = 0x2057, // 0xe040ae (write-only reload)
    kFmvVoff = 0x2036, // 0xe0406c
    kFmvHoff = 0x2037, // 0xe0406e
    kFmvVpix = 0x2038, // 0xe04070
    kFmvHpix = 0x2039, // 0xe04072
    kFmvScrposY = 0x203a, // 0xe04074
    kFmvScrposX = 0x203b, // 0xe04076
    kFmvDecwinH = 0x203c, // 0xe04078
    kFmvDecwinW = 0x203d, // 0xe0407a
    kFmvDecoffY = 0x203e, // 0xe0407c
    kFmvDecoffX = 0x203f, // 0xe0407e
    kFmvGenDecCmd = 0x2044, // 0xe04088
    kFmvGenVdiCmd = 0x2046, // 0xe0408c
    kFmvGenSyscr = 0x204c, // 0xe04098
    kFmvGenSyncDiff = 0x204e, // 0xe0409c
    kFmvGenDecDelay = 0x204f, // 0xe0409e
    kFmvDts = 0x2050, // 0xe040a0
    kFmvPicsInFifo = 0x2052, // 0xe040a4
    kFmvPicRate = 0x2054, // 0xe040a8
    kFmvDisplayRate = 0x2055, // 0xe040aa
    kFmvFrameRate = 0x2056, // 0xe040ac
    kFmvSyscmd = 0x2060, // 0xe040c0
    kFmvVidcmd = 0x2061, // 0xe040c2
    kFmvStreamNum = 0x2062, // 0xe040c4
    kFmvSysScr = 0x2063, // 0xe040c6
    kFmvXfer = 0x206f, // 0xe040de
    kFmvIvec = 0x206e, // 0xe040dc
    kFmvMmuBase = 0x2073, // 0xe040e6
};

// FMA_CMD.
enum FmaCommand : uint16_t {
    kFmaCmdStop = 0x0001,
    kFmaCmdStartDma = 0x8000, // to audio
};

// FMA status, low byte.
enum FmaStatus : uint8_t {
    kFmaStatusFinished = 0x01,
    kFmaStatusFrame = 0x04,
    kFmaStatusUnderflow = 0x08,
    kFmaStatusDecoding = 0x10,
};

// fma_interrupt_status_register bits, vmpeg.sv:703-758.
enum FmaInterruptBit : uint16_t {
    kFmaIntIsoEnd = 1u << 0,
    kFmaIntCsu = 1u << 1, // stream change
    kFmaIntFrame = 1u << 2, // frame header updated
    kFmaIntUnf = 1u << 3, // underflow
    kFmaIntDecoding = 1u << 4, // decoding started
    kFmaIntTim = 1u << 8,
};

// FMA DSP (dsp_registers.sv): DSPA 0/1 select mode and target; with these,
// four DSPA 7 writes set the volumes in dB.
constexpr uint8_t kDspModeVolume = 0x80; // DSPD written at DSPA 0
constexpr uint8_t kDspTargetVolume = 0x93; // DSPD written at DSPA 1
constexpr uint8_t kDspaVolume = 7;

// FMV interrupt_flags_s, vmpeg.sv:243-263.
enum InterruptBit : uint16_t {
    kIntSeq = 1u << 0,
    kIntGop = 1u << 1,
    kIntPic = 1u << 2,
    kIntEod = 1u << 3,
    kIntRfb = 1u << 4,
    kIntNdat = 1u << 5,
    kIntOvf = 1u << 6,
    kIntDcl = 1u << 7,
    kIntTim = 1u << 8,
    kIntEsi = 1u << 9,
    kIntEii = 1u << 10,
    kIntVsync = 1u << 11,
    kIntPai = 1u << 12,
    kIntVcup = 1u << 13,
    kIntErdd = 1u << 14,
    kIntErdv = 1u << 15,
};

// FMV_SYSCMD (vmpeg.sv:836-949).
enum SysCmd : uint16_t {
    kSysCmdSlowMotion = 0x0007, // period x (n + 1)
    kSysCmdPlay = 0x0008,
    kSysCmdPause = 0x0010,
    kSysCmdContinue = 0x0020,
    kSysCmdStep = 0x0040,
    kSysCmdStop = 0x0080,
    kSysCmdClearFifo = 0x0100,
    kSysCmdDecoderOn = 0x1000,
    kSysCmdDecoderOff = 0x2000,
    kSysCmdStartDma = 0x8000, // to video
};

// FMV_VIDCMD (vmpeg.sv:950-981).
enum VidCmd : uint16_t {
    kVidCmdScroll = 0x0004, // RegsUpd at the next vsync, not the next picture
    kVidCmdRegsUpd = 0x0008,
    kVidCmdHide = 0x0100,
    kVidCmdShow = 0x0200,
    kVidCmdShowOnChange = 0x0400,
};

constexpr uint16_t kSysScrSynchronous = 0x0004; // SYSSCR: display follows the DTS
constexpr uint16_t kSysStsFifoNotFull = 0x2000;
constexpr uint16_t kVdiTimestampUpdated = 0x4000; // GEN_VDI_CMD bit 14 (vmpeg.sv:660)
// mpeg_video.sv fifo_full: input FIFO level over this many bytes.
constexpr uint32_t kFmvInputFifoFullBytes = 28000;

// GEN_DISPLAY_RATE (vmpeg.sv kDisplayRate_PAL/NTSC).
enum DisplayRate : uint16_t {
    kDisplayRateNtsc = 0x05dc,
    kDisplayRatePal = 0x0708,
};

// vmpeg.sv:742-747: MPEG RAM is enabled for good on the 64th register write.
constexpr int kMpegRamEnableWrites = 64;

// MPEG-1 start codes (ISO 11172), each after the 00 00 01 prefix.
constexpr uint32_t kStartCodePrefix = 0x00000100;
constexpr uint32_t kStartCodePrefixMask = 0xffffff00;
enum StartCode : uint8_t {
    kPictureStart = 0x00,
    kSequenceHeader = 0xb3,
    kProgramEnd = 0xb9, // ISO 11172 end code
    kPackStart = 0xba,
    kSystemHeader = 0xbb, // this and above carry a 16-bit length
    kAudioStream = 0xc0, // + stream number
    kVideoStream = 0xe0, // + stream number
};

// PES header bytes before the payload (ISO 11172-1 2.4.3.3).
enum PesHeader : uint8_t {
    kPesHasDts = 0x10,
    kPesPts = 0x20, // 0010 PTS or 0011 PTS + DTS
    kPesStdBuffer = 0x40, // P-STD buffer size, 2 bytes
    kPesStdBufferMask = 0xc0,
    kPesPtsMask = 0xe0,
    kPesStuffing = 0xff,
};

} // namespace vmpeg

// One mpeg_demuxer.sv: MPEG-1 pack and PES parsing for one channel. The
// channels handle start codes themselves (scan(), then begin()).
struct PsDemux {
    enum : uint8_t {
        kScan = 0,
        kLengthHigh,
        kLengthLow,
        kSkip,
        kPesHeader,
        kPesStd,
        kPesTimestamp,
        kPayload,
        kPack,
    };
    // step() results, OR'd.
    enum : uint8_t { kNone = 0, kScr = 1, kPts = 2, kLastTimestamp = 4, kPayloadByte = 8 };

    uint8_t state = kScan;
    uint8_t stream = 0; // id of the packet being read
    uint32_t shift = 0xffffffff;
    uint16_t remaining = 0;
    uint8_t ts_bytes = 0; // timestamp bytes still to read
    bool has_dts = false; // this PES carries PTS and DTS
    uint64_t ts_accum = 0;
    int64_t scr = 0; // last pack SCR, 90 kHz

    // Back to kScan, start code search restarted.
    void reset();
    // In kScan: true once `b` completes a start code.
    bool scan(uint8_t b);
    // Acts on start code `code`: a pack header, or a packet with a 16-bit length.
    void begin(uint8_t code);
    // Any state but kScan; only packets of stream `wanted` are parsed.
    uint8_t step(uint8_t b, uint8_t wanted);
    // The 33-bit, 90 kHz timestamp in the last 5 bytes read.
    int64_t timestamp() const;
};

struct VmpegSaveState {
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
    uint8_t fma_demux_mode_ = 0; // kDemuxModeUnknown
    PsDemux fma_demux_;
    bool fma_ps_seen_ = false;
    bool fma_start_valid_ = false;
    uint32_t fma_start_dclk_ = 0;

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
    uint16_t fmv_frame_rate_ = 0; // GEN_FRAME_RATE, 0xe040ac
    uint16_t fmv_timer_compare_register_ = 56 - 1; // TCNT, reset value
    uint32_t timer_cnt_ = 0; // 21 bits
    uint16_t fmv_decoder_command_ = 0; // GEN_DEC_CMD, 0xe04088
    uint16_t fmv_video_data_input_command_register_ = 0; // GEN_VDI_CMD
    uint16_t image_width_ = 0, image_height_ = 0, image_rt_ = 0;
    uint16_t display_width_ = 0, display_height_ = 0; // last picture shown
    uint16_t decoder_frameperiod_90khz_ = 0;
    uint8_t decoder_frameperiod_rawhdr_ = 0;
    int fmv_decoder_held_pictures_ = 0;
    uint32_t fmv_decoder_input_bytes_ = 0; // handed to the decoder, not yet parsed
    bool fmv_dts_start_satisfied_ = false; // display_dts_desync_satisfied_latch
    // Frame player: playback_frame_cnt, latch_frame_until_vsync,
    // display_dts_desync_q, frame_period (30 MHz clocks).
    int64_t player_frame_cnt_ = 0;
    bool player_latch_pending_ = false;
    int player_desync_q_ = 0;
    uint32_t player_frame_period_ = 1200000;
    uint8_t fmv_slow_motion_ = 0; // SYSCMD bits 2-0; period x (n + 1)
    bool player_step_latch_ = false; // single_step_latch

    // FMV program-stream demuxer and the DTS of pictures not yet shown.
    enum : uint8_t {
        kDemuxModeUnknown = 0,
        kDemuxModeProgram = 1,
        kDemuxModeElementary = 2,
    };
    uint8_t fmv_demux_mode_ = kDemuxModeUnknown;
    PsDemux fmv_demux_;
    uint32_t fmv_es_shift_ = 0xffffffff;
    int64_t fmv_demuxer_dts_ = 0; // 33-bit, 90 kHz
    bool fmv_demux_scr_valid_ = false;
    int64_t fmv_next_picture_dts_ = 0;
    static constexpr size_t kDtsFifoDepth = 127; // 7-bit count, mpeg_video.sv
    static constexpr size_t kMaxFmaPsSniffBytes = 64 * 2324; // ~64 Form-2 sectors
    std::vector<int64_t> fmv_dts_fifo_; // one entry per picture not yet shown
    uint8_t fmv_stream_number_ = 0;

    // Window registers as written, and the copies RegsUpd latches (vmpeg.sv:127-132, 623-628).
    uint16_t video_ctrl_y_offset_ = 0, video_ctrl_x_offset_ = 0;
    uint16_t video_ctrl_y_active_ = 0, video_ctrl_x_active_ = 0;
    uint16_t video_ctrl_y_display_ = 0, video_ctrl_x_display_ = 0;
    uint16_t video_ctrl_window_width_ = 0, video_ctrl_window_height_ = 0;
    uint16_t video_ctrl_decoder_offset_y_ = 0, video_ctrl_decoder_offset_x_ = 0;
    uint16_t latched_display_offset_x_ = 0, latched_display_offset_y_ = 0;
    uint16_t latched_window_offset_x_ = 0, latched_window_offset_y_ = 0;
    uint16_t latched_window_width_ = 0, latched_window_height_ = 0;
    bool register_update_latch_ = false;
    bool register_update_scroll_ = false;

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
    bool audio_reset_pending_ = false;
    bool dma_active_ = false;
    uint8_t fmv_xfer_pending_high_ = 0; // latched high byte of an in-progress 16-bit XFER write
    // Demuxed elementary streams for the decoders (mpeg_packet_body bytes, vmpeg.sv:89/150).
    std::vector<uint8_t> fmv_es_bytes_;
    std::vector<uint8_t> fma_es_bytes_;
    std::vector<uint8_t> fma_sniff_bytes_; // input kept until PS or ES is decided
    bool fmv_program_end_pending_ = false;
    int dclk_cycle_accum_ = 0; // 3 dclk ticks per 1000 CPU cycles
};

class VmpegDevice : private VmpegSaveState {
public:
    VmpegDevice();
    // For debuggers (MachineInspector).
    const VmpegSaveState& state() const { return *this; }

    template <class T>
    void state_io(T& archive);

    // Byte access, offset = addr - 0xe00000.
    uint8_t read_byte(uint32_t offset);
    void write_byte(uint32_t offset, uint8_t value);

    void tick_cycles(int cpu_cycles);

    // One call per field.
    void notify_vsync();

    bool irq_asserted() const;
    int iack_vector() const;

    // Unlike read_byte(), these don't clear the ISR.
    uint16_t fmv_isr() const { return fmv_interrupt_status_register_; }
    uint16_t fmv_pictures_in_fifo() const;
    uint16_t fmv_dts_register() const { return uint16_t(fmv_demuxer_dts_ >> 7) & 0x7fff; }

    // Also commits a pending non-scroll RegsUpd.
    void set_video_frame_ready(int width, int height);
    void set_video_frame_decoded(int width, int height, int picture_rate_code);
    void set_video_buffer_underflow();

    void set_video_gop_start();
    void set_video_sequence_start();
    void set_video_sequence_end();
    void set_audio_decoding_started();
    // FMA status "decoding": set by the playback timer or, for audio
    // without timestamps, the first decoded samples.
    bool audio_dsp_enabled() const { return (fma_status_register_ & vmpeg::kFmaStatusDecoding) != 0; }
    // PS audio waiting for the playback timer to reach its first PTS.
    bool audio_start_pending() const { return fma_ps_seen_ && !audio_dsp_enabled(); }
    void set_audio_frame_decoded(uint32_t header);
    void set_audio_underflow();
    // Once per field: `decoded` sample frames were decoded, `field_samples`
    // played. Raises underflow when the field ends with everything played.
    void audio_field(int32_t decoded, int32_t field_samples);
    void set_video_program_end();
    void set_video_last_picture();
    void set_audio_program_end();
    void set_decoder_held_pictures(int count) { fmv_decoder_held_pictures_ = count; }
    void set_decoder_input_bytes(size_t bytes) { fmv_decoder_input_bytes_ = uint32_t(bytes); }
    // mpeg_video.sv fifo_full: input FIFO level over 28000 bytes.
    bool fmv_input_fifo_full() const;

    bool video_display_enabled();
    void note_picture_displayed();

    struct FramePlayerStep {
        int skipped = 0;
        bool show = false;
    };
    // `field_clocks` in 30 MHz clocks; skipped pictures come before the shown one.
    FramePlayerStep frame_player_field(uint32_t field_clocks, size_t pictures_ready);

    void set_video_decoder_timecode(uint32_t timecode);
    void set_video_display_timecode(uint32_t timecode);
    void set_video_display_status(uint8_t status);

    struct DisplayWindow {
        uint16_t scrpos_x = 0, scrpos_y = 0;
        uint16_t decwin_w = 0, decwin_h = 0;
        uint16_t decoff_x = 0, decoff_y = 0;
        bool show = false;
        bool vcd_pixel_clock = false; // 13.5 MHz instead of 15 MHz
    };
    DisplayWindow display_window() const;

    bool mpeg_ram_enabled() const { return mpeg_ram_enabled_cnt_ >= vmpeg::kMpegRamEnableWrites; }

    // Q0.8 gains (256 = unity); unconfigured means apply no gain.
    uint8_t mpeg_volume_l2l() const { return mpeg_volume_l2l_; }
    uint8_t mpeg_volume_r2r() const { return mpeg_volume_r2r_; }
    uint8_t mpeg_volume_l2r() const { return mpeg_volume_l2r_; }
    uint8_t mpeg_volume_r2l() const { return mpeg_volume_r2l_; }

    void set_video_standard(VideoStandard standard) { video_standard_ = standard; }

    // Elementary-stream bytes for the video and audio decoders.
    std::vector<uint8_t> drain_fmv_es_bytes();
    std::vector<uint8_t> drain_fma_es_bytes();

    // One-shot: vmpeg::kProgramEnd demuxed on the FMV channel (EII is already raised).
    bool consume_fmv_program_end() { return std::exchange(fmv_program_end_pending_, false); }

    void feed_dma_word(uint16_t word);

    bool dma_active() const { return dma_active_; }
    // Read before complete_dma(), which clears it.
    bool dma_for_fma() const { return dma_for_fma_; }

    bool consume_clear_fifo() { return std::exchange(clear_fifo_pending_, false); }
    bool consume_decoder_off() { return std::exchange(decoder_off_pending_, false); }
    // FMA_CMD stop: reset the MPEG audio decoder (vmpeg.sv:852).
    bool consume_audio_reset() { return std::exchange(audio_reset_pending_, false); }

    // Leaves kSysCmdStartDma set, as the RTL does (vmpeg.sv:711-714).
    void complete_dma();

private:
    uint16_t read_word(uint32_t word_index) const;
    // Side effects run only when `completing` (the low byte of a 16-bit write).
    void write_word(uint32_t word_index, uint16_t value, bool completing);
    void handle_fmv_syscmd_write(uint16_t value);
    void handle_fmv_vidcmd_write(uint16_t value);
    void handle_fmv_gen_dec_cmd_write(uint16_t value);
    void note_bus_write_access();

    void fma_input_byte(uint8_t b);
    void fma_demux_byte(uint8_t b);
    void reset_fma_playback_timer();
    void fmv_demux_byte(uint8_t b);
    void fmv_es_byte(uint8_t b);
    void reset_fmv_demux();
    // What Clear FIFO and Decoder off both reset (mpeg_video.sv:385, :863-865, :1006).
    void reset_fmv_pipeline();
    void commit_register_update();
    void potential_picture_starts_display();

    VideoStandard video_standard_ = VideoStandard::kNtsc; // configuration, not saved
};

} // namespace mamecdi
} // namespace zcdi

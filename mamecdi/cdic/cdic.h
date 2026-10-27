#pragma once
#include <array>
#include <cstdint>
#include <vector>

#include "cdic/cdic_adpcm.h"
#include "disc/disc.h"

namespace zcdi {
namespace mamecdi {

// Register values, as CdicSaveState and CDIC RAM hold them (names after same_cdi).
namespace cdic {

// Register byte offsets from 0x303c00 (high byte first).
enum RegisterOffset : uint32_t {
    kRegCommand = 0x000,
    kRegTime = 0x002, // 4 bytes
    kRegFile = 0x006,
    kRegChannel = 0x008, // 4 bytes
    kRegAudioChannel = 0x00c,
    kRegAbuf = 0x3f4,
    kRegXbuf = 0x3f6,
    kRegDmaControl = 0x3f8, // write-only
    kRegAudctl = 0x3fa,
    kRegInterruptVector = 0x3fc,
    kRegDbuf = 0x3fe,
};

// Command register, low byte (same_cdi handle_cdic_command).
enum Command : uint8_t {
    kCmdIdle = 0x00, // as disc_command_ only
    kCmdResetMode1 = 0x23,
    kCmdResetMode2 = 0x24,
    kCmdFetchToc = 0x27,
    kCmdPlayCdda = 0x28,
    kCmdReadMode1 = 0x29,
    kCmdReadMode2 = 0x2a,
    kCmdStopCdda = 0x2b,
    kCmdSeek = 0x2c,
    kCmdUpdate = 0x2e,
};

// ABUF/XBUF: a buffer is ready; reading the high byte clears it.
constexpr uint16_t kBufReady = 0x8000;

// DBUF.
enum Dbuf : uint16_t {
    kDbufBank0 = 0x0000,
    kDbufBank1 = 0x0001,
    kDbufAudio = 0x0004, // the sector's audio went to the audio bank
    kDbufActive = 0x4000, // clear: stop reading
    kDbufTrigger = 0x8000, // write: run the command register
};

// AUDCTL (cdic.sv).
enum Audctl : uint16_t {
    kAudctlStoppedOnEnd = 0x0001, // the player met kAudioMapEnd; cleared by reading
    kAudctlRun = 0x0800,
    kAudctlAbufIrq = 0x2000,
};

// CDIC RAM, byte addresses.
enum RamLayout : uint32_t {
    kDataBank0 = 0x0000,
    kDataBank1 = 0x0a00,
    kCddaBank0 = 0x1400, // cdic.sv read_raw: words 0x0a00/0x0f00
    kCddaBank1 = 0x1e00,
    kAudioMap0 = 0x2800,
    kAudioMap1 = 0x3200,
};
// A data bank holds the sector from its header on, split at FILE2 (same_cdi
// SECTOR_FILE2) where Mode 2 audio skips to the audio bank, then the Q subcode
// one byte per word.
constexpr uint32_t kFile2Offset = 20;
constexpr uint32_t kSubqOffset = (kFile2Offset - kSyncSize) + (kRawSectorSize - kFile2Offset);
constexpr uint32_t kAudioBankSkipBytes = 0x1400u * 2; // 0x1400 RAM words

// An audio map buffer: the coding byte (kAudioMapEnd stops the player), then the data.
enum AudioMapLayout : uint32_t {
    kAudioMapCoding = 11,
    kAudioMapData = 12,
};
constexpr uint8_t kAudioMapEnd = 0xff;

} // namespace cdic

struct CdicSaveState {
    static constexpr uint32_t kRamSize = 0x4000;
    std::array<uint8_t, kRamSize> ram_ = {};

    // --- registers ---
    uint16_t command_ = 0;
    uint32_t time_ = 0; // BCD MSF target (Time MSW<<16 | Time LSW)
    uint16_t file_ = 0;
    // Reset default is all channels selected (same_cdi device_reset).
    uint32_t channel_ = 0xffffffffu; // 32-bit channel bitmask
    uint16_t audio_channel_ = 0xffffu;
    uint16_t audio_buffer_ = 0; // ABUF, cdic::kBufReady = audio-ready
    uint16_t x_buffer_ = 0; // XBUF, cdic::kBufReady = sector-ready
    uint16_t audio_control_ = 0; // cdic::Audctl
    uint16_t interrupt_vector_ = 0x0f;
    uint16_t data_buffer_ = 0; // cdic::Dbuf
    uint16_t dma_control_ = 0; // low 14 bits = starting word offset into CDIC RAM for a DMA transfer

    // cdic.sv data_target_buffer/audio_target_buffer: not part of DBUF, a
    // CPU DBUF write doesn't move them (only a bit14-clear write resets both).
    bool data_target_buffer_ = false;
    bool audio_target_buffer_ = false;

    bool irq4_ = false;

    // --- disc-read state machine ---
    uint8_t disc_command_ = 0; // cdic::Command, kCmdIdle = none
    int curr_lba_ = 0;
    int spinup_counter_ = 0;
    int sector_cycle_accum_ = 0; // fractional cycles carried between tick_cycles calls
    // Sector ticks until XBUF bit 15 after Stop/Update (cdic.sv spin_down_cnt); 0 = none.
    int spin_down_count_ = 0;
    int spin_down_cycle_accum_ = 0;

    int read_fail_streak_ = 0;

    // --- ADPCM player, paced by its own 75 Hz timer ---
    uint16_t decode_addr_ = 0xffff; // next buffer to play: cdic::RamLayout, 0xffff = none yet
    bool decoding_audio_map_ = false; // running (kAudctlRun as the player sees it)
    bool cdda_mode_ = false; // set by executing Play CDDA, cleared by any other command
    int audio_sector_counter_ = 0; // 75Hz ticks left of the buffer playing
    int audio_format_sectors_ = 0; // length of the buffer playing in ticks; 0 = none playing
    int audio_map_cycle_accum_ =
        0; // fractional cycles carried between tick_cycles calls, separate from sector_cycle_accum_ above

    std::vector<int16_t> audio_out_;

    AdpcmResampleState adpcm_resample_state_;

    // Reset when the player starts from idle (cdic.sv reset_filter_on_start).
    History adpcm_history_[2];
};

class Cdic : private CdicSaveState {
public:
    Cdic();
    // For debuggers (MachineInspector).
    const CdicSaveState& state() const { return *this; }

    // The mounted disc is not saved: it stays attached across a load.
    template <class T>
    void state_io(T& archive);

    uint8_t read_ram_byte(uint32_t offset) const; // 0x300000-0x303bff, offset relative to 0x300000
    void write_ram_byte(uint32_t offset, uint8_t v);
    uint8_t read_reg_byte(uint32_t offset); // 0x303c00-0x303fff, offset relative to 0x303c00
    void write_reg_byte(uint32_t offset, uint8_t v);

    void set_disc(Disc* disc) {
        disc_ = disc;
        tracks_ = disc ? disc->toc_tracks() : std::vector<TocTrack>{};
    }

    // Call with each CPU chunk, not once per frame: more than one sector
    // before the CPU services the first breaks CD-RTOS's double buffering.
    void tick_cycles(int cpu_cycles);

    bool irq4_asserted() const { return irq4_; }
    int iack_vector() const { return interrupt_vector_ & 0xff; }

    // True while Fetch TOC, Play CDDA, Read or Seek is running.
    bool disc_active() const { return disc_command_ != cdic::kCmdIdle; }

    // Low 14 bits: start word in CDIC RAM for Bus::perform_cdic_dma.
    uint16_t dma_control() const { return dma_control_; }

    // Interleaved stereo S16 at 44100 Hz (ADPCM resampled from 37800 Hz).
    std::vector<int16_t> drain_audio();

private:
    void handle_data_buffer_write(uint16_t value);
    void start_read(uint8_t command);
    // Ends the disc read (curr_lba_ is kept).
    void stop_read();
    bool process_sector(); // returns false to retry curr_lba_ next tick instead of advancing
    void deliver_toc_sector();
    bool is_mode2_sector_selected(const RawSector& s) const;
    bool is_mode2_audio_selected(const RawSector& s) const;
    void pack_into_ram(const RawSector& s);
    void pack_cdda_into_ram(const RawSector& s, const uint8_t* subq);
    // Flips the data toggle into DBUF bit 0; true = bank 1 (cdic.sv:559-582).
    bool next_data_bank();
    // A sector is in RAM: XBUF bit 15, DBUF bit 14, IN4.
    void signal_sector();
    void raise_irq4();

    void handle_audctl_write();
    void audio_map_tick();
    void process_audio_map();
    void abort_audio_playback();

    Disc* disc_ = nullptr;
    std::vector<TocTrack> tracks_; // for the Q subcode
    // Start of the playing buffer in audio_out_, so an abort can drop it. 0 after a drain.
    std::size_t audio_out_mark_ = 0;
};

} // namespace mamecdi
} // namespace zcdi

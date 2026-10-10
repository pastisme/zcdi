#pragma once
#include <array>
#include <cstdint>
#include <vector>

#include "cdic/cdic_adpcm.h"
#include "disc/disc.h"

namespace zcdi {
namespace mamecdi {

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
    uint16_t audio_buffer_ = 0; // ABUF, bit15 = audio-ready
    uint16_t x_buffer_ = 0; // XBUF, bit15 = sector-ready
    uint16_t audio_control_ = 0; // AUDCTL
    uint16_t interrupt_vector_ = 0x0f;
    uint16_t data_buffer_ = 0; // bit15=trigger(write-only-effect) bit14=active bits0,2=bank select (bit2 set only for a
                               // Mode2-audio-selected sector)
    uint16_t dma_control_ = 0; // low 14 bits = starting word offset into CDIC RAM for a DMA transfer

    // cdic.sv data_target_buffer/audio_target_buffer: not part of DBUF, a
    // CPU DBUF write doesn't move them (only a bit14-clear write resets both).
    bool data_target_buffer_ = false;
    bool audio_target_buffer_ = false;

    bool irq4_ = false;

    // --- disc-read state machine ---
    uint8_t disc_command_ = 0; // 0 = idle
    int curr_lba_ = 0;
    int spinup_counter_ = 0;
    int sector_cycle_accum_ = 0; // fractional cycles carried between tick_cycles calls
    // Sector ticks until XBUF bit 15 after Stop/Update (cdic.sv spin_down_cnt); 0 = none.
    int spin_down_count_ = 0;
    int spin_down_cycle_accum_ = 0;

    int read_fail_streak_ = 0;

    // --- ADPCM player, paced by its own 75 Hz timer ---
    uint16_t decode_addr_ = 0xffff; // next buffer to play
    bool decoding_audio_map_ = false; // running (AUDCTL bit 11 as the player sees it)
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
    bool disc_active() const { return disc_command_ != 0; }

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

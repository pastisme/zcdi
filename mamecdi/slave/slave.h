#pragma once
#include <cstdint>
#include <utility>
#include <vector>

#include "scc68070/scc68070.h"

namespace zcdi {

// The SLAVE's STAND pin strap; values are what status command 0xf6 reports.
enum class VideoStandard : uint8_t { kNtsc = 0x01, kPal = 0x02 };

namespace mamecdi {

struct SlaveSaveState {
    static constexpr int kNumChannels = 4;

    struct Channel {
        uint8_t in[4] = {0}; // accumulated command bytes (opcode first); 0xb0 is the longest
        int in_len = 0;
        uint8_t out[4] = {0}; // queued response bytes
        int out_len = 0;
        int out_pos = 0;
    };

    Channel channels_[kNumChannels];
    bool irq2_ = false;
    int disc_status_delay_ = 0;
    // While set, Bus keeps the disc from the CDIC.
    bool tray_open_ = false;
    // Channel 3's 0xfa/0xfb ($63 bit 7); cleared at power-on.
    bool status_forwarding_ = false;

    // SLAVE firmware 2.0 audio attenuation; comments give each field's RAM location.
    int timer_cycle_accum_ = 0;
    uint8_t timer_high_ = 0; // $1a
    uint8_t att_command_ = 0xf5; // $75: power-on mutes both cross paths
    uint8_t att_db_[4] = {0}; // $76-$79
    uint8_t data_count_ = 0; // $6c
    bool level_mode_ = false; // $59 bit 0: 0x90's 2 bytes pending
    bool master_enabled_ = false; // $59 bit 1, toggled by 0x88
    uint8_t master_ = 0; // $d6: dB in bits 0-5, bit 6/7 ramp louder/softer
    uint8_t ramp_count_ = 0x0a; // $d7
    uint8_t ramp_reload_ = 0x0a; // $d8
    bool dim_ = false; // $55 bit 7
    bool muted_ = false; // $56 bit 0
    bool fading_ = false; // $53 bit 4
    uint8_t fade_start_ = 0; // $64, a $1a value
    // Path order L->L, L->R, R->R, R->L.
    uint8_t dac_[4] = {0};

    // 0x83/0x84 on channel 0, not MAME's old 0xf7 gate.
    bool pointer_enabled_ = false;
    int16_t pointer_x_ = 0;
    int16_t pointer_y_ = 0;
    uint8_t button_bits_ = 0x01; // latest host state, bit layout as sent
    int slot_cycle_accum_ = 0;
    int report_slots_left_ = 0; // >0: the reported_* sample is being "transmitted"
    bool have_reported_ = false;
    uint8_t reported_button_bits_ = 0;
    int16_t reported_x_ = 0;
    int16_t reported_y_ = 0;
};

class SlaveHle : private SlaveSaveState {
public:
    SlaveHle();
    // For debuggers (MachineInspector).
    const SlaveSaveState& state() const { return *this; }

    uint8_t read_byte(uint32_t addr);
    void write_byte(uint32_t addr, uint8_t v);

    // Counts down the 0xb0 / tray disc-status reply delay. Once per frame.
    void tick_frame();

    bool irq2_asserted() const { return irq2_; }

    // Channel 2's 0x8a (Reset main cpu): true once per request; the caller resets
    // the machine. Not saved: consumed within the CPU chunk that wrote it.
    bool consume_reset_request() { return std::exchange(reset_requested_, false); }

    // Inputs, not saved. `audio`: every track is audio (servo status 01 25).
    void set_disc_present(bool present, bool audio) {
        disc_present_ = present;
        disc_audio_ = audio;
    }

    // Player's own tray button or frontend eject; reports like channel 2's 0x89/0x92.
    void set_tray_open(bool open);
    bool tray_open() const { return tray_open_; }

    // Configuration, set once before boot, not saved.
    void set_video_standard(VideoStandard standard) { video_standard_ = standard; }

    // Relative deltas; the position is clamped to device space 0-767/0-559.
    // Reports go out from tick_cycles(), only after the BIOS sent 0x83.
    void update_pointer(int dx, int dy, bool button1, bool button2 = false, bool button12 = false);

    // Absolute device-space position (0-767/0-559), e.g. for a lightgun.
    void set_pointer(int x, int y, bool button1, bool button2 = false, bool button12 = false);

    // Paces pointer reports and the SLAVE timer. Call with each CPU chunk.
    void tick_cycles(int cpu_cycles);

    // Applies the SLAVE's AD7528 attenuators to the frame's CDIC audio, in place.
    // Changes land where they happened within the frame. Call once per frame.
    void attenuate(std::vector<int16_t>& interleaved_stereo);

    template <class T>
    void state_io(T& archive);

private:
    void handle_write(int channel, uint8_t v);
    void queue_response(int out_channel, const uint8_t* data, int len);

    // 0xb0's reply comes ~250 ms after the command (same_cdi/MAME trace), 13 PAL frames.
    static constexpr int kDiscStatusDelayFrames = 13;
    bool disc_present_ = false;
    bool disc_audio_ = false;
    VideoStandard video_standard_ = VideoStandard::kNtsc;

    // SLAVE firmware 2.0 audio attenuation.
    void channel2_data(uint8_t v);
    void start_fade();
    void fade_step();
    void timer_tick();
    void apply_dacs();
    // The fade's gain now (firmware $50), from its start and direction.
    uint8_t fade_gain() const;
    uint8_t dac_code(uint8_t db, bool forced_mute) const;

    // 0.5 MHz counter (CDi_MiSTer uc_68hc05.sv); its high byte $1a ticks every 512 us.
    static constexpr int kTimerTickCycles = int(int64_t(kCpuClockHz) * 256 / 500000);
    // Not saved: the window is empty between frames.
    struct DacChange {
        int64_t cycle = 0;
        uint8_t dac[4] = {};
    };
    uint8_t window_start_dac_[4] = {0};
    std::vector<DacChange> dac_changes_;
    int64_t window_cycles_ = 0;
    // Starts a new window at the current DAC codes.
    void reset_dac_window();

    bool reset_requested_ = false; // see consume_reset_request()

    // CDi_MiSTer pointing_device.sv: 1200 baud, one byte per 1/120 s slot,
    // one sample per 3 slots (25 ms).
    static constexpr int kPointerSlotCycles = kCpuClockHz / 120;
    void on_pointer_slot();
    // Clamped to device space 0-767/0-559.
    void set_pointer_position(int x, int y);
    bool loaded(); // checks and rebuilds what a load can't carry
};

} // namespace mamecdi
} // namespace zcdi

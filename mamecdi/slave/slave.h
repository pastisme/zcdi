#pragma once
#include <cstdint>
#include <utility>
#include <vector>

#include "scc68070/scc68070.h"

namespace zcdi {

// The SLAVE's STAND pin strap; values are what status command 0xf6 reports.
enum class VideoStandard : uint8_t { kNtsc = 0x01, kPal = 0x02 };

namespace mamecdi {

// $b6 (firmware 0x762), as 0xf3 reports it.
enum class FrontDeviceType : uint8_t {
    kNone = 0,
    kRelative = 1, // 'M'
    kManeuvering = 2, // 'J'
    kAbsoluteCoordinate = 3, // 'T'
    kAbsoluteScreen = 4, // 'S'
    kPowerOn = 0x20, // 0x1c3
};

// Driven by 0xfc/0xfd (0x47b, 0x9bb); the back socket's RTS.
enum class Pc6 : uint8_t { kReleased, kHigh, kLow };

// SLAVE and SERVO protocol values (cdichips mc6805slave.md, SLAVE 2.0 / SERVO 2.1 firmware).
namespace slave {

// Channels A-D; kChannelControl also carries kChannelStatus's replies.
enum ChannelId : int { kChannelPointer, kChannelLcd, kChannelControl, kChannelStatus, kNumChannels };

// Servo tray states ($99, firmware 0x1ac3): 2-4 closing, 6-9 opening.
enum TrayState : uint8_t {
    kTrayOpen = 1,
    kTrayClosing = 2,
    kTrayClosed = 5,
    kTrayOpening = 6,
};

// Master volume ($d6): attenuation in dB plus the running ramp's direction.
enum MasterVolume : uint8_t {
    kMasterDb = 0x3f,
    kMasterRampLouder = 0x40,
    kMasterRampSofter = 0x80,
    kMasterRamp = 0xc0,
};

// Channel 0 (0x3b6): 0x80-0xbf decode on their low nibble.
enum PointerCommand : uint8_t {
    kPtrEnable = 0x03, // 0x83
    kPtrDisable = 0x04, // 0x84
    kPtrSetPosition = 0xc0, // 0xc0-0xff, 3 bytes
};

// Channel 2 (firmware 0x48d).
enum ControlCommand : uint8_t {
    kCtlMute = 0x82,
    kCtlUnmute = 0x83,
    kCtlUnmutePc7 = 0x84, // also sets port C bit 7, which drives nothing audible
    kCtlMasterLouder = 0x85,
    kCtlMasterSofter = 0x86,
    kCtlMasterStop = 0x87,
    kCtlMasterToggle = 0x88,
    kCtlTrayOpen = 0x89,
    kCtlResetCpu = 0x8a,
    kCtlDim = 0x8f, // toggles the 20 dB dim
    kCtlMasterLevel = 0x90, // level and ramp rate bytes follow
    kCtlTrayClose = 0x92,
    kCtlStopAndOpen = 0x93,
    kCtlAttenuation = 0xc0, // 0xc0-0xcf: 4 dB bytes follow
    kCtlAttenuationEnd = 0xd0,
};
constexpr uint8_t kCtlLevelUnchanged = 0xff; // as a kCtlMasterLevel data byte

// Channel 3; replies come on channel 2.
enum SystemCommand : uint8_t {
    kSysFourByte = 0x80, // 0x80-0xef: 4-byte commands, mostly for the servo
    kSysVersion = 0xf0,
    kSysFrontDevice = 0xf3,
    kSysTestPlug = 0xf4,
    kSysMasterVolume = 0xf5,
    kSysVideoStandard = 0xf6,
    kSysReplyIrqOn = 0xf8,
    kSysReplyIrqOff = 0xf9,
    kSysForwardOn = 0xfa,
    kSysForwardOff = 0xfb,
    kSysPc6High = 0xfc,
    kSysPc6Release = 0xfd,
};

// SERVO firmware 2.1 (0x1698).
enum ServoCommand : uint8_t {
    kServoVersion = 0xa3,
    kServoEcho = 0xa4,
    kServoTrayOpen = 0xa6,
    kServoTrayClose = 0xa7,
    kServoStopAndOpen = 0xa8,
    kServoDiscStatus = 0xb0,
};

// In the servo's kServoDiscStatus report.
enum DiscType : uint8_t {
    kDiscUnknown = 0x00, // TOC not read yet
    kDiscAudio = 0x01,
    kDiscNone = 0x03,
    kDiscCdi = 0x04,
};

// Channel 0 pointer report, first byte (cdichips "Pointer state").
enum PointerReport : uint8_t {
    kReportNone = 0x00,
    kReportButton1 = 0x10,
    kReportButton2 = 0x20,
};

} // namespace slave

struct SlaveSaveState {
    struct Channel {
        uint8_t in[4] = {0}; // accumulated command bytes (opcode first); 0xb0 is the longest
        int in_len = 0;
        uint8_t out[4] = {0}; // queued response bytes
        int out_len = 0;
        int out_pos = 0;
    };

    Channel channels_[slave::kNumChannels];
    // Frames to the servo's next tray step; when closed, until the disc type is known.
    int tray_timer_ = 0;
    // Open or opening: what the frontend sees as ejected.
    bool tray_open_ = false;
    // Channel 3's 0xfa/0xfb ($63 bit 7); cleared at power-on.
    bool status_forwarding_ = false;
    // Channel 3's 0xf8/0xf9 ($63 bit 6): channel 2 replies raise IN2; cleared at power-on.
    bool reply_irq_enabled_ = false;
    // $52: byte 1 of the servo's 0xa3 reply, the last byte of 0xf0's reply; 0 until then.
    uint8_t servo_version_ = 0;
    // Servo $99 (firmware 0x1ac3).
    uint8_t tray_state_ = slave::kTrayClosed; // slave::TrayState

    // SLAVE firmware 2.0 audio attenuation; comments give each field's RAM location.
    int timer_cycle_accum_ = 0;
    uint8_t timer_high_ = 0; // $1a
    uint8_t att_command_ = 0xf5; // $75: power-on mutes both cross paths
    uint8_t att_db_[4] = {0}; // $76-$79
    uint8_t data_count_ = 0; // $6c
    bool level_mode_ = false; // $59 bit 0: 0x90's 2 bytes pending
    bool master_enabled_ = false; // $59 bit 1, toggled by 0x88
    uint8_t master_ = 0; // slave::MasterVolume
    uint8_t ramp_count_ = 0x0a; // $d7
    uint8_t ramp_reload_ = 0x0a; // $d8
    bool dim_ = false; // $55 bit 7
    bool muted_ = false; // $56 bit 0
    bool fading_ = false; // $53 bit 4
    uint8_t fade_start_ = 0; // $64, a $1a value
    // Path order L->L, L->R, R->R, R->L.
    uint8_t dac_[4] = {0};

    // Front socket device (0x67a, 0xd90).
    FrontDeviceType front_type_ = FrontDeviceType::kPowerOn; // $b6
    bool flip_baud_ = false; // $55 b6
    bool sci_9600_ = false; // BAUD 0x30, else 0x33
    bool sci_receiving_ = false; // SCCR2 0x24
    bool front_rts_negated_ = false; // PB4 high
    uint8_t detect_ticks_ = 0; // $b9
    uint8_t packet_count_ = 0; // $af
    bool sync_lost_once_ = false; // $56 b2
    uint8_t packet_buttons_ = 0; // $b0
    uint16_t packet_x_ = 0; // $b1:$b2
    uint16_t packet_y_ = 0; // $b3:$b4 (absolute devices)
    bool pointer_enabled_ = false; // $63 b4, 0x83/0x84
    int16_t pointer_x_ = 0; // $61:$62
    int16_t pointer_y_ = 0; // $5f:$60
    uint8_t buttons_ = 0; // $5e
    Pc6 pc6_ = Pc6::kReleased;
    uint8_t pc6_ticks_ = 0; // $ba
};

class SlaveHle : private SlaveSaveState {
public:
    SlaveHle();
    // For debuggers (MachineInspector).
    const SlaveSaveState& state() const { return *this; }

    uint8_t read_byte(uint32_t addr);
    void write_byte(uint32_t addr, uint8_t v);

    // Steps the servo's tray. Once per frame.
    void tick_frame();

    // Main loop 0x248.
    bool irq2_asserted() const;

    // Channel 2's 0x8a (Reset main cpu): true once per request; the caller resets
    // the machine. Not saved: consumed within the CPU chunk that wrote it.
    bool consume_reset_request() { return std::exchange(reset_requested_, false); }

    // Inputs, not saved. `audio`: every track is audio (servo status 01 25).
    void set_disc_present(bool present, bool audio) {
        disc_present_ = present;
        disc_audio_ = audio;
    }

    // Player's own tray button or frontend eject; acts like the servo's 0xa6/0xa7.
    void set_tray_open(bool open);
    bool tray_open() const { return tray_open_; }
    // Only a fully closed tray lets the CDIC read the disc.
    bool tray_closed() const { return tray_state_ == slave::kTrayClosed; }

    // Configuration, set once before boot, not saved.
    void set_video_standard(VideoStandard standard) { video_standard_ = standard; }

    // Front socket: RTS on PB4, bytes into the SCI.
    bool front_rts_negated() const { return front_rts_negated_; }
    bool sci_at_9600_baud() const { return sci_9600_; }
    void sci_receive(uint8_t byte, bool framing_error);

    bool back_rts_negated() const { return pc6_ != Pc6::kLow; }

    // Call with each CPU chunk.
    void tick_cycles(int cpu_cycles);

    // Applies the SLAVE's AD7528 attenuators to the frame's CDIC audio, in place.
    // Changes land where they happened within the frame. Call once per frame.
    void attenuate(std::vector<int16_t>& interleaved_stereo);

    template <class T>
    void state_io(T& archive);

private:
    void handle_write(int channel, uint8_t v);
    void queue_response(int out_channel, const uint8_t* data, int len);
    // A 4-byte channel 3 command, as the servo handles it.
    void servo_command(const uint8_t cmd[4]);
    // Channel 2 0xf5 reply: the current master volume attenuation in dB (firmware 0xf7e).
    void queue_master_volume_reply();

    // Servo tray requests (firmware tables 0x1b83 open, 0x1b97 close).
    void tray_request_open();
    void tray_request_close();
    void enter_tray_state(uint8_t state);
    // b0 00 <disc type> <0x20 | tray state>, on channel 3 once 0xfa was sent.
    void report_disc_status();
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

    void output_compare_tick(); // 0x961, every 8.19 ms
    void start_detection(); // 0x254
    void device_lost(); // 0x756
    void relative_byte(uint8_t v);
    void absolute_byte(uint8_t v);
    void publish_pointer(); // 0xe0d
    bool loaded(); // checks and rebuilds what a load can't carry
};

} // namespace mamecdi
} // namespace zcdi

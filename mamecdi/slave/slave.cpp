#include "slave/slave.h"

#include "core/pointing_device.h"
#include "core/state_archive.h"
#include "core/util.h"

#include <algorithm>

namespace zcdi {
namespace mamecdi {

using namespace slave;

namespace {
constexpr uint32_t kSlaveBase = 0x310000;

// SLAVE firmware 2.0 tables: DAC code per dB (0xb37, muted from 45 dB) and the
// fade gain per $1a tick (0xb65).
constexpr uint8_t kDacCodeForDb[45] = {
    255, 228, 203, 181, 162, 144, 128, 114, 102, 91, 81, 72, 64, 57, 51, 46, 41, 36, 32, 29, 26, 23, 20,
    18,  16,  14,  13,  11,  10,  9,   8,   7,   6,  6,  5,  5,  4,  4,  3,  3,  3,  2,  2,  2,  2,
};
constexpr uint8_t kFadeTicks = 0x34;
constexpr uint8_t kFadeGain[kFadeTicks] = {
    255, 255, 254, 253, 251, 249, 246, 243, 240, 236, 232, 227, 222, 216, 210, 204, 198, 191,
    184, 177, 170, 162, 155, 147, 139, 131, 124, 116, 108, 100, 93,  85,  78,  71,  64,  57,
    51,  45,  39,  33,  28,  23,  19,  15,  12,  9,   6,   4,   2,   1,   0,   0,
};
constexpr uint8_t kMaxMasterDb = 0x2d;
constexpr unsigned kDimDb = 20;
constexpr uint8_t kForcedMuteDb = 0x3f; // beyond kDacCodeForDb: code 0

constexpr uint8_t kIdleByte = 0xff; // read from a channel with nothing queued

constexpr uint8_t kSlaveRevision = 0x20; // 2.0, in the kSysVersion reply
constexpr uint8_t kServoRevision = 0x20; // 2.x, in the kServoVersion reply

// Pointer position clamps (main loop 0xd90).
constexpr int kPointerMaxX = 0x2ff;
constexpr int kPointerMaxY = 0x22f;
} // namespace

SlaveHle::SlaveHle() {
    // Power-on DAC programming: straight paths 0 dB, cross paths muted.
    apply_dacs();
    reset_dac_window();
    start_detection(); // 0x1e7
}

template <class T>
void SlaveHle::state_io(T& archive) {
    archive.begin_section(state_tag("SLAV"));
    archive.io_struct(static_cast<SlaveSaveState&>(*this));
    archive.end_section();
    if constexpr (T::kLoading) archive.check(loaded());
}
ZCDI_STATE_IO_INSTANTIATE(SlaveHle);

bool SlaveHle::loaded() {
    reset_dac_window();
    for (const Channel& c : channels_) {
        if (c.in_len < 0 || c.in_len > int(sizeof(c.in))) return false;
        if (c.out_len < 0 || c.out_len > int(sizeof(c.out))) return false;
        if (c.out_pos < 0 || c.out_pos > c.out_len) return false;
    }
    if (tray_state_ > 9 || tray_timer_ < 0 || tray_timer_ > 255) return false;
    const uint8_t type = uint8_t(front_type_);
    if (type > uint8_t(FrontDeviceType::kAbsoluteScreen) && front_type_ != FrontDeviceType::kPowerOn) return false;
    if (pc6_ > Pc6::kLow || detect_ticks_ > 5 || packet_count_ > 3) return false;
    return timer_cycle_accum_ >= 0 && timer_cycle_accum_ < kTimerTickCycles && data_count_ < 4;
}

uint8_t SlaveHle::read_byte(uint32_t addr) {
    uint32_t off = addr - kSlaveBase;
    if ((off & 1) == 0) return 0; // high byte of the word: nothing meaningful here
    int channel = static_cast<int>(off >> 1);
    if (channel >= kNumChannels) return kIdleByte;

    Channel& c = channels_[channel];
    if (c.out_pos >= c.out_len) return kIdleByte; // matches MAME's uncommitted-read default

    uint8_t v = c.out[c.out_pos++];
    if (c.out_pos >= c.out_len) {
        c.out_len = 0;
        c.out_pos = 0;
    }
    return v;
}

void SlaveHle::write_byte(uint32_t addr, uint8_t v) {
    uint32_t off = addr - kSlaveBase;
    if ((off & 1) == 0) return;
    int channel = static_cast<int>(off >> 1);
    if (channel >= kNumChannels) return;
    handle_write(channel, v);
}

void SlaveHle::queue_response(int out_channel, const uint8_t* data, int len) {
    if (out_channel >= kNumChannels) return;
    Channel& c = channels_[out_channel];
    for (int i = 0; i < len && i < 4; ++i)
        c.out[i] = data[i];
    c.out_len = len;
    c.out_pos = 0;
}

bool SlaveHle::irq2_asserted() const {
    auto busy = [this](int channel) { return channels_[channel].out_pos < channels_[channel].out_len; };
    return (pointer_enabled_ && busy(kChannelPointer)) || (reply_irq_enabled_ && busy(kChannelControl)) ||
           (status_forwarding_ && busy(kChannelStatus));
}

void SlaveHle::queue_master_volume_reply() {
    uint8_t r[2] = {kSysMasterVolume, uint8_t(master_ & kMasterDb)};
    queue_response(kChannelControl, r, 2);
}

// As MAME slave_w: an unmatched byte resets the channel's input at once, and
// unknown opcodes get no answer.
void SlaveHle::handle_write(int channel, uint8_t v) {
    Channel& c = channels_[channel];

    if (c.in_len == 0) {
        switch (channel) {
            case kChannelPointer:
                // Set Pointer Position (cdichips mc6805slave.md), no response.
                if (v >= kPtrSetPosition) {
                    c.in[0] = v;
                    c.in_len = 1;
                    return;
                }
                if (v < 0x80) return;
                if ((v & 0x0f) == kPtrEnable) {
                    pointer_enabled_ = true;
                    return;
                }
                if ((v & 0x0f) == kPtrDisable) {
                    pointer_enabled_ = false;
                    return;
                }
                return; // anything else: immediately ignored, no state change
            case kChannelLcd: return; // no opcode starts a sequence on this channel
            case kChannelControl:
                // Bit 7 set = a command on its own (firmware 0x48d). 0xf0 (LCD) takes no
                // bytes here; its text comes on channel 1, unlike MAME's 16-byte form.
                if (level_mode_ || v < 0x80) {
                    channel2_data(v);
                    return;
                }
                if (v >= kCtlAttenuation) {
                    if (v < kCtlAttenuationEnd) {
                        att_command_ = v & 0x7f;
                        data_count_ = 0;
                    }
                    return; // 0xd0-0xff (LCD 0xf0 included) set state we don't use
                }
                switch (v) {
                    case kCtlMute: // fading out
                        if (muted_) return;
                        muted_ = true;
                        start_fade();
                        return;
                    case kCtlUnmute: // fading in
                    case kCtlUnmutePc7:
                        if (!muted_) return;
                        muted_ = false;
                        start_fade();
                        return;
                    case kCtlMasterLouder: // start ramping
                    case kCtlMasterSofter:
                        if (!master_enabled_) return;
                        if (v == kCtlMasterLouder) {
                            master_ = uint8_t((master_ & kMasterDb) | kMasterRampLouder);
                            if (master_ & kMasterDb) --master_;
                        } else {
                            master_ = uint8_t((master_ & kMasterDb) | kMasterRampSofter);
                            if ((master_ & kMasterDb) < kMaxMasterDb) ++master_;
                        }
                        ramp_count_ = ramp_reload_;
                        dim_ = false;
                        apply_dacs();
                        return;
                    case kCtlMasterStop:
                        master_ &= kMasterDb;
                        queue_master_volume_reply();
                        return;
                    case kCtlMasterToggle: master_enabled_ = !master_enabled_; return;
                    // To the servo as A6/A7/A8 (command table 0x51d; A8 stops the disc and opens,
                    // cdapdriv SetStat 0x54 sends 0x93); its disc status reports are forwarded.
                    case kCtlTrayOpen:
                    case kCtlTrayClose:
                    case kCtlStopAndOpen: {
                        const uint8_t cmd[4] = {v == kCtlTrayOpen    ? kServoTrayOpen
                                                : v == kCtlTrayClose ? kServoTrayClose
                                                                     : kServoStopAndOpen,
                                                0, 0, 0};
                        servo_command(cmd);
                        return;
                    }
                    case kCtlResetCpu: reset_requested_ = true; return;
                    case kCtlDim:
                        dim_ = !dim_;
                        apply_dacs();
                        return;
                    case kCtlMasterLevel:
                        if (!master_enabled_) return;
                        level_mode_ = true;
                        data_count_ = 0;
                        return;
                    default: return;
                }
            case kChannelStatus:
                switch (v) {
                    case kSysVersion: {
                        // Then the servo's 0xa3 byte (firmware 0xf19).
                        uint8_t r[3] = {kSysVersion, kSlaveRevision, servo_version_};
                        queue_response(kChannelControl, r, 3);
                        return;
                    }
                    case kSysFrontDevice: { // 0x457
                        uint8_t r[2] = {kSysFrontDevice, uint8_t(front_type_)};
                        queue_response(kChannelControl, r, 2);
                        return;
                    }
                    case kSysTestPlug: {
                        uint8_t r[2] = {kSysTestPlug, 0x00};
                        queue_response(kChannelControl, r, 2);
                        return;
                    }
                    case kSysMasterVolume: queue_master_volume_reply(); return;
                    case kSysVideoStandard: {
                        // Firmware 0xf9a: ((RAM $5a & 4) >> 2) + 1 from the STAND pin; the BIOS
                        // polls it before 0xf8 and programs 50 Hz for 2 (cdi220b 0x400f16).
                        uint8_t r[2] = {kSysVideoStandard, static_cast<uint8_t>(video_standard_)};
                        queue_response(kChannelControl, r, 2);
                        return;
                    }
                    case kSysReplyIrqOn: // sldriv init
                        reply_irq_enabled_ = true;
                        return;
                    case kSysReplyIrqOff: reply_irq_enabled_ = false; return;
                    case kSysForwardOn: // forward the servo's disc status messages
                        status_forwarding_ = true;
                        return;
                    case kSysForwardOff: status_forwarding_ = false; return;
                    case kSysPc6High:
                        pc6_ticks_ = 2;
                        pc6_ = Pc6::kHigh;
                        return;
                    case kSysPc6Release: pc6_ = Pc6::kReleased; return;
                    default:
                        // All but 0x80/0x81 go to the servo (firmware 0x3da), e.g. through cdapdriv SetStat 0x52.
                        if (v < kSysFourByte || v >= kSysVersion) return;
                        c.in[0] = v;
                        c.in_len = 1;
                        return;
                }
        }
        return;
    }

    // Continuation of channel 0's Set Pointer Position or a channel 3 command.
    if (c.in_len < static_cast<int>(sizeof(c.in))) c.in[c.in_len++] = v;

    if (channel == kChannelStatus) {
        if (c.in_len == 4) {
            c.in_len = 0;
            servo_command(c.in);
        }
        return;
    }
    if (channel == kChannelPointer && c.in_len == 3) {
        // Set Pointer Position (0x39d, 0xe5c).
        const uint8_t b0 = c.in[0];
        const uint8_t b1 = c.in[1];
        const uint8_t b2 = c.in[2];
        pointer_x_ = int16_t(((b1 & 0x70) << 3) | (b2 & 0x7f));
        pointer_y_ = int16_t(((b1 & 0x0f) << 6) | (b0 & 0x3f));
        channels_[kChannelPointer].out_len = 0;
        channels_[kChannelPointer].out_pos = 0;
        c.in_len = 0;
        return;
    }
}

// SERVO firmware 2.1 command dispatch (0x1698); its replies reach channel 3 only after 0xfa.
void SlaveHle::servo_command(const uint8_t cmd[4]) {
    switch (cmd[0]) {
        case kServoDiscStatus: // answered on the next poll
            report_disc_status();
            return;
        case kServoTrayOpen: // unless open or opening
            if (tray_state_ <= kTrayClosed && tray_state_ != kTrayOpen) tray_request_open();
            return;
        case kServoStopAndOpen: // whatever the tray is doing
            tray_request_open();
            return;
        case kServoTrayClose: // unless closed
            if (tray_state_ != kTrayClosed) tray_request_close();
            return;
        case kServoVersion: { // 0x1833; the SLAVE keeps byte 1 either way (0xc55)
            uint8_t r[4] = {kServoVersion, kServoRevision, 0x00, 0x00};
            servo_version_ = r[1];
            if (status_forwarding_) queue_response(kChannelStatus, r, 4);
            return;
        }
        case kServoEcho: { // 0x1863
            uint8_t r[4] = {kServoEcho, cmd[1], cmd[2], cmd[3]};
            if (status_forwarding_) queue_response(kChannelStatus, r, 4);
            return;
        }
        default: return; // 0x80/0x81 stay in the SLAVE; b1 has no reply without a second session
    }
}

void SlaveHle::set_tray_open(bool open) {
    if (open == tray_open_) return;
    const uint8_t cmd[4] = {open ? kServoTrayOpen : kServoTrayClose, 0, 0, 0};
    servo_command(cmd);
}

namespace {
// Frames each tray state lasts before the next (firmware 0x1ad7, 8.2 ms ticks). Fixed in the
// firmware: 9 (4 ticks), 4 (10 ticks), 2 (14 ticks or more). The others wait on the disc
// stopping or the tray switches, so they are guesses for a ~1 s move; 5 is the TOC read.
constexpr uint8_t kTrayStepFrames[10] = {0, 0, 6, 30, 4, 50, 10, 10, 30, 2};
constexpr uint8_t kTrayNextState[10] = {0, 1, 3, 4, 5, 5, 7, 8, 9, 1};
} // namespace

// Open request by tray state (table 0x1b83): closed starts kTrayOpening; moving trays reverse to kTrayClosing.
void SlaveHle::tray_request_open() {
    static constexpr uint8_t kTo[10] = {7, 2, 7, 7, 7, 6, 0, 2, 2, 2};
    if (kTo[tray_state_]) enter_tray_state(kTo[tray_state_]);
}

// Close request by tray state (table 0x1b97): 6 has not moved yet, so it is closed again.
void SlaveHle::tray_request_close() {
    static constexpr uint8_t kTo[10] = {2, 2, 0, 0, 0, 0, 5, 2, 2, 2};
    if (kTo[tray_state_]) enter_tray_state(kTo[tray_state_]);
}

void SlaveHle::enter_tray_state(uint8_t state) {
    tray_state_ = state;
    tray_open_ = state == kTrayOpen || state >= kTrayOpening;
    tray_timer_ = kTrayStepFrames[state];
    report_disc_status();
}

// The servo sends this whenever it changes (0x178b), the SLAVE forwards it after 0xfa.
void SlaveHle::report_disc_status() {
    // Type 00 until the TOC is read (0x1acb clears it), then 01 audio, 03 no disc.
    // Servo 2.1 gives 0x02 for a CD-i TOC (disc type 0x10, Green Book VII.10.1.a), as our CDIC TOC
    // says; kept at 0x04 as measured on a 210/05 (CDi_MiSTer). The BIOS treats 0x02 and 0x04 alike.
    uint8_t type = kDiscUnknown;
    if (tray_state_ == kTrayClosed && tray_timer_ == 0)
        type = !disc_present_ ? kDiscNone : disc_audio_ ? kDiscAudio : kDiscCdi;
    uint8_t r[4] = {kServoDiscStatus, 0x00, type, uint8_t(0x20 | tray_state_)};
    if (status_forwarding_) queue_response(kChannelStatus, r, 4);
}

void SlaveHle::tick_frame() {
    if (tray_timer_ == 0 || --tray_timer_ != 0) return;
    if (tray_state_ == kTrayClosed) {
        report_disc_status(); // the TOC is read: the disc type is known
        return;
    }
    enter_tray_state(kTrayNextState[tray_state_]);
}

void SlaveHle::tick_cycles(int cpu_cycles) {
    // Timer ticks run at their own cycle so DAC changes land there in the attenuate() window.
    int left = cpu_cycles;
    while (timer_cycle_accum_ + left >= kTimerTickCycles) {
        const int step = kTimerTickCycles - timer_cycle_accum_;
        window_cycles_ += step;
        left -= step;
        timer_cycle_accum_ = 0;
        timer_tick();
    }
    timer_cycle_accum_ += left;
    window_cycles_ += left;
}

// Firmware main loop fade step (0xaa6).
void SlaveHle::fade_step() {
    if (uint8_t(timer_high_ - fade_start_) >= kFadeTicks) fading_ = false;
    apply_dacs();
}

// Only read while fading_, which fade_step() clears once the table runs out.
uint8_t SlaveHle::fade_gain() const {
    const uint8_t elapsed = uint8_t(timer_high_ - fade_start_);
    if (elapsed >= kFadeTicks) return 0;
    return kFadeGain[muted_ ? elapsed : kFadeTicks - 1 - elapsed];
}

// $1a tick.
void SlaveHle::timer_tick() {
    ++timer_high_;
    if (fading_) fade_step();
    if ((timer_high_ & 0x0f) == 0) output_compare_tick();
}

void SlaveHle::output_compare_tick() {
    if (detect_ticks_ != 0) {
        if (--detect_ticks_ == 0) {
            sci_receiving_ = true;
            front_rts_negated_ = false;
        }
    }
    if (pc6_ticks_ != 0 && --pc6_ticks_ == 0) pc6_ = Pc6::kLow;

    // Master volume ramp (0x9c7).
    if (!master_enabled_) return;
    if (ramp_count_ != 0) {
        --ramp_count_;
        return;
    }
    ramp_count_ = ramp_reload_;
    if (master_ & kMasterRampLouder) {
        if (master_ & kMasterDb) {
            --master_;
            apply_dacs();
        }
    } else if (master_ & kMasterRampSofter) {
        if ((master_ & kMasterDb) < kMaxMasterDb) {
            ++master_;
            apply_dacs();
        }
    }
}

// Mute/unmute (0x553/0x571); a running fade turns around from its current gain.
void SlaveHle::start_fade() {
    const uint8_t elapsed = uint8_t(timer_high_ - fade_start_);
    if (fading_ && elapsed < kFadeTicks) {
        fade_start_ = uint8_t(timer_high_ - (kFadeTicks - elapsed));
    } else {
        fade_start_ = timer_high_;
    }
    fading_ = true;
    fade_step();
}

// 0x90's level/ramp bytes (0x4b8) or a 0xc0-0xcf command's 4 dB bytes (0x498).
void SlaveHle::channel2_data(uint8_t v) {
    ++data_count_;
    if (level_mode_) {
        if (data_count_ == 1) {
            if (v == kCtlLevelUnchanged) return;
            ramp_count_ = 0;
            if ((v & 0xc0) == 0x00) {
                master_ = uint8_t((v & kMasterDb) | kMasterRampLouder);
            } else if ((v & 0xc0) == 0x40) {
                master_ = uint8_t((v & kMasterDb) | kMasterRampSofter);
            } else if (master_ & kMasterRamp) {
                master_ &= kMasterDb; // a ramp was running: only stop it
                queue_master_volume_reply();
                return;
            } else {
                master_ = v & kMasterDb;
                dim_ = false;
                apply_dacs();
                queue_master_volume_reply();
                return;
            }
            dim_ = false;
            apply_dacs();
            return;
        }
        if (data_count_ == 2 && v != kCtlLevelUnchanged) ramp_reload_ = v;
        level_mode_ = false;
        data_count_ = 0;
        return;
    }
    att_db_[data_count_ - 1] = v;
    if (data_count_ < 4) return;
    data_count_ = 0;
    apply_dacs();
}

// Firmware 0xafe.
uint8_t SlaveHle::dac_code(uint8_t db, bool forced_mute) const {
    unsigned total = (forced_mute ? kForcedMuteDb : db) + (master_ & kMasterDb) + (dim_ ? kDimDb : 0u);
    if (total >= sizeof(kDacCodeForDb)) return 0;
    const uint8_t code = kDacCodeForDb[total];
    if (fading_) return uint8_t((code * fade_gain()) >> 8);
    return muted_ ? 0 : code;
}

// Firmware 0xabf; the command's low nibble mutes paths, bit 3 the first.
void SlaveHle::apply_dacs() {
    uint8_t dac[4];
    for (int i = 0; i < 4; ++i)
        dac[i] = dac_code(att_db_[i], (att_command_ >> (3 - i)) & 1);
    if (std::equal(dac, dac + 4, dac_)) return;
    std::copy(dac, dac + 4, dac_);
    // Cap the change list if attenuate() is never called.
    if (dac_changes_.size() >= 4096) reset_dac_window();
    DacChange change{window_cycles_, {}};
    std::copy(dac, dac + 4, change.dac);
    dac_changes_.push_back(change);
}

void SlaveHle::attenuate(std::vector<int16_t>& interleaved_stereo) {
    const std::size_t frames = interleaved_stereo.size() / 2;
    uint8_t dac[4];
    std::copy(window_start_dac_, window_start_dac_ + 4, dac);
    std::size_t next = 0;
    for (std::size_t f = 0; f < frames; ++f) {
        const int64_t cycle = window_cycles_ * int64_t(f) / int64_t(frames);
        while (next < dac_changes_.size() && dac_changes_[next].cycle <= cycle) {
            std::copy(dac_changes_[next].dac, dac_changes_[next].dac + 4, dac);
            ++next;
        }
        const int l = interleaved_stereo[f * 2];
        const int r = interleaved_stereo[f * 2 + 1];
        // Straight plus cross path; an AD7528 passes code/256, scaled 9/16 for
        // the 210/05's headroom as MPEG audio is (dual_ad7528.sv scaled_shiftreg).
        auto gain = [&](int i) { return dac[i] * 9 / 16; };
        interleaved_stereo[f * 2] = clamp16((l * gain(0) + r * gain(3)) >> 8);
        interleaved_stereo[f * 2 + 1] = clamp16((r * gain(2) + l * gain(1)) >> 8);
    }
    reset_dac_window();
}

void SlaveHle::reset_dac_window() {
    std::copy(dac_, dac_ + 4, window_start_dac_);
    dac_changes_.clear();
    window_cycles_ = 0;
}

void SlaveHle::start_detection() {
    if (flip_baud_) {
        flip_baud_ = false;
        sci_9600_ = !sci_9600_; // 0x25c
    }
    sci_receiving_ = false;
    front_rts_negated_ = true;
    detect_ticks_ = 5;
}

void SlaveHle::device_lost() {
    front_type_ = FrontDeviceType::kNone;
    flip_baud_ = true;
    sync_lost_once_ = false;
    start_detection();
}

// SCI interrupt (0x6f9).
void SlaveHle::sci_receive(uint8_t byte, bool framing_error) {
    if (!sci_receiving_) return;
    if (framing_error) {
        device_lost();
        return;
    }
    const uint8_t v = byte & 0x7f;
    switch (front_type_) {
        case FrontDeviceType::kNone:
            switch (DeviceId(v)) {
                case DeviceId::kRelative: front_type_ = FrontDeviceType::kRelative; return;
                case DeviceId::kManeuvering: front_type_ = FrontDeviceType::kManeuvering; return;
                case DeviceId::kAbsoluteCoordinate: front_type_ = FrontDeviceType::kAbsoluteCoordinate; return;
                case DeviceId::kAbsoluteScreen: front_type_ = FrontDeviceType::kAbsoluteScreen; return;
                default: device_lost(); return;
            }
        case FrontDeviceType::kRelative:
        case FrontDeviceType::kManeuvering: relative_byte(v); return;
        case FrontDeviceType::kAbsoluteCoordinate:
        case FrontDeviceType::kAbsoluteScreen: absolute_byte(v); return;
        default: device_lost(); return;
    }
}

namespace {
// 0x7a8.
uint8_t report_buttons(uint8_t v) {
    return uint8_t((v & kPacketButton2 ? kReportButton2 : kReportNone) |
                   (v & kPacketButton1 ? kReportButton1 : kReportNone));
}
} // namespace

// 0x781.
void SlaveHle::relative_byte(uint8_t v) {
    const uint8_t count = packet_count_++;
    if (v & kPacketSync) {
        packet_count_ = 0;
        if (count == 2) {
            sync_lost_once_ = false;
        } else if (sync_lost_once_) {
            device_lost();
            return;
        } else {
            sync_lost_once_ = true;
        }
        packet_buttons_ = report_buttons(v);
        packet_x_ = uint16_t((v & 0x03) << 6);
        packet_y_ = uint16_t((v & 0x0c) << 4);
        return;
    }
    if (count == 0) {
        packet_x_ |= v;
        return;
    }
    if (count != 1) {
        device_lost(); // 0x7c4
        return;
    }
    packet_y_ |= v;
    // Main loop 0xd90.
    pointer_x_ = int16_t(std::clamp(pointer_x_ + int8_t(packet_x_), 0, kPointerMaxX));
    pointer_y_ = int16_t(std::clamp(pointer_y_ + int8_t(packet_y_), 0, kPointerMaxY));
    buttons_ = packet_buttons_;
    publish_pointer();
}

// 0x719.
void SlaveHle::absolute_byte(uint8_t v) {
    const uint8_t count = packet_count_++;
    if (v & kPacketSync) {
        packet_count_ = 0;
        if (count == 0) {
            sync_lost_once_ = false;
        } else if (sync_lost_once_) {
            device_lost();
            return;
        } else {
            sync_lost_once_ = true;
        }
        packet_buttons_ = report_buttons(v);
        packet_x_ = uint16_t((v & 0x0f) << 6);
        return;
    }
    switch (count) {
        case 0:
            if (v >= 0x20) packet_buttons_ |= 0x08;
            packet_y_ = uint16_t((v & 0x0f) << 6);
            return;
        case 1: {
            const int x = packet_x_ | (v & 0x3f);
            packet_x_ = uint16_t((x >> 1) + (x >> 2));
            return;
        }
        case 2: {
            const int y = packet_y_ | (v & 0x3f);
            pointer_x_ = int16_t(packet_x_);
            pointer_y_ = int16_t((y >> 1) + (y >> 4) + (y >> 5));
            buttons_ = packet_buttons_;
            packet_count_ = 0;
            publish_pointer();
            return;
        }
        default: device_lost(); return;
    }
}

void SlaveHle::publish_pointer() {
    const uint8_t report[4] = {
        uint8_t(0x88 | buttons_ | ((pointer_x_ >> 7) & 0x07)),
        uint8_t(pointer_x_ & 0x7f),
        uint8_t((pointer_y_ >> 7) & 0x07),
        uint8_t(pointer_y_ & 0x7f),
    };
    queue_response(kChannelPointer, report, 4);
}

} // namespace mamecdi
} // namespace zcdi

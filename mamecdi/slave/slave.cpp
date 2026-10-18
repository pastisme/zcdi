#include "slave/slave.h"

#include "core/state_archive.h"
#include "core/util.h"

#include <algorithm>

namespace zcdi {
namespace mamecdi {

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

} // namespace

SlaveHle::SlaveHle() {
    // Power-on DAC programming: straight paths 0 dB, cross paths muted.
    apply_dacs();
    reset_dac_window();
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
    return timer_cycle_accum_ >= 0 && timer_cycle_accum_ < kTimerTickCycles && data_count_ < 4;
}

uint8_t SlaveHle::read_byte(uint32_t addr) {
    uint32_t off = addr - kSlaveBase;
    if ((off & 1) == 0) return 0; // high byte of the word: nothing meaningful here
    int channel = static_cast<int>(off >> 1);
    if (channel >= kNumChannels) return 0xff;

    Channel& c = channels_[channel];
    if (c.out_pos >= c.out_len) return 0xff; // idle -- matches MAME's uncommitted-read default

    uint8_t v = c.out[c.out_pos++];
    if (c.out_pos == 1) irq2_ = false; // first read of a response acknowledges it
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
    if (out_channel != 2 || reply_irq_enabled_) irq2_ = true;
}

void SlaveHle::queue_master_volume_reply() {
    uint8_t r[2] = {0xf5, uint8_t(master_ & 0x3f)};
    queue_response(2, r, 2);
}

// As MAME slave_w: an unmatched byte resets the channel's input at once, and
// unknown opcodes get no answer.
void SlaveHle::handle_write(int channel, uint8_t v) {
    Channel& c = channels_[channel];

    if (c.in_len == 0) {
        switch (channel) {
            case 0:
                // Set Pointer Position (cdichips mc6805slave.md), no response.
                if (v >= 0xc0) {
                    c.in[0] = v;
                    c.in_len = 1;
                    return;
                }
                // Enable/disable pointer input; the BIOS sends 0x83 after installing its handler (cdi220b 0x431b18).
                // 0x80-0xbf decode on the low nibble alone (firmware 0x3b6), so 0x93 is 0x83.
                if (v < 0x80) return;
                if ((v & 0x0f) == 0x03) {
                    pointer_enabled_ = true;
                    have_reported_ = false;
                    return;
                }
                if ((v & 0x0f) == 0x04) {
                    pointer_enabled_ = false;
                    return;
                }
                return; // anything else: immediately ignored, no state change
            case 1: return; // no opcode starts a sequence on this channel
            case 2:
                // Bit 7 set = a command on its own (firmware 0x48d). 0xf0 (LCD) takes no
                // bytes here; its text comes on channel 1, unlike MAME's 16-byte form.
                if (level_mode_ || v < 0x80) {
                    channel2_data(v);
                    return;
                }
                if (v >= 0xc0) {
                    if (v < 0xd0) {
                        att_command_ = v & 0x7f;
                        data_count_ = 0;
                    }
                    return; // 0xd0-0xff (0xf0 included) set state we don't use
                }
                switch (v) {
                    case 0x82: // mute, fading out
                        if (muted_) return;
                        muted_ = true;
                        start_fade();
                        return;
                    case 0x83: // unmute, fading in; 0x84 also sets port C bit 7,
                    case 0x84: // which drives nothing audible
                        if (!muted_) return;
                        muted_ = false;
                        start_fade();
                        return;
                    case 0x85: // master volume: start ramping louder
                    case 0x86: // ... softer
                        if (!master_enabled_) return;
                        if (v == 0x85) {
                            master_ = uint8_t((master_ & 0x3f) | 0x40);
                            if (master_ & 0x3f) --master_;
                        } else {
                            master_ = uint8_t((master_ & 0x3f) | 0x80);
                            if ((master_ & 0x3f) < kMaxMasterDb) ++master_;
                        }
                        ramp_count_ = ramp_reload_;
                        dim_ = false;
                        apply_dacs();
                        return;
                    case 0x87: // stop the ramp
                        master_ &= 0x3f;
                        queue_master_volume_reply();
                        return;
                    case 0x88: master_enabled_ = !master_enabled_; return;
                    // To the servo as A6/A7/A8 (command table 0x51d; A8 stops the disc and opens,
                    // cdapdriv SetStat 0x54 sends 0x93); its disc status reports are forwarded.
                    case 0x89:
                    case 0x92:
                    case 0x93: {
                        const uint8_t cmd[4] = {uint8_t(v == 0x89 ? 0xa6 : v == 0x92 ? 0xa7 : 0xa8), 0, 0, 0};
                        servo_command(cmd);
                        return;
                    }
                    case 0x8a: // Reset main cpu
                        reset_requested_ = true;
                        return;
                    case 0x8f: // toggle the 20 dB dim
                        dim_ = !dim_;
                        apply_dacs();
                        return;
                    case 0x90: // master level and ramp rate follow
                        if (!master_enabled_) return;
                        level_mode_ = true;
                        data_count_ = 0;
                        return;
                    default: return;
                }
            case 3:
                switch (v) {
                    case 0xf0: {
                        // Revision 2.0, then the servo's 0xa3 byte (firmware 0xf19).
                        uint8_t r[3] = {0xf0, 0x20, servo_version_};
                        queue_response(2, r, 3);
                        return;
                    }
                    // The real SLAVE firmware (as CDi_MiSTer runs it) replies 2; MAME's HLE guesses 1.
                    case 0xf3: {
                        uint8_t r[2] = {0xf3, 0x02};
                        queue_response(2, r, 2);
                        return;
                    } // pointer type
                    case 0xf4: {
                        uint8_t r[2] = {0xf4, 0x00};
                        queue_response(2, r, 2);
                        return;
                    } // test plug
                    case 0xf5: queue_master_volume_reply(); return;
                    case 0xf6: {
                        // Firmware 0xf9a: ((RAM $5a & 4) >> 2) + 1 from the STAND pin; the BIOS
                        // polls it before 0xf8 and programs 50 Hz for 2 (cdi220b 0x400f16).
                        uint8_t r[2] = {0xf6, static_cast<uint8_t>(video_standard_)};
                        queue_response(2, r, 2);
                        return;
                    }
                    case 0xf8: // sldriv init
                        reply_irq_enabled_ = true;
                        if (channels_[2].out_pos < channels_[2].out_len) irq2_ = true;
                        return;
                    case 0xf9: reply_irq_enabled_ = false; return;
                    case 0xfa: // forward the servo's messages (disc status)
                        status_forwarding_ = true;
                        return;
                    case 0xfb: // stop forwarding them
                        status_forwarding_ = false;
                        return;
                    default:
                        // 0x80-0xef start a 4-byte command; all but 0x80/0x81 go to the servo
                        // (firmware 0x3da), e.g. through cdapdriv SetStat 0x52.
                        if (v < 0x80 || v >= 0xf0) return;
                        c.in[0] = v;
                        c.in_len = 1;
                        return;
                }
        }
        return;
    }

    // Continuation of channel 0's Set Pointer Position or a channel 3 command.
    if (c.in_len < static_cast<int>(sizeof(c.in))) c.in[c.in_len++] = v;

    if (channel == 3) {
        if (c.in_len == 4) {
            c.in_len = 0;
            servo_command(c.in);
        }
        return;
    }
    if (channel == 0 && c.in_len == 3) {
        // Bit layout as same_cdi set_mouse_position(): seeds the position CD-RTOS drew the cursor at.
        const uint8_t b0 = c.in[0];
        const uint8_t b1 = c.in[1];
        const uint8_t b2 = c.in[2];
        set_pointer_position(((b1 & 0x70) << 3) | (b2 & 0x7f), ((b1 & 0x0f) << 6) | (b0 & 0x3f));
        c.in_len = 0;
        return;
    }
}

// SERVO firmware 2.1 command dispatch (0x1698); its replies reach channel 3 only after 0xfa.
void SlaveHle::servo_command(const uint8_t cmd[4]) {
    switch (cmd[0]) {
        case 0xb0: // Request Disc Status: answered on the next poll
            report_disc_status();
            return;
        case 0xa6: // open the tray, unless open or opening
            if (tray_state_ <= 5 && tray_state_ != 1) tray_request_open();
            return;
        case 0xa8: // stop the disc and open, whatever the tray is doing
            tray_request_open();
            return;
        case 0xa7: // close it, unless closed
            if (tray_state_ != 5) tray_request_close();
            return;
        case 0xa3: { // version 2.x (0x1833); the SLAVE keeps byte 1 either way (0xc55)
            uint8_t r[4] = {0xa3, 0x20, 0x00, 0x00};
            servo_version_ = r[1];
            if (status_forwarding_) queue_response(3, r, 4);
            return;
        }
        case 0xa4: { // echo (0x1863)
            uint8_t r[4] = {0xa4, cmd[1], cmd[2], cmd[3]};
            if (status_forwarding_) queue_response(3, r, 4);
            return;
        }
        default: return; // 0x80/0x81 stay in the SLAVE; b1 has no reply without a second session
    }
}

void SlaveHle::set_tray_open(bool open) {
    if (open == tray_open_) return;
    const uint8_t cmd[4] = {uint8_t(open ? 0xa6 : 0xa7), 0, 0, 0};
    servo_command(cmd);
}

namespace {
// Frames each tray state lasts before the next (firmware 0x1ad7, 8.2 ms ticks). Fixed in the
// firmware: 9 (4 ticks), 4 (10 ticks), 2 (14 ticks or more). The others wait on the disc
// stopping or the tray switches, so they are guesses for a ~1 s move; 5 is the TOC read.
constexpr uint8_t kTrayStepFrames[10] = {0, 0, 6, 30, 4, 50, 10, 10, 30, 2};
constexpr uint8_t kTrayNextState[10] = {0, 1, 3, 4, 5, 5, 7, 8, 9, 1};
} // namespace

// Open request by tray state (table 0x1b83): closed starts at 6; moving trays reverse to 2.
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
    tray_open_ = state == 1 || state >= 6;
    tray_timer_ = kTrayStepFrames[state];
    report_disc_status();
}

// The servo sends this whenever it changes (0x178b), the SLAVE forwards it after 0xfa.
void SlaveHle::report_disc_status() {
    // Type 00 until the TOC is read (0x1acb clears it), then 01 audio, 03 no disc.
    // Servo 2.1 gives 0x02 for a CD-i TOC (disc type 0x10, Green Book VII.10.1.a), as our CDIC TOC
    // says; kept at 0x04 as measured on a 210/05 (CDi_MiSTer). The BIOS treats 0x02 and 0x04 alike.
    uint8_t type = 0x00;
    if (tray_state_ == 5 && tray_timer_ == 0) type = !disc_present_ ? 0x03 : disc_audio_ ? 0x01 : 0x04;
    uint8_t r[4] = {0xb0, 0x00, type, uint8_t(0x20 | tray_state_)};
    if (status_forwarding_) queue_response(3, r, 4);
}

void SlaveHle::tick_frame() {
    if (tray_timer_ == 0 || --tray_timer_ != 0) return;
    if (tray_state_ == 5) {
        report_disc_status(); // the TOC is read: the disc type is known
        return;
    }
    enter_tray_state(kTrayNextState[tray_state_]);
}

void SlaveHle::update_pointer(int dx, int dy, bool button1, bool button2, bool button12) {
    set_pointer(pointer_x_ + dx, pointer_y_ + dy, button1, button2, button12);
}

void SlaveHle::set_pointer_position(int x, int y) {
    pointer_x_ = int16_t(std::clamp(x, 0, 767));
    pointer_y_ = int16_t(std::clamp(y, 0, 559));
}

void SlaveHle::set_pointer(int x, int y, bool button1, bool button2, bool button12) {
    set_pointer_position(x, y);
    uint8_t button_bits = 0x01;
    if (button1) button_bits |= 0x02;
    if (button2) button_bits |= 0x04;
    if (button12) button_bits |= 0x06;
    button_bits_ = button_bits;
}

void SlaveHle::tick_cycles(int cpu_cycles) {
    slot_cycle_accum_ += cpu_cycles;
    while (slot_cycle_accum_ >= kPointerSlotCycles) {
        slot_cycle_accum_ -= kPointerSlotCycles;
        on_pointer_slot();
    }
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

// $1a tick: fade step; every 16 ticks the output compare master ramp (0x9c7).
void SlaveHle::timer_tick() {
    ++timer_high_;
    if (fading_) fade_step();
    if ((timer_high_ & 0x0f) != 0 || !master_enabled_) return;
    if (ramp_count_ != 0) {
        --ramp_count_;
        return;
    }
    ramp_count_ = ramp_reload_;
    if (master_ & 0x40) {
        if (master_ & 0x3f) {
            --master_;
            apply_dacs();
        }
    } else if (master_ & 0x80) {
        if ((master_ & 0x3f) < kMaxMasterDb) {
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
            if (v == 0xff) return;
            ramp_count_ = 0;
            if ((v & 0xc0) == 0x00) {
                master_ = uint8_t((v & 0x3f) | 0x40);
            } else if ((v & 0xc0) == 0x40) {
                master_ = uint8_t((v & 0x3f) | 0x80);
            } else if (master_ & 0xc0) {
                master_ &= 0x3f; // a ramp was running: only stop it
                queue_master_volume_reply();
                return;
            } else {
                master_ = v & 0x3f;
                dim_ = false;
                apply_dacs();
                queue_master_volume_reply();
                return;
            }
            dim_ = false;
            apply_dacs();
            return;
        }
        if (data_count_ == 2 && v != 0xff) ramp_reload_ = v;
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
    unsigned total = (forced_mute ? 0x3f : db) + (master_ & 0x3fu) + (dim_ ? 0x14u : 0u);
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

void SlaveHle::on_pointer_slot() {
    if (report_slots_left_ > 0) {
        if (--report_slots_left_ == 0) {
            // Byte 0 has bit 7 set (firmware 0xe27: 0x88 | buttons | x high bits).
            const uint8_t report[4] = {
                uint8_t(0x80 | ((reported_x_ & 0x380) >> 7) | (reported_button_bits_ << 3)),
                uint8_t(reported_x_ & 0x7f),
                uint8_t((reported_y_ & 0x380) >> 7),
                uint8_t(reported_y_ & 0x7f),
            };
            queue_response(0, report, 4);
        }
        return;
    }
    if (!pointer_enabled_) return;
    if (have_reported_ && button_bits_ == reported_button_bits_ && pointer_x_ == reported_x_ &&
        pointer_y_ == reported_y_) {
        return;
    }
    have_reported_ = true;
    reported_button_bits_ = button_bits_;
    reported_x_ = pointer_x_;
    reported_y_ = pointer_y_;
    report_slots_left_ = 2; // first byte goes out on this slot
}

} // namespace mamecdi
} // namespace zcdi

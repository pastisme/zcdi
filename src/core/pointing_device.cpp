#include "core/pointing_device.h"

#include <algorithm>

#include "scc68070/scc68070.h"

namespace zcdi {
namespace {

// 1200 baud, 10 bits a byte.
constexpr int kCyclesPerByte = mamecdi::kCpuClockHz / 120;
constexpr int kPowerOnCycles = mamecdi::kCpuClockHz / 10;

int saturate(int v) { return std::clamp(v, -127, 127); }

} // namespace

PointingDevice::PointingDevice(DeviceId id) : id_(id) { cycles_to_next_ = kPowerOnCycles; }

template <class T>
void PointingDevice::state_io(T& archive) {
    archive.begin_section(state_tag("PDEV"));
    archive.io_struct(static_cast<PointingDeviceSaveState&>(*this));
    archive.end_section();
    if constexpr (T::kLoading) {
        // packet_pos_ is stale once a packet is out; it's only read while sending.
        archive.check(state_ <= kSending && packet_len_ <= sizeof(packet_) &&
                      (state_ != kSending || packet_pos_ < packet_len_));
        archive.check(accel_ >= 0 && accel_ <= 7);
    }
}
ZCDI_STATE_IO_INSTANTIATE(PointingDevice);

void PointingDevice::set_input(const Input& input) {
    input_ = input;
    mouse_acc_x_ += input.mouse_dx;
    mouse_acc_y_ += input.mouse_dy;
}

void PointingDevice::relative_packet() {
    const bool digital = input_.left || input_.right || input_.up || input_.down;
    const bool analog = input_.analog_dx != 0 || input_.analog_dy != 0;
    int x = 0, y = 0;
    if (analog) {
        x = input_.analog_dx;
        y = input_.analog_dy;
    } else if (digital) {
        const int speed = gamepad_speed_n_step(accel_ + 1);
        if (input_.right) x = speed;
        if (input_.down) y = speed;
        if (input_.left) x = -speed;
        if (input_.up) y = -speed;
    }
    x = saturate(x + mouse_acc_x_);
    y = saturate(y + mouse_acc_y_);
    const bool b1 = input_.button1, b2 = input_.button2;
    if (initial_packet_ || b1 != b1_q_ || b2 != b2_q_ || x != x_q_ || y != y_q_ || x != 0 || y != 0) {
        const uint8_t ux = uint8_t(x), uy = uint8_t(y);
        packet_[0] = uint8_t(0x40 | (b1 ? 0x20 : 0) | (b2 ? 0x10 : 0) | (uy >> 6 & 3) << 2 | (ux >> 6 & 3));
        packet_[1] = uint8_t(ux & 0x3f);
        packet_[2] = uint8_t(uy & 0x3f);
        packet_len_ = 3;
        x_q_ = x;
        y_q_ = y;
        b1_q_ = b1;
        b2_q_ = b2;
        mouse_acc_x_ = 0;
        mouse_acc_y_ = 0;
    }
    accel_ = !digital ? 0 : accel_ < 7 ? accel_ + 1 : accel_;
}

void PointingDevice::absolute_packet() {
    const int x = std::clamp(input_.abs_x, 0, 0x3ff), y = std::clamp(input_.abs_y, 0, 0x3ff);
    const bool b1 = input_.button1, b2 = input_.button2, in_area = input_.in_area;
    const bool moved = in_area && (x != x_q_ || y != y_q_);
    if ((initial_packet_ && in_area) || b1 != b1_q_ || b2 != b2_q_ || in_area != in_area_q_ || moved) {
        packet_[0] = uint8_t(0x40 | (b1 ? 0x20 : 0) | (b2 ? 0x10 : 0) | (x >> 6 & 0x0f));
        packet_[1] = uint8_t((in_area ? 0x20 : 0) | (y >> 6 & 0x0f));
        packet_[2] = uint8_t(x & 0x3f);
        packet_[3] = uint8_t(y & 0x3f);
        packet_len_ = 4;
        x_q_ = x;
        y_q_ = y;
        b1_q_ = b1;
        b2_q_ = b2;
        in_area_q_ = in_area;
    }
}

void PointingDevice::prepare_packet() {
    packet_len_ = 0;
    if (absolute()) {
        absolute_packet();
    } else {
        relative_packet();
    }
    initial_packet_ = false;
    if (packet_len_ == 0) return;
    packet_pos_ = 0;
    state_ = kSending;
}

int PointingDevice::tick(int cpu_cycles, bool rts_negated) {
    if (rts_negated) {
        state_ = kHeld;
        return -1;
    }
    if (state_ == kHeld) {
        state_ = kSendId;
        cycles_to_next_ = kCyclesPerByte;
    }
    cycles_to_next_ -= cpu_cycles;
    if (cycles_to_next_ > 0) return -1;
    cycles_to_next_ += kCyclesPerByte;

    if (state_ == kSendId) {
        state_ = kIdle;
        initial_packet_ = true;
        return uint8_t(id_) | 0x80;
    }
    if (state_ == kIdle) prepare_packet();
    if (state_ != kSending) return -1;
    const uint8_t b = packet_[packet_pos_++];
    if (packet_pos_ == packet_len_) state_ = kIdle;
    return b | 0x80;
}

} // namespace zcdi

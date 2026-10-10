#include "serial_pointer.h"

#include <algorithm>
#include <cstdlib>

#include "scc68070/scc68070.h"

namespace zcdi {
namespace {

// 1200 baud, 1 start + 8 data + 1 stop bit: 120 bytes a second.
constexpr int kCyclesPerByte = mamecdi::kCpuClockHz / 120;

int saturate(int v) { return std::clamp(v, -127, 127); }

} // namespace

void SerialPointer::set_connected(bool connected) {
    if (connected && !connected_) {
        state_ = kDeviceId;
        cycles_to_next_ = kCyclesPerByte;
    }
    connected_ = connected;
}

void SerialPointer::set_input(const Input& input) {
    input_ = input;
    if (std::abs(input.mouse_dx) > 2 || std::abs(input.mouse_dy) > 2) significant_mouse_movement_ = true;
    if (mode_ == kMouse) {
        mouse_acc_x_ += input.mouse_dx;
        mouse_acc_y_ += input.mouse_dy;
    }
}

// Once per byte time while idle, as the RTL decides before each byte slot.
void SerialPointer::prepare_report() {
    const bool digital = input_.left || input_.right || input_.up || input_.down;
    const bool analog = input_.analog_dx != 0 || input_.analog_dy != 0;
    if (significant_mouse_movement_ && !digital && !analog && mode_ != kMouse) {
        mode_ = kMouse;
        state_ = kDeviceId;
    } else if ((digital || analog) && mode_ == kMouse) {
        mode_ = kPad;
        state_ = kDeviceId;
    } else {
        int x = 0, y = 0;
        if (mode_ == kMouse) {
            x = saturate(mouse_acc_x_);
            y = saturate(mouse_acc_y_);
        } else if (analog) {
            x = saturate(input_.analog_dx);
            y = saturate(input_.analog_dy);
        } else {
            const int speed = gamepad_speed_n_step(accel_ + 1);
            if (input_.right) x = speed;
            if (input_.down) y = speed;
            if (input_.left) x = -speed;
            if (input_.up) y = -speed;
        }
        const bool b1 = input_.button1, b2 = input_.button2;
        // A maneuvering device keeps reporting while held.
        if (b1 != b1_q_ || b2 != b2_q_ || x != x_q_ || y != y_q_ || x != 0 || y != 0) {
            x_q_ = x;
            y_q_ = y;
            b1_q_ = b1;
            b2_q_ = b2;
            const uint8_t ux = uint8_t(x), uy = uint8_t(y);
            frame_[0] = uint8_t(0xc0 | (b1 ? 0x20 : 0) | (b2 ? 0x10 : 0) | (uy >> 6 & 3) << 2 | (ux >> 6 & 3));
            frame_[1] = uint8_t(0x80 | (ux & 0x3f));
            frame_[2] = uint8_t(0x80 | (uy & 0x3f));
            state_ = kByte0;
            mouse_acc_x_ = 0;
            mouse_acc_y_ = 0;
        }
    }
    significant_mouse_movement_ = false;
    accel_ = !digital ? 0 : accel_ < 7 ? accel_ + 1 : accel_;
}

int SerialPointer::tick(int cpu_cycles, bool rts) {
    if (!connected_) return -1;
    if (rts) {
        state_ = kDeviceId;
        cycles_to_next_ = kCyclesPerByte;
        return -1;
    }
    cycles_to_next_ -= cpu_cycles;
    if (cycles_to_next_ > 0) return -1;
    cycles_to_next_ += kCyclesPerByte;

    if (state_ == kIdle) prepare_report();
    switch (state_) {
        case kDeviceId: state_ = kIdle; return mode_ | 0x80;
        case kByte0: state_ = kByte1; return frame_[0];
        case kByte1: state_ = kByte2; return frame_[1];
        case kByte2: state_ = kIdle; return frame_[2];
        default: return -1;
    }
}

} // namespace zcdi

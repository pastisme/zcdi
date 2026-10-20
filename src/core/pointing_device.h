#pragma once
#include <cstdint>

#include "core/state_archive.h"

namespace zcdi {

// Philips "Technical Info Pointing Devices" (1990).
enum class DeviceId : uint8_t {
    kRelative = 'M', // mouse, trackball
    kManeuvering = 'J', // gamepad, joystick
    kAbsoluteCoordinate = 'T', // tablet
    kAbsoluteScreen = 'S', // touch screen, light pen
};

// CDi_MiSTer rtl/pointing_device.sv.
struct PointingDeviceSaveState {
    enum State : uint8_t { kHeld, kSendId, kIdle, kSending };

    State state_ = kSendId;
    int cycles_to_next_ = 0;
    uint8_t packet_[4] = {};
    uint8_t packet_len_ = 0;
    uint8_t packet_pos_ = 0;
    bool initial_packet_ = true;
    // Last packet sent.
    bool b1_q_ = false, b2_q_ = false;
    int x_q_ = 0, y_q_ = 0;
    bool in_area_q_ = false;
    int accel_ = 0; // 0-7
    int mouse_acc_x_ = 0, mouse_acc_y_ = 0;
};

class PointingDevice : private PointingDeviceSaveState {
public:
    struct Input {
        bool left = false, right = false, up = false, down = false;
        int analog_dx = 0, analog_dy = 0; // per packet
        int mouse_dx = 0, mouse_dy = 0; // since the last frame
        bool button1 = false, button2 = false;
        // Absolute devices, 10-bit.
        int abs_x = 0, abs_y = 0;
        bool in_area = false;
    };

    explicit PointingDevice(DeviceId id);

    DeviceId id() const { return id_; }
    bool absolute() const { return id_ == DeviceId::kAbsoluteCoordinate || id_ == DeviceId::kAbsoluteScreen; }

    void set_input(const Input& input);

    // The byte finished in this span, with bit 7 set (stop bit), or -1.
    int tick(int cpu_cycles, bool rts_negated);

    // Speed N (pointing_device.sv speed_lut); `held` counts packets from 1.
    static int gamepad_speed_n_step(int held) {
        int step = held + (held & 1);
        return step < 8 ? step : 8;
    }

    template <class T>
    void state_io(T& archive);

private:
    void prepare_packet();
    void relative_packet();
    void absolute_packet();

    DeviceId id_;
    Input input_{};
};

} // namespace zcdi

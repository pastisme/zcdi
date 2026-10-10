#pragma once
#include <cstdint>

#include "core/state_archive.h"

namespace zcdi {

// The back-socket pointer on the SCC68070 UART (/pt2), after CDi_MiSTer's
// rtl/pointing_device.sv: 1200 baud, ID byte then 3-byte reports.
struct SerialPointerSaveState {
    enum State : uint8_t { kDeviceId, kIdle, kByte0, kByte1, kByte2 };
    // The device ID byte the device sends after reset.
    enum Mode : uint8_t { kPad = 'J', kMouse = 'M' };

    Mode mode_ = kPad;
    State state_ = kDeviceId;
    int cycles_to_next_ = 0;
    uint8_t frame_[3] = {};
    bool b1_q_ = false, b2_q_ = false;
    int x_q_ = 0, y_q_ = 0;
    int accel_ = 0;
    int mouse_acc_x_ = 0, mouse_acc_y_ = 0;
    bool significant_mouse_movement_ = false;
};

class SerialPointer : private SerialPointerSaveState {
public:
    // Set by the frontend once per frame (Bus::set_pointer2_input).
    struct Input {
        bool left = false, right = false, up = false, down = false;
        int analog_dx = 0, analog_dy = 0; // stick, already a per-report step
        int mouse_dx = 0, mouse_dy = 0; // motion since the last frame
        bool button1 = false, button2 = false;
    };

    // Frontend config, not machine state.
    void set_connected(bool connected);

    void set_input(const Input& input);

    // The wired remote's speed-N D-Pad ramp (2 2 4 4 6 6 8 ..., pointing_device.sv
    // speed_lut). `held_frames` counts frames held, from 1.
    static int gamepad_speed_n_step(int held_frames) {
        int step = held_frames + (held_frames & 1);
        return step < 8 ? step : 8;
    }

    // Returns the byte finished in this span, or -1. `rts` holds it in
    // reset. Chunks must be well under a byte time (one byte per call).
    int tick(int cpu_cycles, bool rts);

    template <class T>
    void state_io(T& archive) {
        archive.begin_section(state_tag("PTR2"));
        archive.io_struct(static_cast<SerialPointerSaveState&>(*this));
        archive.end_section();
    }

private:
    void prepare_report();

    bool connected_ = false;
    Input input_{};
};

} // namespace zcdi

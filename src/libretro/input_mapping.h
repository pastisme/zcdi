#pragma once
#include "libretro/libretro.h"

#include "core/state_archive.h"

namespace zcdi {

class Machine;

void set_input_descriptors(retro_environment_t env);

// Call from retro_load_game().
void set_controller_info(retro_environment_t env);

// Call from retro_init(): read each pad in one call if the frontend can.
void query_input_bitmasks(retro_environment_t env);

// A port's joypad buttons as a RETRO_DEVICE_ID_JOYPAD bit mask.
uint16_t joypad_buttons(retro_input_state_t input_state_cb, unsigned port);

// From retro_set_controller_port_device(). Port 1 starts unplugged.
void set_port_device(unsigned port, unsigned device);

// Left-stick reading to a per-report pointer step.
void analog_stick_delta(int16_t raw_x, int16_t raw_y, int& out_dx, int& out_dy);

// Call once per retro_run(), after input poll. No-op if input_state_cb is null.
void poll_pointer_input(retro_input_state_t input_state_cb, Machine& machine);

struct InputHistory {
    int dpad_held_frames = 0;
    bool prev_button1 = false;
    bool prev_button2 = false;
    bool prev_button12 = false;
    int prev_gun_x = -1;
    int prev_gun_y = -1;

    template <class T>
    void state_io(T& archive) {
        archive.begin_section(state_tag("INPT"));
        archive.io_struct(*this);
        archive.end_section();
    }
};

InputHistory& input_history();

} // namespace zcdi

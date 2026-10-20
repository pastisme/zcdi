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

// From retro_set_controller_port_device(): port 0 front socket, port 1 back.
void set_port_device(unsigned port, unsigned device);

// Left-stick reading to a per-report pointer step.
void analog_stick_delta(int16_t raw_x, int16_t raw_y, int& out_dx, int& out_dy);

// Device space (0-767, 0-559) to the 10-bit coordinate the SLAVE scales to it.
int absolute_x_for(int device_x);
int absolute_y_for(int device_y);

// Call once per retro_run(), after input poll. No-op if input_state_cb is null.
void poll_pointer_input(retro_input_state_t input_state_cb, Machine& machine);

struct InputHistory {
    // Lightgun's last aim, 10-bit; -1 before it has one.
    int gun_x = -1;
    int gun_y = -1;

    template <class T>
    void state_io(T& archive) {
        archive.begin_section(state_tag("INPT"));
        archive.io_struct(*this);
        archive.end_section();
    }
};

InputHistory& input_history();

} // namespace zcdi

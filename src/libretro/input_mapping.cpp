#include "input_mapping.h"

#include <cmath>

#include "core/machine.h"
#include "core/serial_pointer.h"

namespace zcdi {

// Not a real CD-i input; the speed range matches curve II's ceiling
// (SerialPointer::gamepad_speed_n_step). Thresholds are on the magnitude, so
// diagonals aren't faster and the dead zone is round.
constexpr double kAnalogDeadzone = 4096.0; // 12.5% of full scale
constexpr double kAnalogSaturate = 29491.0; // ~90%: real sticks rarely reach 0x7fff
constexpr double kAnalogMinSpeed = 2.0;
constexpr double kAnalogMaxSpeed = 16.0;
// A manoeuvring device gives at least 16 directions (Green Book A VII.2.5.1.1 d),
// so within half a step of an axis the other axis is dropped: some titles take
// any nonzero step as a direction.
constexpr double kAnalogAxisSnap = 0.19891237; // tan(11.25 degrees)

void analog_stick_delta(int16_t raw_x, int16_t raw_y, int& out_dx, int& out_dy) {
    double x = raw_x, y = raw_y;
    double mag = std::hypot(x, y);
    if (mag <= kAnalogDeadzone) {
        out_dx = 0;
        out_dy = 0;
        return;
    }
    double clamped_mag = mag < kAnalogSaturate ? mag : kAnalogSaturate;
    double t = (clamped_mag - kAnalogDeadzone) / (kAnalogSaturate - kAnalogDeadzone);
    double speed = kAnalogMinSpeed + t * (kAnalogMaxSpeed - kAnalogMinSpeed);
    if (std::abs(y) < std::abs(x) * kAnalogAxisSnap) y = 0;
    if (std::abs(x) < std::abs(y) * kAnalogAxisSnap) x = 0;
    double dir = std::hypot(x, y);
    out_dx = int(std::lround((x / dir) * speed));
    out_dy = int(std::lround((y / dir) * speed));
}

namespace {

InputHistory g_history;
bool g_input_bitmasks = false;

unsigned g_port0_device = RETRO_DEVICE_JOYPAD;
unsigned g_port1_device = RETRO_DEVICE_NONE;

// SLAVE device space; CD-RTOS draws device (x, y) at frame (x/2, y/2), so a
// linear map of the gun's screen range onto it lands on the cursor tip.
constexpr int kDeviceWidth = 768;
constexpr int kDeviceHeight = 560;

int lightgun_to_device(int16_t screen, int device_extent) {
    // -0x8000 (some frontends' "no reading") lands on 0 like the edge.
    long v = long(screen) + 0x7fff;
    if (v < 0) v = 0;
    long pos = v * device_extent / 0xfffe;
    return int(pos < device_extent ? pos : device_extent - 1);
}

void left_stick_delta(retro_input_state_t input_state_cb, unsigned port, int& out_dx, int& out_dy) {
    const int16_t x =
        input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
    const int16_t y =
        input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
    analog_stick_delta(x, y, out_dx, out_dy);
}

void poll_lightgun_input(retro_input_state_t input_state_cb, unsigned port, Machine& machine) {
    auto gun = [&](unsigned id) { return input_state_cb(port, RETRO_DEVICE_LIGHTGUN, 0, id); };
    bool offscreen = gun(RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN) != 0;
    // Off screen: the pointer stays, the buttons still work.
    int x = g_history.prev_gun_x, y = g_history.prev_gun_y;
    if (!offscreen) {
        x = lightgun_to_device(gun(RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X), kDeviceWidth);
        y = lightgun_to_device(gun(RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y), kDeviceHeight);
    }
    bool button1 = gun(RETRO_DEVICE_ID_LIGHTGUN_TRIGGER) != 0;
    bool button2 = gun(RETRO_DEVICE_ID_LIGHTGUN_AUX_A) != 0;

    if (x >= 0) {
        // Every frame: CD-RTOS moves the pointer itself (Set Pointer Position).
        machine.bus().set_pointer(x, y, button1, button2, false);
    } else if (button1 != g_history.prev_button1 || button2 != g_history.prev_button2 || g_history.prev_button12) {
        // Never aimed at the screen yet: keep CD-RTOS's own position.
        machine.bus().update_pointer(0, 0, button1, button2, false);
    }
    g_history.prev_gun_x = x;
    g_history.prev_gun_y = y;
    g_history.prev_button1 = button1;
    g_history.prev_button2 = button2;
    g_history.prev_button12 = false;
}

// The back socket. Y is both buttons held: the serial report has no combo.
void poll_serial_input(retro_input_state_t input_state_cb, unsigned port, Machine& machine) {
    const uint16_t buttons = joypad_buttons(input_state_cb, port);
    auto pad = [&](unsigned id) { return bool((buttons >> id) & 1); };
    auto mouse = [&](unsigned id) { return input_state_cb(port, RETRO_DEVICE_MOUSE, 0, id); };
    int analog_dx = 0, analog_dy = 0;
    left_stick_delta(input_state_cb, port, analog_dx, analog_dy);
    const bool both = pad(RETRO_DEVICE_ID_JOYPAD_Y) || mouse(RETRO_DEVICE_ID_MOUSE_MIDDLE);
    machine.bus().set_pointer2_input({
        .left = pad(RETRO_DEVICE_ID_JOYPAD_LEFT),
        .right = pad(RETRO_DEVICE_ID_JOYPAD_RIGHT),
        .up = pad(RETRO_DEVICE_ID_JOYPAD_UP),
        .down = pad(RETRO_DEVICE_ID_JOYPAD_DOWN),
        .analog_dx = analog_dx,
        .analog_dy = analog_dy,
        .mouse_dx = mouse(RETRO_DEVICE_ID_MOUSE_X),
        .mouse_dy = mouse(RETRO_DEVICE_ID_MOUSE_Y),
        .button1 =
            both || pad(RETRO_DEVICE_ID_JOYPAD_B) || pad(RETRO_DEVICE_ID_JOYPAD_X) || mouse(RETRO_DEVICE_ID_MOUSE_LEFT),
        .button2 = both || pad(RETRO_DEVICE_ID_JOYPAD_A) || mouse(RETRO_DEVICE_ID_MOUSE_RIGHT),
    });
}

void poll_slave_input(retro_input_state_t input_state_cb, unsigned port, Machine& machine) {
    if (port == 0 && (g_port0_device & RETRO_DEVICE_MASK) == RETRO_DEVICE_LIGHTGUN) {
        poll_lightgun_input(input_state_cb, port, machine);
        return;
    }

    int dx = 0, dy = 0;
    const uint16_t buttons = joypad_buttons(input_state_cb, port);
    auto pad = [&](unsigned id) { return bool((buttons >> id) & 1); };
    auto mouse = [&](unsigned id) { return input_state_cb(port, RETRO_DEVICE_MOUSE, 0, id); };
    bool dpad_left = pad(RETRO_DEVICE_ID_JOYPAD_LEFT);
    bool dpad_right = pad(RETRO_DEVICE_ID_JOYPAD_RIGHT);
    bool dpad_up = pad(RETRO_DEVICE_ID_JOYPAD_UP);
    bool dpad_down = pad(RETRO_DEVICE_ID_JOYPAD_DOWN);
    g_history.dpad_held_frames = (dpad_left || dpad_right || dpad_up || dpad_down) ? g_history.dpad_held_frames + 1 : 0;
    int step = SerialPointer::gamepad_speed_n_step(g_history.dpad_held_frames);
    if (dpad_left) dx -= step;
    if (dpad_right) dx += step;
    if (dpad_up) dy -= step;
    if (dpad_down) dy += step;
    dx += mouse(RETRO_DEVICE_ID_MOUSE_X);
    dy += mouse(RETRO_DEVICE_ID_MOUSE_Y);

    int analog_dx = 0, analog_dy = 0;
    left_stick_delta(input_state_cb, port, analog_dx, analog_dy);
    dx += analog_dx;
    dy += analog_dy;

    bool button1 = pad(RETRO_DEVICE_ID_JOYPAD_B) || pad(RETRO_DEVICE_ID_JOYPAD_X) || mouse(RETRO_DEVICE_ID_MOUSE_LEFT);
    bool button2 = pad(RETRO_DEVICE_ID_JOYPAD_A) || mouse(RETRO_DEVICE_ID_MOUSE_RIGHT);
    bool button12 = pad(RETRO_DEVICE_ID_JOYPAD_Y) || mouse(RETRO_DEVICE_ID_MOUSE_MIDDLE);

    if (dx != 0 || dy != 0 || button1 != g_history.prev_button1 || button2 != g_history.prev_button2 ||
        button12 != g_history.prev_button12) {
        machine.bus().update_pointer(dx, dy, button1, button2, button12);
    }
    g_history.prev_button1 = button1;
    g_history.prev_button2 = button2;
    g_history.prev_button12 = button12;
}

} // namespace

void query_input_bitmasks(retro_environment_t env) {
    g_input_bitmasks = env && env(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, nullptr);
}

uint16_t joypad_buttons(retro_input_state_t input_state_cb, unsigned port) {
    if (g_input_bitmasks) return uint16_t(input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK));
    // Without bitmasks, only the buttons zcdi reads.
    static constexpr unsigned kIds[] = {
        RETRO_DEVICE_ID_JOYPAD_B,    RETRO_DEVICE_ID_JOYPAD_Y,    RETRO_DEVICE_ID_JOYPAD_UP,
        RETRO_DEVICE_ID_JOYPAD_DOWN, RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_RIGHT,
        RETRO_DEVICE_ID_JOYPAD_A,    RETRO_DEVICE_ID_JOYPAD_X,
    };
    uint16_t mask = 0;
    for (unsigned id : kIds) {
        if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, id)) mask |= uint16_t(1u << id);
    }
    return mask;
}

void set_controller_info(retro_environment_t env) {
    static const struct retro_controller_description port0_types[] = {
        {"RetroPad / Mouse", RETRO_DEVICE_JOYPAD},
        {"Lightgun", RETRO_DEVICE_LIGHTGUN},
    };
    static const struct retro_controller_description port1_types[] = {
        {"RetroPad / Mouse", RETRO_DEVICE_JOYPAD},
        {"None", RETRO_DEVICE_NONE},
    };
    static const struct retro_controller_info ports[] = {
        {port0_types, 2},
        {port1_types, 2},
        {nullptr, 0},
    };
    env(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, const_cast<struct retro_controller_info*>(ports));
}

void set_port_device(unsigned port, unsigned device) {
    if (port == 1) g_port1_device = device;
    if (port != 0) return;
    g_port0_device = device;
    // A fresh device starts from CD-RTOS's current pointer position again.
    g_history.prev_gun_x = -1;
    g_history.prev_gun_y = -1;
}

void set_input_descriptors(retro_environment_t env) {
    static const struct retro_input_descriptor desc[] = {
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "D-Pad Left"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "D-Pad Up"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "D-Pad Down"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Button 2"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Button 1"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "Button 1"},
        {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Button 1+2"},
        {0, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER, "Button 1"},
        {0, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_AUX_A, "Button 2"},
        {1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "D-Pad Left"},
        {1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right"},
        {1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "D-Pad Up"},
        {1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "D-Pad Down"},
        {1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Button 2"},
        {1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Button 1"},
        {1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "Button 1"},
        {1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Button 1+2"},
        {0, 0, 0, 0, nullptr},
    };
    env(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, const_cast<struct retro_input_descriptor*>(desc));
}

// As CDi_MiSTer "P1 Back + P2 Front" with two players; a lightgun stays in
// front, the only socket that takes one.
void poll_pointer_input(retro_input_state_t input_state_cb, Machine& machine) {
    if (!input_state_cb) return;
    const bool two_players = (g_port1_device & RETRO_DEVICE_MASK) == RETRO_DEVICE_JOYPAD;
    machine.bus().set_pointer2_connected(two_players);
    if (!two_players) {
        poll_slave_input(input_state_cb, 0, machine);
        return;
    }
    const bool port0_front = (g_port0_device & RETRO_DEVICE_MASK) == RETRO_DEVICE_LIGHTGUN;
    poll_slave_input(input_state_cb, port0_front ? 0 : 1, machine);
    poll_serial_input(input_state_cb, port0_front ? 1 : 0, machine);
}

InputHistory& input_history() { return g_history; }

} // namespace zcdi

#include "input_mapping.h"

#include <cmath>
#include <optional>

#include "core/machine.h"

namespace zcdi {

// Not a real CD-i input; the speed range matches curve II's ceiling
// (PointingDevice::gamepad_speed_n_step). Thresholds are on the magnitude, so
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

// RetroArch defaults every port to a plain RetroPad.
constexpr unsigned kSecondGamepad = RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 0);
constexpr unsigned kSecondMouse = RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_MOUSE, 0);
// The Peacekeeper's real ID is undocumented.
constexpr DeviceId kLightgunId = DeviceId::kAbsoluteScreen;

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

// SLAVE firmware 0x69c, 0x6c5.
int slave_scaled_x(int x) { return (x >> 1) + (x >> 2); }
int slave_scaled_y(int y) { return (y >> 1) + (y >> 4) + (y >> 5); }

template <class Scale>
int unscale(int target, Scale scale) {
    int lo = 0, hi = 0x3ff;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (scale(mid) < target) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

std::optional<DeviceId> device_for_port(unsigned port, unsigned device) {
    if (port == 0) {
        switch (device & RETRO_DEVICE_MASK) {
            case RETRO_DEVICE_MOUSE: return DeviceId::kRelative;
            case RETRO_DEVICE_LIGHTGUN: return kLightgunId;
            case RETRO_DEVICE_NONE: return std::nullopt;
            default: return DeviceId::kManeuvering;
        }
    }
    if (device == kSecondGamepad) return DeviceId::kManeuvering;
    if (device == kSecondMouse) return DeviceId::kRelative;
    return std::nullopt;
}

void left_stick_delta(retro_input_state_t input_state_cb, unsigned port, int& out_dx, int& out_dy) {
    const int16_t x =
        input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
    const int16_t y =
        input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
    analog_stick_delta(x, y, out_dx, out_dy);
}

PointingDevice::Input lightgun_input(retro_input_state_t input_state_cb, unsigned port) {
    auto gun = [&](unsigned id) { return input_state_cb(port, RETRO_DEVICE_LIGHTGUN, 0, id); };
    const bool offscreen = gun(RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN) != 0;
    if (!offscreen) {
        g_history.gun_x = absolute_x_for(lightgun_to_device(gun(RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X), kDeviceWidth));
        g_history.gun_y = absolute_y_for(lightgun_to_device(gun(RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y), kDeviceHeight));
    }
    return {
        .button1 = gun(RETRO_DEVICE_ID_LIGHTGUN_TRIGGER) != 0,
        .button2 = gun(RETRO_DEVICE_ID_LIGHTGUN_AUX_A) != 0,
        .abs_x = g_history.gun_x < 0 ? 0 : g_history.gun_x,
        .abs_y = g_history.gun_y < 0 ? 0 : g_history.gun_y,
        .in_area = !offscreen && g_history.gun_x >= 0,
    };
}

// Y is both buttons.
PointingDevice::Input relative_input(retro_input_state_t input_state_cb, unsigned port) {
    const uint16_t buttons = joypad_buttons(input_state_cb, port);
    auto pad = [&](unsigned id) { return bool((buttons >> id) & 1); };
    auto mouse = [&](unsigned id) { return input_state_cb(port, RETRO_DEVICE_MOUSE, 0, id); };
    int analog_dx = 0, analog_dy = 0;
    left_stick_delta(input_state_cb, port, analog_dx, analog_dy);
    const bool both = pad(RETRO_DEVICE_ID_JOYPAD_Y) || mouse(RETRO_DEVICE_ID_MOUSE_MIDDLE);
    return {
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
    };
}

void poll_socket(retro_input_state_t input_state_cb, unsigned port, unsigned device, Bus::Socket socket,
                 Machine& machine) {
    const std::optional<DeviceId> id = device_for_port(port, device);
    machine.bus().plug(socket, id);
    if (!id) return;
    const bool absolute = *id == DeviceId::kAbsoluteCoordinate || *id == DeviceId::kAbsoluteScreen;
    machine.bus().set_socket_input(
        socket, absolute ? lightgun_input(input_state_cb, port) : relative_input(input_state_cb, port));
}

} // namespace

int absolute_x_for(int device_x) { return unscale(device_x, slave_scaled_x); }
int absolute_y_for(int device_y) { return unscale(device_y, slave_scaled_y); }

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
        {"Gamepad", RETRO_DEVICE_JOYPAD},
        {"Mouse", RETRO_DEVICE_MOUSE},
        {"Lightgun", RETRO_DEVICE_LIGHTGUN},
    };
    static const struct retro_controller_description port1_types[] = {
        {"None", RETRO_DEVICE_NONE},
        {"Gamepad (2nd player)", kSecondGamepad},
        {"Mouse (2nd player)", kSecondMouse},
    };
    static const struct retro_controller_info ports[] = {
        {port0_types, 3},
        {port1_types, 3},
        {nullptr, 0},
    };
    env(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, const_cast<struct retro_controller_info*>(ports));
}

void set_port_device(unsigned port, unsigned device) {
    if (port == 1) g_port1_device = device;
    if (port != 0) return;
    g_port0_device = device;
    g_history.gun_x = -1;
    g_history.gun_y = -1;
}

void plug_port_devices(Machine& machine) {
    machine.bus().plug(Bus::Socket::kFront, device_for_port(0, g_port0_device));
    machine.bus().plug(Bus::Socket::kBack, device_for_port(1, g_port1_device));
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

void poll_pointer_input(retro_input_state_t input_state_cb, Machine& machine) {
    if (!input_state_cb) return;
    poll_socket(input_state_cb, 0, g_port0_device, Bus::Socket::kFront, machine);
    poll_socket(input_state_cb, 1, g_port1_device, Bus::Socket::kBack, machine);
}

InputHistory& input_history() { return g_history; }

} // namespace zcdi

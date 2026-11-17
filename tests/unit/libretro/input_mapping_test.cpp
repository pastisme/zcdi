#include "doctest.h"

#include <cstdint>
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "libretro/input_mapping.h"
#include "support/fake_frontend.h"
#include "support/fake_machine.h"
#include "support/machine_board.h"
#include "support/state_roundtrip.h"

using zcdi::analog_stick_delta;
using zcdi::DeviceId;
using zcdi::Socket;
using zcdi_test::FakeFrontend;
using zcdi_test::FakeInput;
using zcdi_test::socket_input;

TEST_SUITE("libretro") {

TEST_CASE("a stick inside the dead zone doesn't move the pointer") {
    int dx = 5, dy = 5;
    analog_stick_delta(3000, -2000, dx, dy);
    CHECK(dx == 0);
    CHECK(dy == 0);
}

TEST_CASE("a stick near an axis moves along that axis only") {
    int dx = 0, dy = 0;
    analog_stick_delta(32000, 2600, dx, dy);  // ~5 degrees of drift
    CHECK(dx == 16);
    CHECK(dy == 0);
    analog_stick_delta(-5000, -30000, dx, dy);  // ~9.5 degrees
    CHECK(dx == 0);
    CHECK(dy < 0);
}

TEST_CASE("a stick away from the axes keeps its angle") {
    int dx = 0, dy = 0;
    analog_stick_delta(20000, -20000, dx, dy);
    CHECK(dx > 0);
    CHECK(dx == -dy);
    analog_stick_delta(30000, 8000, dx, dy);  // ~15 degrees
    CHECK(dy > 0);
    CHECK(dx > dy);
}

TEST_CASE("full tilt is the same speed in every direction") {
    for (auto [x, y] : {std::pair{32767, 0}, std::pair{0, -32768}, std::pair{23170, 23170}}) {
        CAPTURE(x);
        CAPTURE(y);
        int dx = 0, dy = 0;
        analog_stick_delta(int16_t(x), int16_t(y), dx, dy);
        CHECK(std::abs(dx) + std::abs(dy) <= 23);
        CHECK(std::max(std::abs(dx), std::abs(dy)) >= 11);
    }
}

TEST_CASE("joypad buttons read the same with and without frontend bitmasks") {
    // B and Left held on port 1, nothing on port 0.
    const uint16_t held = (1 << RETRO_DEVICE_ID_JOYPAD_B) | (1 << RETRO_DEVICE_ID_JOYPAD_LEFT);
    FakeInput input;
    input.set(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK, int16_t(held));
    input.set(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, 1);
    input.set(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, 1);
    for (bool bitmasks : {true, false}) {
        FakeFrontend frontend;
        frontend.input_bitmasks = bitmasks;
        zcdi::query_input_bitmasks(FakeFrontend::env);
        CHECK(zcdi::joypad_buttons(FakeInput::state, 1) == held);
        CHECK(zcdi::joypad_buttons(FakeInput::state, 0) == 0);
    }
    zcdi::query_input_bitmasks(nullptr);
}

TEST_CASE("the ports' devices are plugged before the first frame, so a state loaded then restores them") {
    zcdi::set_port_device(0, RETRO_DEVICE_JOYPAD);
    zcdi::set_port_device(1, RETRO_DEVICE_NONE);
    std::vector<uint8_t> state;
    {
        std::unique_ptr<zcdi::Machine> m = zcdi_test::build_fake_machine();
        zcdi::plug_port_devices(*m);
        CHECK(m->plugged(zcdi::Socket::kFront) == zcdi::DeviceId::kManeuvering);
        CHECK(!m->plugged(zcdi::Socket::kBack));
        for (int i = 0; i < 20; ++i) m->run_frame();  // past the pad's power-on delay
        state = zcdi_test::save_machine(*m);
    }
    // A new session loading the state before its first frame.
    std::unique_ptr<zcdi::Machine> m = zcdi_test::build_fake_machine();
    zcdi::plug_port_devices(*m);
    REQUIRE(zcdi_test::load_machine(*m, state));
    CHECK(zcdi_test::first_difference(zcdi_test::save_machine(*m), state) == -1);
}

namespace {

// The ports' devices and the input globals are back to their power-on values afterwards.
struct Ports {
    Ports(unsigned port0, unsigned port1) {
        zcdi::set_port_device(0, port0);
        zcdi::set_port_device(1, port1);
    }
    ~Ports() {
        zcdi::set_port_device(0, RETRO_DEVICE_JOYPAD);
        zcdi::set_port_device(1, RETRO_DEVICE_NONE);
        zcdi::query_input_bitmasks(nullptr);
    }
};

// SLAVE firmware 0x69c and 0x6c5: a 10-bit coordinate to device space.
int slave_scaled_x(int x) { return (x >> 1) + (x >> 2); }
int slave_scaled_y(int y) { return (y >> 1) + (y >> 4) + (y >> 5); }

// One frontend poll on a fresh machine; the socket's input after it.
zcdi::PointingDevice::Input polled(zcdi::Machine& m, Socket socket) {
    zcdi::poll_pointer_input(FakeInput::state, m);
    const zcdi::PointingDevice::Input* input = socket_input(m, socket);
    REQUIRE(input);
    return *input;
}

}  // namespace

TEST_CASE("each device the frontend can pick plugs its socket, and only 2nd-player devices plug the back") {
    FakeFrontend frontend;
    zcdi::set_controller_info(FakeFrontend::env);
    REQUIRE(frontend.controller_info.size() == 2);
    const DeviceId front[] = {DeviceId::kManeuvering, DeviceId::kRelative, DeviceId::kAbsoluteScreen};
    const std::optional<DeviceId> back[] = {std::nullopt, DeviceId::kManeuvering, DeviceId::kRelative};
    REQUIRE(frontend.controller_info[0].num_types == 3);
    REQUIRE(frontend.controller_info[1].num_types == 3);

    std::unique_ptr<zcdi::Machine> m = zcdi_test::build_fake_machine();
    for (unsigned i = 0; i < 3; ++i) {
        const retro_controller_description& f = frontend.controller_info[0].types[i];
        const retro_controller_description& b = frontend.controller_info[1].types[i];
        CAPTURE(f.desc);
        CAPTURE(b.desc);
        Ports ports(f.id, b.id);
        zcdi::plug_port_devices(*m);
        CHECK(m->plugged(Socket::kFront) == front[i]);
        CHECK(m->plugged(Socket::kBack) == back[i]);
    }
    // A plain RetroPad on port 1 is RetroArch's default, not a second player.
    Ports ports(RETRO_DEVICE_NONE, RETRO_DEVICE_JOYPAD);
    zcdi::plug_port_devices(*m);
    CHECK(!m->plugged(Socket::kFront));
    CHECK(!m->plugged(Socket::kBack));
}

TEST_CASE("both ports describe the same buttons") {
    FakeFrontend frontend;
    zcdi::set_input_descriptors(FakeFrontend::env);
    REQUIRE_FALSE(frontend.input_descriptors.empty());
    int joypad[2] = {};
    for (const retro_input_descriptor& d : frontend.input_descriptors) {
        CAPTURE(d.description);
        REQUIRE(d.port < 2);
        if (d.device != RETRO_DEVICE_JOYPAD) continue;
        ++joypad[d.port];
        const auto other = std::find_if(frontend.input_descriptors.begin(), frontend.input_descriptors.end(),
                                        [&](const retro_input_descriptor& o) {
                                            return o.port != d.port && o.device == d.device && o.id == d.id;
                                        });
        REQUIRE(other != frontend.input_descriptors.end());
        CHECK(std::string(other->description) == d.description);
    }
    CHECK(joypad[0] == 8);
    CHECK(joypad[1] == joypad[0]);
}

TEST_CASE("without an input callback the sockets are left alone") {
    Ports ports(RETRO_DEVICE_MOUSE, RETRO_DEVICE_NONE);
    std::unique_ptr<zcdi::Machine> m = zcdi_test::build_fake_machine();
    zcdi::plug_port_devices(*m);
    zcdi::poll_pointer_input(nullptr, *m);
    CHECK(m->plugged(Socket::kFront) == DeviceId::kRelative);
}

TEST_CASE("a gamepad's D-pad, stick and buttons reach the front socket") {
    Ports ports(RETRO_DEVICE_JOYPAD, RETRO_DEVICE_NONE);
    std::unique_ptr<zcdi::Machine> m = zcdi_test::build_fake_machine();
    FakeInput input;
    input.set(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, 1);
    input.set(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, 1);
    input.set(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, 20000);
    input.set(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, -20000);
    zcdi::PointingDevice::Input in = polled(*m, Socket::kFront);
    CHECK(in.right);
    CHECK(in.up);
    CHECK_FALSE(in.left);
    CHECK_FALSE(in.down);
    int dx = 0, dy = 0;
    analog_stick_delta(20000, -20000, dx, dy);
    CHECK(in.analog_dx == dx);
    CHECK(in.analog_dy == dy);
    CHECK_FALSE(in.button1);
    CHECK_FALSE(in.button2);

    // B or X is button 1, A button 2, Y both.
    const std::pair<unsigned, std::pair<bool, bool>> buttons[] = {
        {RETRO_DEVICE_ID_JOYPAD_B, {true, false}},
        {RETRO_DEVICE_ID_JOYPAD_X, {true, false}},
        {RETRO_DEVICE_ID_JOYPAD_A, {false, true}},
        {RETRO_DEVICE_ID_JOYPAD_Y, {true, true}},
    };
    for (const auto& [id, expected] : buttons) {
        CAPTURE(id);
        input.clear();
        input.set(0, RETRO_DEVICE_JOYPAD, 0, id, 1);
        in = polled(*m, Socket::kFront);
        CHECK(in.button1 == expected.first);
        CHECK(in.button2 == expected.second);
    }
}

TEST_CASE("a mouse's motion and buttons reach the socket it is plugged into") {
    Ports ports(RETRO_DEVICE_JOYPAD, RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_MOUSE, 0));
    std::unique_ptr<zcdi::Machine> m = zcdi_test::build_fake_machine();
    FakeInput input;
    input.set(1, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X, -7);
    input.set(1, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y, 12);
    input.set(1, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT, 1);
    zcdi::PointingDevice::Input in = polled(*m, Socket::kBack);
    CHECK(in.mouse_dx == -7);
    CHECK(in.mouse_dy == 12);
    CHECK_FALSE(in.button1);
    CHECK(in.button2);

    input.clear();
    input.set(1, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT, 1);
    in = polled(*m, Socket::kBack);
    CHECK(in.button1);
    CHECK_FALSE(in.button2);

    input.clear();
    input.set(1, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_MIDDLE, 1);
    in = polled(*m, Socket::kBack);
    CHECK(in.button1);
    CHECK(in.button2);
    CHECK(socket_input(*m, Socket::kFront));  // the gamepad, untouched by port 1
}

TEST_CASE("absolute coordinates are the smallest 10-bit values the SLAVE scales onto the device position") {
    for (int x : {0, 1, 383, 384, 500, 766}) {
        CAPTURE(x);
        const int a = zcdi::absolute_x_for(x);
        CHECK(slave_scaled_x(a) >= x);
        if (a > 0) CHECK(slave_scaled_x(a - 1) < x);
    }
    for (int y : {0, 1, 279, 280, 400, 559}) {
        CAPTURE(y);
        const int a = zcdi::absolute_y_for(y);
        CHECK(slave_scaled_y(a) >= y);
        if (a > 0) CHECK(slave_scaled_y(a - 1) < y);
    }
    CHECK(zcdi::absolute_x_for(384) == 512);  // the screen centre
}

TEST_CASE("the lightgun aims in device space and keeps its last aim off screen") {
    Ports ports(RETRO_DEVICE_LIGHTGUN, RETRO_DEVICE_NONE);
    std::unique_ptr<zcdi::Machine> m = zcdi_test::build_fake_machine();
    FakeInput input;

    // No aim yet: off screen, at the origin.
    input.set(0, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN, 1);
    zcdi::PointingDevice::Input in = polled(*m, Socket::kFront);
    CHECK_FALSE(in.in_area);
    CHECK(in.abs_x == 0);
    CHECK(in.abs_y == 0);

    // The screen centre (0, 0) is the middle of the 768x560 device space.
    input.clear();
    input.set(0, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER, 1);
    in = polled(*m, Socket::kFront);
    CHECK(in.in_area);
    CHECK(in.button1);
    CHECK_FALSE(in.button2);
    CHECK(in.abs_x == zcdi::absolute_x_for(384));
    CHECK(in.abs_y == zcdi::absolute_y_for(280));
    CHECK(zcdi::input_history().gun_x == in.abs_x);

    // Off screen again: the last aim stays, outside the area.
    input.clear();
    input.set(0, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN, 1);
    input.set(0, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_AUX_A, 1);
    const int aim_x = in.abs_x, aim_y = in.abs_y;
    in = polled(*m, Socket::kFront);
    CHECK_FALSE(in.in_area);
    CHECK(in.button2);
    CHECK(in.abs_x == aim_x);
    CHECK(in.abs_y == aim_y);

    // The screen's edges, and -0x8000 ("no reading" on some frontends), are the device's edges.
    input.clear();
    input.set(0, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X, -0x8000);
    input.set(0, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y, 0x7fff);
    in = polled(*m, Socket::kFront);
    CHECK(in.abs_x == 0);
    CHECK(in.abs_y == zcdi::absolute_y_for(559));

    // Picking the lightgun again forgets the aim.
    zcdi::set_port_device(0, RETRO_DEVICE_LIGHTGUN);
    CHECK(zcdi::input_history().gun_x == -1);
}

TEST_CASE("unplugging a port empties its socket on the next poll") {
    Ports ports(RETRO_DEVICE_JOYPAD, RETRO_DEVICE_NONE);
    std::unique_ptr<zcdi::Machine> m = zcdi_test::build_fake_machine();
    FakeInput input;
    zcdi::poll_pointer_input(FakeInput::state, *m);
    REQUIRE(m->plugged(Socket::kFront));
    zcdi::set_port_device(0, RETRO_DEVICE_NONE);
    zcdi::poll_pointer_input(FakeInput::state, *m);
    CHECK(!m->plugged(Socket::kFront));
    CHECK_FALSE(socket_input(*m, Socket::kFront));
}

}  // TEST_SUITE
